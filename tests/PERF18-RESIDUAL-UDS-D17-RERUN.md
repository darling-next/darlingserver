# perf#18 D17-RERUN (dar-1il.13): residual-UDS census on a HEAVY multi-thread workload

**RECON-ONLY.** Re-measures the post-D16 residual census ([[PERF18-RESIDUAL-UDS-D17.md]]) on a
representative heavy workload instead of D17's startup-dominated `true`/`echo` micro-processes. Same
default-OFF instrumentation (`DARLING_SERVER_RESIDUAL_CENSUS=1` + `DARLING_SERVER_ATTACH_CENSUS=1` +
`DARLING_SERVER_RPC_HEATMAP=1` + `DARLING_GUEST_LANE_STATS=1`). No new migrations, no duplex/psynch, no
lane-ownership change. Snapshots: `d17rr-heavy-armed.json` (1× probe), `d17rr-heavy-armed-final.json`
(3× probe + fork-storm, 363,734 RPCs). Matched D17 server+dylib+dyld so `attach_rejects == 0`. Prod
binaries restored + md5-verified after (srv 835946.., dyl 6bd251.. ×3, dyld 79b227.. ×2).

## Why this rerun (the D17 measurement caveat)
D17 was measured on `darling shell /usr/bin/true` + `/bin/echo` + a 30× `true` fork-storm — startup-only
mayflies (max **3** ring-threads/process). Its STRUCTURAL verdict and mechanism stand, but its NUMBERS
(231 ≈ 2/proc residual; the top-UDS shares) were unrepresentative. This rerun drives a process that issues
hundreds of thousands of RPCs and, critically, goes **wide** (one process, 96 concurrent threads) so the
per-PROCESS lane cap (`GR_MAX_LANES = 64`) is actually exercised.

## Workloads
1. **Heavy steady-state / lane-exhaustion** — ONE process spawning **96 concurrent threads**, each looping
   `task_self_trap` / `host_self_trap` / `thread_self_trap` (the D10 Tier-2 ring ops) ×600 iters. Run 3×.
   This is the real exhaustion test: >64 simultaneously-live eligible-op threads in a single process.
2. **Meaningful fork-storm** — 48 children each doing real ring work (mach ops + execs) before clean exit.
3. **make -j8 build (attempted, ABANDONED as a census workload)** — see the env caveat below.

