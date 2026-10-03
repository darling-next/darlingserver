# perf#18 D18a (task #68): GR_MAX_LANES scaling A/B

**RECON/A-B — one guest constant swept.** Varies only `GR_MAX_LANES` (the per-process guest lane-table
cap) ∈ {64, 96, 128, 256} via a compile-time `#define` override; **no lane reclaim**, **no opcode-set
change**, **no duplex/psynch/mach_msg_overwrite**. `GR_MAX_LANES` is a **guest-only** cap (the server's
`_ringThreads` is an unbounded `std::vector`, no server limit), so the A/B rebuilds only the dylib+dyld and
**reuses the D17 census server unchanged** for every value. Snapshots `d18a-g-*.json`; guest lane-stats are
the primary signal (they dump reliably on every foreground probe run). Prod restored + md5-verified.

## Method
Workload = the D17-rerun exhaustion probe: ONE process × N threads, each looping `task_self_trap` /
`host_self_trap` / `thread_self_trap` (the D10 Tier-2 ring ops) ×600 iters. Per variant: clean cold boot,
warm shellspawn, recompile the probe, run the N-thread probe ×3 **foreground** (`timeout -s KILL … | grep`),
capture the main-thread `[dring-lane-stats]` line, and snapshot the live armed server. 96 threads is the
spec workload; 200 threads added on the 128 and 256 builds to exercise the >cap and mid-cap regimes.

## RESULTS

| GR_MAX_LANES | threads | acquired / held_now | max | exhausted (×3 runs) | self_trap ring% | ring p50/p99 | uds p50/p99 | health¹ |
|-------------:|--------:|:-------------------:|----:|:-------------------:|:---------------:|:------------:|:-----------:|:--:|
| **64**  |  96 | 64 / 64  |  64 | **39351 / 39293 / 39280** | 65% | 4 / 32–128µs | 2 / 4µs  | all 0 |
| **96**  |  96 | 96 / 96  |  96 | **1202 / 1202 / 1202**    | 98% | 4 / 32µs     | 2 / 8µs  | all 0 |
| **128** |  96 | 97 / 97  | 128 | **0 / 0 / 0**             | **100%** | 4 / 32µs | — (uds=0) | all 0 |
| **256** |  96 | 97 / 97  | 256 | **0 / 0 / 0**             | **100%** | 4 / 32µs | — (uds=0) | all 0 |
| 256 | 200 | (726K rpcs) | 256 | 0 | 100% | 4 / 64–128µs | — (uds=0) | all 0 |
| 128 | 200 | 128 / 128 | 128 | **87249 / 87229 / 87285** | 63% | 4–8 / 64µs | 2 / 4µs | all 0 |

¹ health = `ring_fast_fail`, `ring_s2c_full`, `ring_fast_suspend`, `clients_blocked_in_rpc`,
`attach_rejects` — **all 0 in every variant** (census stayed armed, rpcs 350K per 96-thread variant,
726K per 200-thread variant).

### What the numbers say
- **A 96-worker process actually needs ~97 lanes, not 96.** At `GR_MAX_LANES=96` the table fills (`acquired
  =96 held_now=96`) and still logs **1202** exhaustion events — the process has the 96 workers **plus** a
  main/runtime helper thread, so it wants 97. `128@96` shows `acquired=97 held_now=97 exhausted=0`: the
  process settles at exactly 97 lanes and the extra headroom above 97 is unused but harmless.
- **128 fully cures the 96-thread workload:** `exhausted = 0` across all 3 runs, **100% ring** (0 UDS
  fallback), no regressions. This is the spec's proposal condition.
- **256 buys nothing over 128 for a 96-thread process** (identical `acquired=97 exhausted=0`), and costs
  nothing measurable (all health 0). It only helps a genuinely wider process:
- **128 is not a universal cure — it is right-sized, not infinite.** `128@200` still exhausts hard
  (~87,250 misses, 63% ring), because 200 threads > 128 lanes. `256@200` cures it (exhausted=0, 100% ring).
  So the cap should track the realistic per-process thread-RPC fan-out, not be treated as "enough forever."
- **Latency:** under this synthetic hammer the ring p50 (4µs) is slightly *above* the UDS p50 (2µs) because
  96–200 contending threads add ring spin/wake latency to the fast path; the ring's win here is
  eliminating the UDS server-thread-pool round-trips and scaling (100% vs 65% ring), and the p99 stays
  bounded (32–128µs) with no `ring_fast_suspend`/`s2c_full`. No latency regression from raising the cap:
  128 and 256 have identical ring p50/p99 to each other.

