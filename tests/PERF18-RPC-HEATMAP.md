# perf#18 D9 (dar-1il.4): global RPC heatmap + lane-eligibility census

**Status: DONE. Verdict: the next migration is the `*_self_trap` family (thread_self_trap +
host_self_trap) as Tier-2 no-fiber ops.** Chosen by DATA, not by guessing. Pure measurement; no
production behavior change. Diagnostics default-OFF (armed by `DARLING_SERVER_RPC_HEATMAP=1` on a warm
server). Nothing pushed.

## Why D9 exists

D8 measured the single hottest RPC (mach_msg_overwrite, ~19%) and found its reclaimable subset was
~1.6% of all RPC → **STOP**. The lesson: **"op is hot" ≠ "op is reclaimable."** After the simple ring
(Lane 1 Tier-1 + Tier-2), the duplex sideband (Lane 2), and the wake model were all proven, the open
question stopped being "can SHM work" and became "WHERE does it give a >1–2% system win." D9 answers
that **systematically**: per call number, measure both hotness AND the runtime facts that decide lane
eligibility, rank by expected win, classify each op into a lane verdict, recommend exactly ONE next
migration.

## What was built (default-OFF, no behavior change)

A per-call-number heatmap recorded at the existing `recordCall` service-completion sites
(thread.cpp:409 generic fiber, :730 doWorkInline Tier-2, :839 doMachReplyPortInline). For every call
number, when armed, it accumulates:

- **transport split**: `uds` vs `ring` count (which lane serviced it today). Already-ring ⇒ little
  headroom; hot + still-100%-UDS ⇒ the prize.
- **per-transport latency**: `uds_p50/p99`, `ring_p50/p99` (sizes the per-call win a migration buys).
- **used_fiber**: did the op suspend its microthread (generic `doWork`) rather than run inline? A
  fiber/blocking op is not a Tier-2 no-fiber candidate.
- **caller_s2c**: did servicing it drive a caller-S2C upcall (the duplex-lane hazard)? Any nonzero
  count ⇒ Lane 2, not Lane 1 (canon rule 1).

The static eligibility axes (destroy-capable, caller-S2C-capable, simple-ring membership) come from the
existing `dserver_ring_op_class()` table (rpc-supplement.h) at emit time — not threaded through the hot
path. A **lane verdict** combines the measured facts with the static class:

| verdict | meaning |
|---|---|
| `already-on-ring` | canon simple member, observed riding the ring |
| `duplex-only` | canon destroy/caller-S2C, OR a caller-S2C was MEASURED this run |
| `tier2-candidate` | never used a fiber AND never an S2C → no-fiber direct-dispatch shape |
| `lane1-candidate` | completed on a fiber, no caller-S2C, currently (mostly) UDS |
| `needs-review` | mixed/unknown — a human must classify |

Wiring: `Metrics::heatmapOn` + `recordCallHeatmap()` + the `rpc_heatmap` snapshot block
(metrics.hpp/cpp); sticky per-call latches `_heatmapCallWasRing` (set in `beginRingReply`) and
`_heatmapCallDidS2c` (set in `_s2cPerform` when the S2C targets the current thread), reset at the
record site (thread.hpp/cpp); arming hatch in `Server::Server` (server.cpp,
`DARLING_SERVER_RPC_HEATMAP=1`). Hard gate `tests/rpc_heatmap_gate_test.cpp` (GREEN + 2 RED arms,
wired into `run-ring-shm-validate.sh`): pins the default-OFF no-op invariant, the transport split, the
flag accumulation, and the verdict derivation (incl. that a measured caller-S2C FORCES `duplex-only`).

## Live run (homebrew-test prefix, heatmap armed, warm server)

Workload: several shellspawns + a 40-child fork-storm + `ps aux`. **5057 RPC** serviced across 24 call
numbers; ring health clean throughout (`ring_s2c_full=0`, `ring_fast_suspend=0`, `ring_fast_fail=0`);
post-workload shellspawn still healthy (no wedge from the armed diagnostic). Raw snapshot:
`tests/PERF18-RPC-HEATMAP-snapshot.json`.

### Verdict tally
| verdict | ops | calls | % of RPC |
|---|---|---|---|
| lane1-candidate | 20 | 2332 | 46.1% |
| duplex-only | 2 | 1955 | 38.7% |
| already-on-ring | 2 | 770 | 15.2% |

