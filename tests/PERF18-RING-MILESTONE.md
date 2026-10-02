# perf#18 — shmem ring transport: current-state milestone (post-D18a)

**Purpose: stop optimizing by inertia.** This is the standing map of what already rides the shared-memory
ring, what deliberately stays on UDS, which investigations are *closed* (STOP), and which branches are
genuinely still open. Continued work should start from an open-branch **trigger** below — not from "this op
is still on UDS."

Source of truth for ring membership is the `DSERVER_RING_C2S_OPCODES` macro
(`darlingserver/include/darlingserver/rpc-supplement.h`) and `ringFastPathEligible()`
(`darlingserver/src/call.cpp`) — this doc summarizes them; if they disagree, the code wins.

_State: as of **D18a** (per-process lane cap `GR_MAX_LANES` = 128). All perf#18 work is **LOCAL / not
pushed**; flipping the ring default-ON is a separate product call. Bead/phase names (not commit hashes) are
used as anchors so this record does not depend on ephemeral local-only heads._

---

## 1. The lane model

Every RPC rides the cheapest lane that preserves its semantics. Membership is a set of orthogonal,
independently-proven properties — **not** "make UDS faster for every RPC."

| Lane | Carries | Status |
|------|---------|--------|
| **Lane 0 · UDS** | Control / fallback: bootstrap, creds, fd-passing, blocking waits, unknown/complex ops, and every destroy-capable / caller-S2C op until a duplex lane proves it. | baseline, always present |
| **Lane 1 · simple ring** | CLOSED request→single-reply, no caller-S2C while the caller is parked. Two sub-tiers: **Tier 1** generic-fiber `doWork`; **Tier 2** no-fiber inline (a strictly smaller, separately-proven set). | live |
| **Lane 2 · duplex ring** | Reentrant lane delivering a caller-side S2C upcall while the caller is parked — the right home for destroy-capable ops. Machinery + a live cure proof exist (D3–D6); **no production op routed onto it yet.** | proven, dormant |

---

## 2. On the ring today

From the shared `DSERVER_RING_C2S_OPCODES` macro (one macro drives guest dispatch, the server allowlist,
and the attach-time opcode hash — so guest and server cannot drift, and a build/version skew rejects the
ring cleanly to all-UDS instead of a silent per-op drop).

| Op(s) | Tier | Shape / how it rides |
|-------|------|----------------------|
| `mach_reply_port` | 2 · surgical direct-dispatch | pure mint; skips Call/Message, ~0.95µs (~31× UDS) |
| `task_self_trap` | 2 · no-fiber inline | pure mint, empty req → `uint32` port |
| `thread_self_trap` | 2 · no-fiber inline | pure mint (D10) |
| `host_self_trap` | 2 · no-fiber inline | pure mint (D10) |
| `mach_port_allocate` | 1 · generic fiber | fixed 16B body (~5.1×) |
| `mach_port_insert_right` | 1 · generic fiber | fixed 16B body (~6.4×) |
| `uidgid`, `set_thread_handles`, `started_suspended`, `get_tracer`, `task_is_64_bit` | 1 · generic fiber | D11 closed-fast batch: fixed inline request + fixed inline reply |
| `mldr_path`, `vchroot_path` | 1 · generic fiber | D13 "path" ops — path bytes travel via the server's `/proc/mem` read/write, so the RPC body stays fixed + tiny (this is **not** a caller-S2C) |

**Hot mint-op latency progression:** P3 26µs → P4 4.7µs → step1 3.5µs → **step2 ~0.95µs** (~31× UDS; beat
the <3µs target by ~3×).

**Per-thread lanes (D16):** each guest thread gets its own SPSC lane → eligible-op UDS traffic dropped
1533 → 231. **D18a** then sized the per-process lane cap to **128**: a 96-worker process actually wants
~97 lanes (96 workers + 1 runtime helper), so 64 exhausted ~39k eligible ops/run to UDS; at 128 it is
`acquired≈97 / exhausted=0`, 100% ring, health counters all 0. The cap is guest-only and ABI-neutral.

---

## 3. Deliberately still UDS

These are **correct on UDS given today's lanes** — not "not done yet." Moving them needs a new lane, or is
pointless, not merely more effort.

**By design — destroy-capable / caller-S2C**
- `mach_port_deallocate`, `mach_port_mod_refs` — a last-ref destroy can drive a munmap S2C upcall to a
  parked caller → deadlock on the simple ring. *"Wrong lane, not bad op":* they belong on Lane 2.