## MEMORY / LAYOUT COST
- **Table BSS** = `sizeof(gr_lane_t)=40` × `GR_MAX_LANES`, a static process-global array:
  64 → 2,560 B, 96 → 3,840 B, 128 → 5,120 B, 256 → 10,240 B. **Negligible** (a few KB of BSS per image;
  paid once per process regardless of how many lanes actually attach, and the table is compiled into both
  the `emulation` dylib and the `emulation_dyld` image — so ×2 per process, still < 21 KB at 256).
- **Per-attached-lane** (the real cost, paid only when a thread actually acquires a lane): one memfd mapping
  of `total ≈ 2,560 B` → **1 page (4 KB) of shared memory + 1 memfd + 1 wake eventfd**. Raising the cap
  does NOT allocate these up front — a lane is mapped lazily on first eligible call by that thread.
- **Marginal cost of 64 → 128 for a genuinely wide process** = up to **+64 lanes × (4 KB + 2 fds)** =
  **+256 KB shared memory + 128 fds** for that one process, only if it actually runs >64 concurrent
  eligible-op threads. A normal (≤64-thread) process pays nothing beyond the ~2.5 KB larger BSS table.
- **Server side:** each attached lane adds a `_ringThreads` entry (drained per spin), a mapped ring, and a
  wake-fd Monitor. No hard cap; the cost scales with *attached* lanes, same lazy story. At `256@200`
  (726K rpcs, `threads_reg=642`) the server showed no `s2c_full`/`fast_suspend`/`blocked` — it absorbs the
  extra lanes cleanly.

## REGRESSIONS
None observed at any cap value. Across all 6 runs (350K–726K RPCs each): `ring_fast_fail=0`,
`ring_s2c_full=0`, `ring_fast_suspend=0`, `clients_blocked_in_rpc=0`, `attach_rejects=0`; fork-storm ran
clean; no server wedge, no respawn during the measured probes. Lane exhaustion (when the cap IS hit)
remains a clean UDS fallback, never a failure.

## PROPOSAL
**Adopt `GR_MAX_LANES = 128` as the new default.** It satisfies the spec's condition exactly: at the
96-thread hot-RPC workload it drives `exhausted=0` / 100%-ring with **no regressions** and negligible cost
(+2.5 KB BSS/image always; +≤256 KB shared mem only for a process that genuinely goes wide). It gives real
headroom over the observed "96 workers → 97 lanes wanted" so common wide processes (thread pools up to
~127 concurrent eligible-op threads) stop exhausting, while not paying the (still-cheap) 256 cost for
capacity nothing in the measured range uses.

Caveats to record with the proposal:
- 128 is **right-sized, not infinite** — a process with >128 concurrent eligible-op threads still exhausts
  (see `128@200`). If such targets appear, the cap tracks the fan-out (256 cured 200 threads cleanly), or
  the cheaper structural fix is **lane reclaim on thread-exit** (`reclaimed=0` everywhere here — lanes are
  held for a thread's whole life, so a churny short-thread process accumulates lanes it no longer uses).
  Reclaim is explicitly **out of scope for D18a** (spec: "Do not implement lane reclaim yet") and is the
  natural D18b.
- The change is a one-line `#define` in `dserver-ring.c`; it is a guest-only ABI-neutral constant (the ring
  ABI/opcode-hash negotiation is unaffected — a lane is the same ring attached N times). No server rebuild
  is required for correctness, but the server should be rebuilt from the same tree as usual.

## LIVE-RUN method
Variants pre-built by overriding `#define GR_MAX_LANES` (line 76) per value, rebuilding
`libsystem_kernel.dylib` + `dyld` explicitly (NOT `ninja emulation`), staging each, then reverting source
to the committed `64u` baseline (build tree rebuilt to match). `tests/run-d18a-lanes-ab.sh` is the runner
skeleton; the trustworthy numbers came from the foreground-probe guest-stats sweep (`d18a-guest-sweep.sh`
in the job scratch) — deploy the D17 census server + the variant dylib(×3)+dyld(×2), clean boot, warm
shellspawn, recompile probe, run the N-thread probe ×3 foreground, read the guest `[dring-lane-stats]`
line + a best-effort census snapshot. Prod restored + md5-verified (srv 835946.., dyl 6bd251.. ×3, dyld
79b227.. ×2). Server binaries + committed source UNCHANGED (guest-only `#define` A/B, reverted). NEVER pushed.