### Top by raw count (hotness)
| op | total | uds | ring | fiber | s2c | uds_p50 | verdict |
|---|---|---|---|---|---|---|---|
| pthread_canceled | 1873 | 1868 | 5 | 1873 | **2** | 2µs | duplex-only |
| mach_reply_port | 514 | 168 | 346 | 168 | 0 | 4µs | already-on-ring |
| host_self_trap | 343 | 341 | 2 | 343 | 0 | 4µs | **lane1-candidate** |
| thread_self_trap | 265 | 265 | 0 | 265 | 0 | 4µs | **lane1-candidate** |
| task_self_trap | 256 | 83 | 173 | 83 | 0 | 4µs | already-on-ring |
| ring_attach | 256 | 256 | 0 | 256 | 0 | 32µs | (excluded — see below) |
| vchroot_path | 248 | 248 | 0 | 248 | 0 | 8µs | lane1-candidate |
| checkin | 182 | 182 | 0 | 182 | 0 | 64µs | (blocking — see below) |
| uidgid | 182 | 182 | 0 | 182 | 0 | 4µs | lane1-candidate |
| set_thread_handles | 178 | 178 | 0 | 178 | 0 | 4µs | lane1-candidate |

### Three traps the raw ranking sets — and how the data avoids them
1. **`pthread_canceled`** (37% of RPC, the single hottest) is `duplex-only` because `caller_s2c=2` was
   MEASURED (it can upcall the caller, even if rarely). Per canon rule 1 that disqualifies Lane-1 — but
   it doesn't matter: its uds_p50 is **2µs** (already transport-bound), so reclaimable ≈ 0. Hot but a
   non-target. (This is the D8 lesson reproduced by a different op.)
2. **`ring_attach`** is the single biggest raw reclaimable (256 × ~29µs) but it is the per-thread ring
   **opt-in handshake itself** (generate-rpc-wrappers.py:663) — you cannot ring-attach over the ring you
   are attaching. Chicken-and-egg; **excluded**.
3. **`checkin` (64µs), `fork_wait_for_child` (256µs), `interrupt_enter`** carry high p50 because they
   **block on real coordination/waiting**, not on the round-trip. Migrating the transport reclaims ≈0
   of that latency. Excluded from the reclaimable set (the perf#25.1 / D8 "latency is real work" rule).

### Honest reclaimable set (closed-fast lane1 candidates, currently UDS, reclaim vs ~3µs generic ring)
After excluding the three traps, the win is a **long tail of cheap (1–8µs) thread/task setup traps** —
no single big lever, the value is the aggregate of bulk-migrating them. Top of the tail:
`vchroot_path` (248 calls, 5µs reclaim), `host_self_trap`/`thread_self_trap`/`uidgid`/
`set_thread_handles` (1–4µs each), `checkout`, `set_executable_path`, the `*_self_trap` family.

## The ONE recommended next migration (by data)

**Migrate `thread_self_trap` + `host_self_trap` as Tier-2 no-fiber ops.**

Why this exact bead, justified by the measurements + the canon:
- **Byte-identical shape to the already-proven family.** generate-rpc-wrappers.py:305-315 shows
  `task_self_trap`, `host_self_trap`, `thread_self_trap` have the SAME signature: empty request `[]`,
  single `uint32_t port_name` reply — the same shape as `mach_reply_port` (already Tier-2) and
  `task_self_trap` (already Lane-1). They are the two remaining un-migrated members of that class.
- **Measured eligibility is clean.** Both: 100% UDS today (608 calls combined ≈ 12% of workload RPC),
  `caller_s2c=0`, no destroy. They pass all 5 membership-canon rules trivially.
- **Lowest risk, highest confidence, reuses existing machinery.** Pure-mint port traps → the exact
  Tier-2 no-fiber direct-dispatch path that `mach_reply_port` proved (~0.95µs); cacheable per-process
  like `task_self_trap`. No new lane, no S2C, no destroy, no blocking — the cheapest possible migration
  to land and gate.
- **Per-call win** ~4µs→~1µs; small per call, but it is the highest-confidence step and completes the
  self-trap family. The broader closed-fast tail (vchroot_path, uidgid, set_thread_handles, …) is the
  natural FOLLOW-ON bulk Lane-1 bead once the self-trap family proves the pattern again.

## Non-goals (held — user, verbatim)
Did NOT migrate anything; did NOT touch mach_msg_overwrite; did NOT expand the duplex lane; did NOT
build the mmap sideband; did NOT change Lane-1 membership ({task_self_trap, mach_reply_port,
mach_port_allocate, mach_port_insert_right}).

## Reproduce
1. Build darlingserver (ring-ON branch). Deploy to the prefix's `bin/darlingserver` (back up prod).
2. `DPREFIX=<prefix> DARLING_SERVER_RPC_HEATMAP=1 <launcher> shell <workload>` (warm server).
3. `tools/darling-stat <prefix>` → read `rpc_heatmap`. Rank: `count × (uds_p50 − ring_p50) ×
   eligibility`, then exclude handshake/blocking ops as above.
4. Restore the prod binary. Gate: `bash tests/run-ring-shm-validate.sh`.