- `mach_msg_overwrite` — the sole boot-time caller-S2C carrier **and** the single hottest RPC; can drive
  mmap/munmap/mprotect/msync S2C. See STOP §4.

**Structural — control-plane & blocking**
- `ring_attach` / `checkin` / `checkout` — the handshake that establishes the ring cannot ride the ring it
  establishes.
- `fork_wait_for_child` — a real blocking WAIT, not a round-trip; transport-orthogonal.
- `pthread_canceled` — the ~2µs pthread floor; caller-S2C → duplex-only (per the D9 heatmap).
- `set_dyld_info` / `set_executable_path` — pre-attach dyld-image / NO_REPLY.

**Residual tail — per-image lane split**
- `started_suspended` & `vchroot_path` miss the ring ~once per process. `dserver-ring.c` is compiled into
  two images (the main libsystem_kernel dylib and the dyld loader image), each with its own lane table; the
  dyld-phase / mldr call misses. A flat ~2/process (<0.2% of RPCs under load). Not worth a migration alone.

---

## 4. STOP decisions (locked)

Investigated to a verdict and closed. **Re-open only with new information — do not re-litigate by reflex.**

- **STOP — `mach_msg_overwrite` subset.** D7 recon + D8 live shape census (362 msgs): 88% blocking receive,
  only 8.3% simple send-only (≈1.6% of all RPC). Do **not** build a Lane-2 send-only subset; receive stays
  UDS. The hottest op is not the next lever.
- **STOP — OOL `mach_msg` → ring.** An OOL message maps pages into the *receiver*
  (mmap+munmap+mprotect+msync caller-S2C). Permanent rule: no OOL-message ring migration until the duplex
  sideband supports **mmap** (D6 covers munmap only). Prerequisite, not sufficient.
- **N/A — `deallocate` / `vm_deallocate` live cure.** D4/D5/D6 built and green, but no production op drives
  the caller-S2C munmap onto the ring today (the trap resolves to a local fast-path; the boot munmaps sit
  under a UDS parent). The duplex sideband is the *prerequisite* for ever ring-migrating such an op; there
  is nothing to cure right now.
- **CANON — ring-membership 5-rule bar (permanent).** Any op joining the simple ring must pass: (1) no
  caller-S2C, (2) not destroy-capable, (3) safety decidable *before* mutation, (4) participates in the ABI
  opcode-hash, (5) side-effect A/B, not just return-code A/B.

---

## 5. Open branches

Genuinely unexplored or deferred — the only places continued work is warranted. Each has a concrete
**trigger**; absent the trigger, perf#18's hot path is done.

- **D18b — lane reclaim on thread-exit.** `reclaimed=0` everywhere today: lanes are held for a thread's
  whole life, never recycled. *Trigger:* a churny or >128-concurrent-eligible-thread process (JVM,
  threadpool server, in-process `-j128`). Changes lane ownership → needs an exhaustive gate (dar-my8 style)
  first.
- **Lane 2 — route a real op onto the duplex ring.** Machinery + live cure proof exist (D3–D6); no
  production op uses it. *Trigger:* a destroy-capable op becomes hot **and** actually drives a caller-S2C
  in production. None does today.
- **Per-image lane handoff (the residual tail).** Reclaim the ~2/process `started_suspended` /
  `vchroot_path` startup miss by sharing lane state across the dyld→main-dylib exec boundary. *Trigger:*
  only if those two ops' startup latency ever matters. Tiny vs the D16 win.
- **Flip ring default-ON.** Rollout checklist + staged-rollout NOTICE are in place and the gate is green.
  This is a product / release call, not an engineering task — everything stays LOCAL and un-pushed until
  then.

---

## The inertia check

The hot Mach mint path is **~31× faster** and eligible-op UDS traffic is essentially gone. The biggest
remaining UDS counts are all things that **should** stay UDS: blocking waits, control-plane handshakes,
fd-passing, the pthread floor. **"Hot ≠ reclaimable"** — the raw hotness ranking has been checked
repeatedly and the top entries are structural.

**Do not pick the next op off a hotness list.** Continued work should start from an open-branch *trigger*
above (a real wide/churny process for D18b; a real hot caller-S2C op for Lane 2), not from "this op is
still on UDS."
