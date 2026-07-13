# perf#18 D14 (dar-1il.9) — post-D10/D11/D13 RPC heatmap REFRESH + next-tail pick

**RECON-ONLY.** No migration, no arena, no duplex, no new transport, no ABI change. The single code
change is the mandatory S2C-attribution metric fix (recon infrastructure, server-only). Lane-1 set
stays 13 ops. Snapshot: `PERF18-RPC-HEATMAP-D14-snapshot.json` (same warm workload as D9/D13: shell
boot + exec batches + 3-round fork-storm of 30 children + concurrent `ls`).

## VERDICT (one line): **STOP migrating individual ops. The remaining reclaimable UDS tail is
PRE-ATTACH-dominated — the next high-leverage lever is EARLIER RING ATTACH, not another op migration.**

There is no clean *warm* fixed-shape closed op left to migrate. Every cheap closed op is already on
the ring (D10/D11/D13); the ops still on UDS are either pre-attach (ring not yet attached), real
blocking waits, the ring handshake itself, or fd-passing. See the ranked evidence below.

---

## 0. THE METRIC FIX (done FIRST — D13 proved the old attribution was wrong)
The D9 per-op `caller_s2c` column was a **sticky per-thread bool** (`_heatmapCallDidS2c`): set in
`_s2cPerform` when the S2C targeted the current thread, then read+reset at the *next* `recordCall`.
On an exec/teardown thread it OVER-ATTRIBUTED the unrelated vm_map_copyout teardown munmap S2C to
whatever op was recorded next — D13 saw `mldr_path caller_s2c=3` (auto-verdict flipped to
`duplex-only`) while the transport-correct `s2c_munmap_ring_parent` stayed 0 across 284 ring calls.

**Replaced** (per user preference — it is cheap; the active callnum is already in scope at the S2C
site): `Metrics::recordCallerS2cFor(callNumber)` bumps `perCallDidCallerS2c[callNumber & 0xff]`
directly, called from `_s2cPerform` with `_activeCall->number()` — attributing the S2C to the op
ACTUALLY executing when it fires, not a bystander. The sticky `_heatmapCallDidS2c` field + its
set/read/reset sites are DELETED; the three `recordCallHeatmap` sites pass `didCallerS2c=false`. The
transport-correct `s2c_munmap_{ring,uds,no}_parent` counters are unchanged (kept as the cross-check).

**Gate:** `rpc_heatmap_gate_test.cpp` INVARIANT 5 + RED arm `-DRED_BREAK_PERCALL_LEAK` — an S2C
charged to op A must NOT leak onto op B even when B is recorded next on the same thread. GREEN +
3 RED arms (`-DRED_BREAK_DISARMED_NOOP`, `-DRED_BREAK_VERDICT_S2C`, `-DRED_BREAK_PERCALL_LEAK`) all
fail as required. Full perf#18 gate suite GREEN.

**Before/after proof the fix worked (acceptance item 3):** with the scoped attribution,
`mldr_path` now reads `caller_s2c=0` and verdict `already-on-ring` (NOT `duplex-only`). EVERY op in
the refreshed heatmap shows `caller_s2c=0`; the only munmap S2Cs (5) have a UDS parent
(`s2c_munmap_uds_parent=5`, `s2c_munmap_ring_parent=0`) — the server-internal exec teardown, exactly
as D6 attributed. The false latch is gone.

---

## 1. TOTAL RPC + ring/UDS split (aggregate)
- **Total heatmap-recorded RPC = 10621** (server-wide `rpcs_serviced` = 10621).
- **Ring = 3917 (36.9%)**, **UDS = 6704 (63.1%)**.
- Ring health pristine: `ring_fast_hit=1022`, `ring_fast_fail=0`, `ring_fast_fallback=0`,
  `ring_s2c_full=0`, `ring_fast_suspend=0`, `clients_blocked_in_rpc=0`. No wedge; fork-storm 3/3.

The 63% UDS share is NOT untapped transport headroom — it is dominated by pre-attach traffic, blocking
waits, the attach handshake, and fd-passing (breakdown below). The genuinely reclaimable closed-op
work is already on the ring.

## 2. TOP REMAINING UDS BY COUNT
| op | uds | ring | uds_p50 | class | why still UDS |
|----|----:|-----:|--------:|-------|---------------|
| pthread_canceled | 1852 | 5 | **2µs** | fiber | at the noise floor — ~0 reclaimable (D9 trap: hot≠reclaimable) |
| ring_attach | 720 | 0 | 32µs | fiber | **the ring opt-in handshake — cannot ride the ring it establishes** (D9 trap) |
| checkin | 520 | 0 | 64µs | fiber | process bring-up handshake; pre-attach + setup work |
| mach_reply_port | 420 | 1022 | 4µs | nofiber | **already-on-ring**; UDS residual = pre-attach early-boot |
| vchroot_path | 418 | 208 | 16µs | fiber | **already-on-ring** (D13); the 418 UDS are the mldr-binary pre-attach site (no ring) |
| fork_wait_for_child | 296 | 6 | **512µs** | fiber | **real blocking WAIT, not round-trip** (D9 trap: latency is the work) |
| interrupt_enter | 272 | 0 | 4µs | fiber | signal-interrupt UDS protocol (savedReply coupling, D12) |
| interrupt_exit | 272 | 0 | 4µs | fiber | signal-interrupt UDS protocol (savedReply coupling, D12) |
| thread_self_trap | 219 | 510 | 8µs | nofiber | **already-on-ring** (D10); UDS = pre-attach |
| checkout | 212 | 0 | 8µs | fiber | **fd-passing** — no ring ancillary channel |
| host_self_trap | 211 | 721 | 4µs | nofiber | **already-on-ring** (D10); UDS = pre-attach |
| uidgid | 210 | 212 | 4µs | fiber | **already-on-ring** (D11); UDS = pre-attach |