### ENV caveat — `make -j` clang build is not a viable census workload here (not a transport finding)
A real `make -j8` clang build of 200 `.c` files (the canonical perf#18 Homebrew-build target) was attempted
first. It does NOT complete under this emulation harness: the `darling shell` wrapping the build **hangs on
teardown** after the build's objects are produced. This reproduced **identically on the prod (pre-D16)
dylib** — a small control build produced 20/20 objects and then the wrapper hung past its own `timeout`.
So it is a **harness/teardown limitation, not a transport defect and not D17-specific**. (Under the D17
dylib the heavy `clang` additionally never produced objects inside a bounded window, but the prod-identical
teardown hang means the build can't serve as a controlled census workload regardless.) The wide-thread
probe captures the same "many forked/threaded subprocesses each doing eligible ops" pressure the make
swarm represents, with binaries that complete and exit cleanly so the census is trustworthy. The lesson
(make/configure is the motivating target) is unchanged; measuring it needs a harness that can tear down a
fork-heavy leaf, which is out of scope for a RECON census.

## HEADLINE — under a WIDE process, the residual is dominated by reason **A/C (lane exhaustion)**, not B/D
The D17 micro-workload saw exhaustion = 0 and residual = B/D only. A genuinely wide process flips this:

```
residual_reason (final, 363,734 RPCs):
  thread_no_ring_proc_none        0     <- reason A (whole-proc pre-attach)
  thread_no_ring_proc_has    118998     <- reason A/C: thread has NO lane while its proc DOES  <-- DOMINATES
  thread_has_ring               729     <- B/D: the D17 per-image-split tail (now ~flat constant)
  control_plane                2430
  ineligible                 356063
```

Guest lane stats make the cause unambiguous — it is **C (exhaustion)**, reproducibly:
```
[dring-lane-stats] pid=420 acquired=64 exhausted=39350 reclaimed=0 held_now=64 max=64   (run 1)
[dring-lane-stats] pid=520 acquired=64 exhausted=39349 reclaimed=0 held_now=64 max=64   (run 2)
[dring-lane-stats] pid=620 acquired=64 exhausted=39321 reclaimed=0 held_now=64 max=64   (run 3)
```
A 96-thread process lands **exactly 64** threads on lanes (`acquired=64`, `held_now=64`, `max=64`), and the
remaining ~32 threads can never get a lane — each of their eligible self-trap calls re-attempts, misses
(`exhausted ≈ 39,330`/run ≈ 32 threads × 600 iters × ~2 self-traps), and falls back to UDS. That UDS fallback
is exactly the `thread_no_ring_proc_has` (reason A) bucket server-side, and shows up as the UDS half of the
self-trap heatmap rows:

| op | ring | uds | ring% | r_p50 | u_p50 | nature |
|----|-----:|----:|------:|------:|------:|--------|
| host_self_trap   | 114916 | 59400 | 66% | 4us | 2us | wide-thread eligible op; 64/96 ride ring, 32/96 exhausted→UDS |
| thread_self_trap | 114748 | 59499 | 66% | 4us | 2us | same |
| mach_reply_port  |   2296 |     0 | 100%| 4us | — | rides ring fully |
| task_self_trap   |   1147 |     0 | 100%| 8us | — | rides ring fully |
| vchroot_path     |    730 |   364 | 67% |16us | 8us | the B/D per-image tail (started_suspended sibling) |
| started_suspended|    365 |   365 | 50% | 8us | 2us | the B/D per-image tail |

**Key nuances:**
- Even *under hard exhaustion*, the migrated self-trap ops still ride the ring **~66%** of the time — the
  cap throttles only the over-64 tail, and degradation is graceful: `ring_fast_fail=0`, `ring_s2c_full=0`,
  `ring_fast_suspend=0`, `clients_blocked_in_rpc=0`, `attach_rejects=0`. Exhaustion falls back to UDS
  cleanly with no wedge, no corruption, no lost wakeups.
- The classifier reports exhausted threads as `thread_no_ring_proc_has` (reason A — "this thread has no
  lane"), which is **structurally indistinguishable from reason A timing** at the server. The GUEST
  `exhausted` counter is what disambiguates C (exhaustion) from A (not-up-yet). D17's brief anticipated
  exactly this: ">64 concurrent eligible-op threads → reason C, and a `GR_MAX_LANES` bump is the lever —
  IF it triggers." **It triggers, hard, and reproducibly.**
- `residual_max_ring_threads_per_process = 3` server-side looks low only because it's sampled at snapshot
  time, *after* the 96-thread probe exited and released its lanes; the durable peak is the guest
  `acquired=64 / held_now=64`.

## The D17 B/D tail is confirmed as a small FLAT constant
`thread_has_ring` (B/D, the per-image lane-table split: `started_suspended` + `vchroot_path`) is **729**
at 363,734 RPCs and was **717** at 132K RPCs — it does **not** grow with steady-state volume. It is a
~2/process startup floor exactly as D17 concluded; on a heavy workload it is a **<0.2%** tail
(729 / 363,734). D17's B/D verdict and the per-image-split mechanism are unchanged and now quantified as
negligible relative to total traffic.

## VERDICT (A/B/C/D), revised for heavy/wide workloads
- **B/D (per-image split)** — confirmed, but a tiny flat ~2/process constant (<0.2% under load). Not worth
  a migration on its own (matches D17).
- **C (lane exhaustion)** — **NEWLY OBSERVED and dominant for wide processes.** D17 (max 3 threads/proc)
  could never surface it; a process with >64 concurrent eligible-op threads pins all 64 lanes and pushes
  the rest to UDS. Reproducible (~39,330 misses/run across 3 runs).
- **A (first-before-lane)** — `thread_no_ring_proc_none = 0` throughout; whole-process pre-attach is not a
  factor. (The `thread_no_ring_proc_has` bucket here is exhaustion, not timing — see disambiguation above.)

### The lever (if pursued — NOT in scope for this RECON bead)
Raise the per-process lane cap `GR_MAX_LANES` (currently 64) for very-wide processes, and/or add lane
**reclaim** for parked/idle threads (`reclaimed=0` was observed — lanes are held for the thread's life, not
recycled while the thread lives). Whether this matters in PRODUCTION depends on whether real targets spawn
single processes with >64 concurrently-RPC-active threads; the motivating `make`/`configure` swarm is
many *processes* of modest thread count (each subprocess stays well under 64), so it would mostly hit the
B/D floor, not exhaustion. A wide-threaded app (a JVM, a thread-pool server, a `-j128` in-process build)
WOULD hit it. Recommend a separate bead only if such a target is in scope; flag `reclaim`-on-thread-exit as
the cheaper half of that lever.

## Ring health under heavy load (the safety story)
Across 363,734 RPCs incl. 3× 96-thread exhaustion bursts + a 48-child fork-storm, on the same warm server
(PID constant — no respawn): `ring_fast_fail=0`, `ring_s2c_full=0`, `ring_fast_suspend=0`,
`clients_blocked_in_rpc=0`, `attach_rejects=0`. Fork-storm children all acquired lanes cleanly
(`acquired=1/2 exhausted=0 held_now=1`). Lane exhaustion is a clean UDS fallback, never a failure.

## LESSONS
- **"exhaustion=0" was a workload artifact, not a property of the system.** D17's max-3-threads/proc
  workload structurally could not reach the 64-lane cap; the cap's behavior only appears under a wide
  process. Generalizes the D14→D15a / D17 discipline: a residual classification is only as representative
  as the workload's *shape* (here: thread-width per process), not just its volume.
- **The server-side classifier cannot tell exhaustion (C) from not-up-yet timing (A)** — both present as
  `thread_no_ring_proc_has`. The GUEST `exhausted` counter is the disambiguator; keep both halves.
- **A `make -j` clang build is not a usable census leaf in this harness** (teardown hangs, prod-identical).
  Wide-thread + fork-storm probes are the practical stand-ins for the fork/thread swarm.
- **Census arming does not survive a server respawn.** An early attempt lost the census when killing a
  wedged build leaf respawned the server; the valid runs kept ONE warm armed server alive (verified by
  constant PID) and snapshotted the stat socket before any teardown.

## LIVE-RUN method
`tests/run-residual-census-d17-rerun.sh` (the full three-workload runner, incl. the abandoned make build)
plus the focused manual sequence that produced the trustworthy numbers: deploy matched D17 server+dylib(×3)
+dyld(×2); warm-boot with all censuses armed; recompile `d17rr-threads.c` (96 threads ×600 iters) in-guest;
run fork-storm + 3× the 96-thread probe against the **same** warm server (PID held constant — no respawn);
`darling-stat` snapshot the live armed server; restore prod + md5-verify. Guest workload files live in
`$PREFIX/Users/ilyagulya/d17rr-*`. HEADs darlingserver @d7573c7 / xnu @a1425a0. NEVER pushed.
