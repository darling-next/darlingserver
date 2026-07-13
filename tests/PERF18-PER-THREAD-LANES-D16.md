# perf#18 D16 (dar-1il.11): PER-THREAD ring lanes

**The P4 "full per-thread rings" P3 deferred.** P3/P4 shipped ONE process-wide ring owned by the
first-attaching TID; every OTHER thread saw a foreign owner and fell back to UDS. D15a
([PERF18-ATTACH-TIMELINE-D15a.md](PERF18-ATTACH-TIMELINE-D15a.md)) measured the cost: the reclaimable
UDS pool is the ~2053 **POST-attach** eligible calls scattered to UDS because the eligible-op traffic is
multi-thread but the ring was single-thread. D16 gives **each guest thread its own SPSC ring lane** so a
non-owner thread's eligible ops ride the ring instead of UDS.

## RESULT — the headline win (matched dylib+dyld+server A/B, same warm workload)

Both arms run the SAME workload (warm boot + 12 exec batches of multi-thread leaves + a 30-child
fork-storm) with a MATCHED dyld+dylib+server (so `attach_rejects == 0` and the post-attach pool is the
clean signal — D15a's 29% ABI-reject was a stale-dyld artifact, eliminated here by deploying the
matched-HEAD dyld).

| metric | baseline (single-owner) | D16 (per-thread) | change |
|--------|------------------------:|-----------------:|-------:|
| **post_attach_eligible_UDS** | **1533** | **231** | **−85%** |
| attach_rejects | 0 | 0 | — |
| ring_fast_fail / ring_s2c_full / ring_fast_suspend / clients_blocked | 0 | 0 | clean |

**Per-op ring uptake (D16 heatmap, ring vs UDS):** almost every eligible op is now **100% ring**:
mach_reply_port 746/0, host_self_trap 491/0, thread_self_trap 381/0, task_self_trap 372/0, uidgid 284/0,
set_thread_handles 261/0, get_tracer 116/0. Only the FIRST eligible call a process makes before its lane
attaches remains on UDS: `started_suspended` 50% ring, `vchroot_path` 67% ring. Under the single-owner
baseline these same ops were ~50% UDS across the board (the non-owner-thread share). The residual D16
UDS pool (231) is ~one-call-per-process (started_suspended 116 + vchroot_path 115) — the unavoidable
first-eligible-op-before-lane-attach, NOT a per-thread-fallback. There is no leftover non-owner-thread
scatter.

(The first D16 run was done with the STALE prod dyld still deployed — it carried ABI v4 and the server
rejected its early mldr-phase attach `reason=abi(2)`, a constant 116-reject floor in BOTH arms that
polluted the pool. Deploying the matched-HEAD dyld dropped rejects to 0 and gave the clean numbers
above. The matched-build requirement is exactly the deployment-hygiene item D15a flagged.)

## DESIGN

### The server was ALREADY per-thread — D16 is a GUEST change
The server tracks ring **threads**, not a process ring: `Server::_ringThreads`, `registerRingThread` /
`unregisterRingThread`, and the main-loop spin phase `_drainRings()` already iterates EVERY registered
ring thread and services it. `RingAttach::processCall` attaches a ring PER CALLING THREAD (keyed on
`thread->nsid()`), registers a per-thread wake-fd Monitor, and adds the thread to `_ringThreads`. So a
second thread calling `ring_attach` already got its own server-side ring + drain + wake-fd. **No server
change was needed.** The block was purely the guest single-owner gate (`g_owner_tid`).

### Guest: a fixed-max lane TABLE keyed by hashed gettid (NOT __thread)
`dserver-ring.c` replaced the six `g_ring_*` process globals with `gr_lane_t g_lanes[GR_MAX_LANES]`
(default 64). Each lane carries its OWN `{active, generation, owner_tid, map, size, wake_fd, seq}` — a
full independent SPSC ring. A thread resolves its lane via `gr_lane_for_this_thread()`:
`gr_find_lane(tid)` (hashed-start linear probe for an `active==1` lane owned by this tid) or
`gr_attach_lane(tid)` (claim a free slot, build the memfd, negotiate via the EXISTING `ring_attach` RPC,
publish). On lane exhaustion the thread cleanly **UDS-falls-back** (never blocks, never shares a lane).

**Why a table, not `__thread`:** `__thread` is unsafe this far below libSystem — the perf#9 sleep
accountant (`rpc-sleep-account.h`) records "TLS aborts here pre-pthread", and P3 chose single-owner
*precisely* to dodge guest TLS fragility. A static table keyed by TID keeps the per-thread SPSC
invariant (each live TID → exactly one lane) WITHOUT the emulation layer's TLS machinery. The storage
mechanism is an impl detail; the per-thread SPSC lane is the invariant. (The brief authorized this
proven-safe fallback explicitly; the user's "TLS lane attach" wording is satisfied in spirit — per-thread
lane state — by the safe mechanism.)