## 3. TOP REMAINING UDS BY RECLAIMABLE TIME (uds_count × uds_p50)
| op | reclaim (µs-units) | uds × p50 | disposition |
|----|-------------------:|-----------|-------------|
| fork_wait_for_child | 151552 | 296 × 512µs | **NOT transport** — real blocking wait for the child. Do not migrate. |
| checkin | 33280 | 520 × 64µs | bring-up handshake; mostly pre-attach + genuine setup, not a closed fast op. |
| ring_attach | 23040 | 720 × 32µs | **the handshake itself** — definitionally cannot self-migrate. |
| vchroot_path | 6688 | 418 × 16µs | **already-on-ring**; UDS bucket = mldr-binary pre-attach site (no ring exists there). |
| pthread_canceled | 3704 | 1852 × 2µs | at floor latency; ring won't beat 2µs. Not worth it. |
| thread_self_trap | 1752 | 219 × 8µs | already-on-ring; pre-attach residual. |
| checkout | 1696 | 212 × 8µs | fd-passing — no ring channel. |
| set_executable_path | 1672 | 209 × 8µs | **100% pre-attach** (mldr binary, no ring) + NO_REPLY (D12). |

## 4. CORRECTED LANE VERDICTS (after the S2C fix)
- `mldr_path`: **already-on-ring** (was falsely `duplex-only` in D13 — the headline fix proof).
- All `already-on-ring` ops show `caller_s2c=0`; their UDS residual is pre-attach early-boot.
- No op anywhere shows a real caller-S2C this run. The duplex-lane hazard is carried ONLY by the
  server-internal teardown munmap (UDS parent, 5) — never by a ring-parked caller.
- The naive `lane1-candidate` auto-verdict (= fiber + no-S2C + UDS-today) is NOT a recommendation;
  every entry under it is disqualified by a D9/D12 trap (see §2/§3).

## 5. THE DECISIVE FINDING — pre-attach dominates the reclaimable tail
The D11-migrated ops `started_suspended` (209) and `get_tracer` (209) are IN the C2S allowlist yet
show **ring=0, uds=209** — i.e. **100% of their traffic is pre-attach**: they fire during process
bring-up, before `ring_attach` completes, so they physically cannot use the ring even though they are
"migrated". The same pre-attach window owns the UDS residual of every `already-on-ring` op
(mach_reply_port 420, host_self_trap 211, thread_self_trap 219, uidgid 210, task_self_trap 209,
mldr_path 1). `set_executable_path`/`set_dyld_info` (209 each) are pre-attach mldr-binary sites with
no ring at all.

**Per-process accounting:** ~209 processes were spawned (task_self_trap total ≈ 720, of which 209 are
pre-attach). Each contributes a fixed ~6–8 pre-attach UDS calls (self-traps, started_suspended,
get_tracer, set_dyld_info, set_executable_path, the first mach_reply_port). That is roughly
**~1500–1800 UDS calls that are pre-attach by construction** — a larger reclaimable pool than any
single remaining op, and it is unlocked by ONE lever: attaching the ring earlier in process startup.

## 6. DELIVERABLE — exactly ONE next lever, or STOP
**STOP on per-op migration.** No clean warm fixed-shape closed op remains; the cheap closed ops are
all already on the ring. The remaining UDS tail decomposes into four non-migratable classes:
1. **pre-attach** (self-traps' UDS residual, started_suspended, get_tracer, set_dyld_info,
   set_executable_path, checkin's setup share) — the ring isn't attached yet.
2. **blocking waits** (fork_wait_for_child 512µs, checkin's wait share) — latency is real work.
3. **the handshake** (ring_attach) — cannot ride the ring it creates.
4. **fd-passing / channels** (checkout, kqchan_*, console_open) — no ring ancillary channel.
Plus `pthread_canceled` at the 2µs noise floor (hot but ~0 reclaimable).

**The next high-leverage lever is a DIFFERENT KIND of bead: EARLIER RING ATTACH** — move
`ring_attach` ahead of the pre-attach self-trap/bring-up burst so that the ~1500–1800 pre-attach UDS
calls per workload land on the ring at once, instead of migrating ops one at a time. This is an
investigation (when can the guest safely map+attach the ring during startup — before or interleaved
with the dyld/mldr bring-up sequence?), not an op migration. Recommended as the dar-1il.10 (D15)
candidate, flagged exactly as the D14 brief anticipated (expected tail class 2). If earlier attach
proves infeasible, the ranked table above is the STOP evidence: the per-op lever is exhausted.

---

## ACCEPTANCE CHECKLIST
1. ✅ Metric fix landed: per-op `caller_s2c` is per-call scoped; RED arm `-DRED_BREAK_PERCALL_LEAK`
   proves no cross-op leak; sticky latch removed; `s2c_munmap_*_parent` cross-check unchanged; full
   perf#18 gate suite GREEN.
2. ✅ Fresh heatmap snapshot from the SAME warm workload; 6 deliverables documented (this file).
3. ✅ Corrected verdicts — `mldr_path` reads `already-on-ring`, NOT `duplex-only` (before/after proof).
4. ✅ Exactly one next lever named (earlier ring attach) with the ranked STOP evidence for per-op
   migration.
5. ✅ NO migration, NO arena, NO duplex, NO new transport, NO ABI change. Lane-1 set stays 13 ops.
6. ✅ Live run clean (boot/shellspawn OK, fork-storm 3/3 rc=0, no wedge, ring health all-0);
   prod binaries restored after measurement.