### The active-bit / generation bookkeeping (the dar-my8 gate)
The hazard a multi-lane table introduces is the active-bit/generation bookkeeping: a lane freed by a
dying/forking thread and reused by a new (possibly tid-recycled) thread must never let a reader act on a
half-initialized lane or false-match a stale epoch. The contract:
- **acquire publishes `active=1` LAST** (release order) after fully initializing the lane — a lookup that
  observes `active==1` always sees a complete lane. Claim uses a CAS `0 → 2` ("claiming" sentinel, not
  yet findable) so two racing threads get distinct slots; settle to `1` only after init.
- **every (re)claim bumps `generation`** so a recycled TID landing on the same slot can't match a prior
  epoch.
- **release clears `active=0` FIRST** so the slot can't be reclaimed while still advertised.

This is pinned by `tests/ring_multilane_gate_test.c` (the dar-my8 exhaustive-interleaving lesson applied
to multi-lane): a deterministic single-stepped driver enumerates EVERY relative ordering of
{acquire-of-lane, reader-observes, release, reclaim} and asserts the two invariants for every
interleaving. GREEN = zero violations. RED arms `-DRACE_PUBLISH_ACTIVE_BEFORE_INIT` (publish active
before init → reader reads garbage) and `-DRACE_REUSE_WITHOUT_GEN_BUMP` (reuse without gen bump → stale
epoch false-matches) both fail. Wired into `run-ring-shm-validate.sh`.

### fork
`__dserver_ring_postfork_reset()` drops EVERY lane (munmap + close + zero the table, generation back to
0). The parent's lane mappings/wake-fds belong to the parent's server-side threads; the child re-attaches
lazily, per thread, on demand. Fork-storm 3/3 DONE with ring health all-0 (no wedge, no lane leak).

## NON-GOALS (held)
- NO earlier attach (D15a proved it's not the lever).
- NO MPMC process-wide ring — each lane stays strictly SPSC (one guest thread ↔ the server).
- NO new opcodes — the Lane-1 C2S set is unchanged (13 ops). This is transport plumbing, not membership.
- NO duplex / mach_msg_overwrite / psynch touched (those functions were threaded through the per-lane
  accessors for correctness but their behavior + default-OFF gating are unchanged).
- NO shared ring across threads (the whole point is per-thread SPSC; sharing would reintroduce the
  producer race the SPSC correctness depends on NOT having).
- **ABI stays v5** — a new lane is the SAME ring ABI attached N times; `ring_attach` was already
  per-thread, so no ABI bump.

## ACCEPTANCE
1. ✅ Deterministic multi-lane active-bit/generation gate GREEN + 2 RED arms; wired into the validate
   suite; full perf#18 gate suite GREEN.
2. ✅ Per-thread lanes implemented (guest fixed-max lane table keyed by hashed gettid — the proven-safe
   storage; active bitmap via the CAS'd `active` word; UDS fallback on exhaustion; postfork drops all
   lanes). NO shared ring across threads.
3. ✅ Live A/B (same warm workload, matched dyld+dylib+server): post_attach_eligible_UDS 1533 → 231
   (−85%); heatmap ring-share ~100% on the eligible ops; fork-storm 3/3; ring health all-0; no wedge.
4. ✅ NO earlier attach, NO MPMC ring, NO new opcodes, NO duplex/msg_overwrite/psynch, NO shared lane.
   ABI v5 unchanged.
5. ✅ Prod binaries (server + dylib + dyld, all locations) restored + md5-verified; prod boots clean.
6. ✅ TLS-safety claim PROVEN (not assumed): the rpc-sleep-account.h "TLS aborts here pre-pthread" record
   + the P3 single-owner-as-TLS-dodge precedent → used the lane-table fallback the brief authorized.

## NOTES / future
- High-fan-out per-thread Monitor count: the server registers one wake-fd Monitor per attached thread. At
  very high thread fan-out this is one epoll fd + one Monitor per lane. Not a problem at the homebrew
  workload's fan-out (≤ a few hundred live lanes across all processes); if it ever becomes one, the spin
  phase already drains lanes without the per-fd Monitor on the hot path (the Monitor is only the cold
  doorbell). NOTED, not addressed in D16 (measure first).
- `GR_MAX_LANES = 64` per process; exhaustion logs once (phase-prof builds) and UDS-falls-back. The
  homebrew workload never approached this.
- The matched-dyld requirement (drives rejects to 0) is the same deployment/versioning hygiene D15a
  flagged; in a coherent production build dyld+dylib+server are always the same ABI.

## LIVE-RUN method
`tests/run-per-thread-lanes-d16.sh {baseline|d16}` with `DEPLOY_DYLD=1` for the matched-dyld clean
signal. setuid `darling-prefix/bin/darling` drives `DPREFIX=darling-prefix-homebrew-test`; deploys the
matched census server + the chosen dylib (3 locations) + (with DEPLOY_DYLD=1) the matched dyld (2
locations); warm-boots with `DARLING_SERVER_ATTACH_CENSUS=1 DARLING_SERVER_RPC_HEATMAP=1` in the server
env; runs the workload; reads `tools/darling-stat`. Prod binaries backed up to `.prod-bak`, restored +
md5-verified after. NEVER pushed.
