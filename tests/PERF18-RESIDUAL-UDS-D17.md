# perf#18 D17 (dar-1il.12): POST-D16 residual-UDS heatmap + classifier

**RECON-ONLY.** Default-OFF instrumentation only (`DARLING_SERVER_RESIDUAL_CENSUS=1` +
`DARLING_GUEST_LANE_STATS=1`, reusing the D15a attach census + D14 heatmap). No new migrations, no
duplex/psynch, no lane-ownership change. Snapshot: `PERF18-RESIDUAL-UDS-D17-snapshot.json` (same warm
workload as D16: warm boot + 12 exec batches + a 30-child fork-storm; matched dyld+dylib+server so
`attach_rejects == 0`). Full perf#18 gate suite GREEN incl. the new `residual_census_gate_test.cpp`; prod
binaries restored + md5-verified.

## HEADLINE — the residual 231 is NOT reason A, NOT C; it is B/D (a specific call-site coverage gap)

The user's question: are the 231 post-attach eligible UDS calls (A) unavoidable
first-eligible-before-this-thread's-lane, (B) a wrapper gap, (C) lane exhaustion, or (D) call sites not
using the ring wrapper? The classifier answers from server-observable per-(thread,process) state at the
UDS receive choke point:

```
residual_reason:
  thread_no_ring_proc_none   0     <- reason A (whole-process pre-attach)
  thread_no_ring_proc_has    0     <- reason A (this thread's lane not up yet)
  thread_has_ring          231     <- B/D: eligible op on UDS DESPITE a live lane
  control_plane            646     (checkin / ring_attach)
  ineligible              4546
residual_uds_despite_lane:
  dserver_callnum_started_suspended   116
  dserver_callnum_vchroot_path        115
```

**All 231 land in `thread_has_ring`** — the calling thread already owned a live ring when these eligible
ops went over UDS. **Reason A buckets are both 0** (it is NOT "no lane yet"), and the guest lane stats
confirm **reason C is 0**:

```
[dring-lane-stats] pid=104 acquired=1 exhausted=0 reclaimed=0 held_now=1 max=64
residual_total_ring_threads_registered  150   (server-side "lanes acquired")
residual_max_ring_threads_per_process     3   (<< GR_MAX_LANES=64 -> exhaustion impossible)
```

So the residual is exactly **two early-init call sites** — `started_suspended` and `vchroot_path` — each
going UDS ~once per process even though the thread has a ring. That is the **B/D signal**: a coverage gap,
not unavoidable timing and not exhaustion.

## WHY those two ops, specifically (the mechanism)

Both call sites DO try the ring wrapper first (`__dserver_ring_started_suspended`,
`__dserver_ring_vchroot_path`), so this is not "a site that forgot the wrapper" in the naive sense. The
heatmap shows both ride the ring the MAJORITY of the time and miss ~once per process:

| op | ring | uds | ring_p50 | uds_p50 |
|----|-----:|----:|---------:|--------:|
| started_suspended | 116 | 116 | 32us | 4us |
| vchroot_path | 232 | 115 | 8us | 16us |

The grounded mechanism is the **per-image lane-table split**:
- `dserver-ring.c` is compiled into TWO separate link units — `emulation` (the main `libsystem_kernel`
  dylib) AND `emulation_dyld` (the dyld/mldr loader image). Each has its OWN `g_lanes[]` table (verified:
  both `dserver-ring.c.o` objects exist, one per image).
- `started_suspended` is issued **only from `sigexc_setup()` under `#ifdef VARIANT_DYLD`** — i.e. from the
  dyld image, very early, before/around exec. It uses the **dyld image's** lane table.
- `vchroot_path` is issued from `init_vchroot_path()` (main dylib) AND, per the in-source note
  (vchroot_userspace.c:1680), from **mldr.c:940 in the mldr binary, which has no ring → always UDS**.

So the residual splits into two structural sub-causes, both "the call ran in an image whose lane is not
the one the server has registered for that OS thread at that instant":
1. **dyld-image / cross-exec lane handoff** (`started_suspended`, and the dyld-phase share of
   `vchroot_path`): the dyld image attaches a lane in ITS table; after exec the main dylib attaches a
   lane in its own table on the same OS thread, and the server's per-thread `attachRing` "replaces any
   prior ring." The first eligible op issued from a given image, before THAT image's lane round-trips
   cleanly, takes UDS. `thread_has_ring` reads true because the server has *a* ring registered for that
   nsid (from the other image / a sibling), even though the calling image's own attempt missed.
2. **mldr binary** (the ~115 `vchroot_path` floor): the mldr ELF links `dserver_rpc_*` directly with no
   ring code; its `vchroot_path` is structurally UDS-only (reason D in the pure sense).

The classifier cannot see the guest's per-image lane identity (a server only sees the OS thread's nsid),
so it honestly reports `thread_has_ring`; the per-image attribution above comes from the call-site read.

## VERDICT (A/B/C/D) and the next lever

- **NOT (C) lane exhaustion** — exhausted=0 guest-side, max 3 ring threads/process vs a cap of 64.
- **NOT (A) "no lane yet"** — the reason-A buckets are 0; the thread has a ring at call time.
- **It is (B/D)** — two specific early-init call sites (`started_suspended`, `vchroot_path`) miss the ring
  ~once per process, rooted in the **per-image lane-table split** (dyld image vs main dylib) plus the
  mldr binary's structurally-UDS `vchroot_path`.

Because it is B/D and not A, the user's proposed **D18 ("after a successful lane acquire, send the same
current request over the ring")** would NOT help here: the same-call-rides-the-ring behavior is ALREADY
what `gr_*_trap` does (it attaches then publishes the same call), and these calls miss for a different
reason (image-split / cross-exec handoff), not "attach-then-UDS-this-one." The actual next lever is a
**small, targeted D18: investigate the two call sites' per-image lane miss** — most likely make the
dyld-image and main-dylib share lane state across the exec boundary (or accept the dyld-phase miss as a
~1/process structural cost and only fix the main-dylib `vchroot_path`/mldr split). Either way it is a
~231-call (≈ 2/process) tail, small relative to the 1533→231 D16 win; worth a bead only if the per-call
latency of those two ops matters at startup.

## TOP TOTAL-UDS LATENCY SOURCES (the other half of D17)

Ranked by UDS count from the heatmap (the genuinely-remaining UDS traffic after per-thread lanes):

| op | ring | uds | uds_p50 | uds_p99 | nature |
|----|-----:|----:|--------:|--------:|--------|
| pthread_canceled | 5 | 1946 | 2us | 8us | **pthread floor** — fast (~2us), caller-S2C/duplex-only per D9; not a ring candidate |
| ring_attach | 0 | 381 | 32us | 128us | **control-plane** — the ring opt-in handshake; cannot ride the ring it establishes |
| checkin | 0 | 265 | 64us | 256us | **control-plane** — establishes the process; pre-everything |
| fork_wait_for_child | 6 | 134 | 256us | 512us | **blocking wait** — real wait for the child, not a round-trip; transport-orthogonal |
| checkout | 0 | 117 | 8us | 64us | process teardown; control-plane-ish |
| started_suspended | 116 | 116 | 4us | 8us | the D17 residual (B/D, above) |
| set_dyld_info | 0 | 116 | 4us | 32us | early dyld-image config (NO_REPLY / pre-attach class) |
| set_executable_path | 0 | 116 | 8us | 64us | early dyld-image, pre-attach (NOT migrated by design) |
| vchroot_path | 232 | 115 | 16us | 64us | the D17 residual (B/D, above) |
| interrupt_enter | 0 | 77 | 8us | 32us | signal/interrupt control-plane |

**The biggest UDS counts are all the EXPECTED non-ring classes** the prior beads already classified:
`pthread_canceled` (the ~2us pthread floor, D9 found caller-S2C → duplex-only), the control-plane
handshakes (`ring_attach`/`checkin`/`checkout` — cannot/should not ride the ring), the blocking WAIT ops
(`fork_wait_for_child` — real waiting, not round-trip latency), and the early dyld-image-only ops
(`set_dyld_info`/`set_executable_path`, pre-attach / NO_REPLY). None of these is a new ring lever; D9's
"hot ≠ reclaimable" rankings already hold. The only *eligible-op* UDS remaining is the 231 D17 residual.

## ACCEPTANCE
1. ✅ Default-OFF residual classifier (reason buckets + per-op uds-despite-lane) + lane stats (acquired/
   exhausted/reclaimed/held + server-side lanes-registered/max-per-process) + p50/p99 ring-vs-UDS per op.
2. ✅ Same warm workload (matched dyld+dylib+server); residual classified.
3. ✅ Verdict among A/B/C/D: **B/D** (two early-init call sites, per-image lane split), NOT A, NOT C.
   The proposed D18 "attach-then-send-same-call" is already the wrapper's behavior and would not help;
   the real next step is the per-image lane-handoff investigation (small ~2/process tail).
4. ✅ No new migrations, no duplex/psynch, no lane-ownership change. Gate RED→GREEN. Prod restored.

## LESSONS
- The D14→D15a "op-observed-on-UDS ≠ ran-before-attach" caution generalizes: here it is
  "UDS-despite-a-live-lane ≠ first-call-before-attach." The classifier separated the two and showed the
  residual is the *former* (B/D), overturning the natural assumption (from D16's prose) that the residual
  was unavoidable first-call-before-attach (A).
- The **per-image lane-table split** (main dylib vs dyld image, each with its own `g_lanes`) is the
  structural root of the residual eligible-UDS. It is invisible to a server-side classifier (which sees
  only the OS-thread nsid) and only emerges from reading the call sites + the build's two `dserver-ring.c`
  objects. Any future "reclaim the last eligible-UDS" work must reckon with this split.

## INSTRUMENTATION (default-OFF; hot path byte-identical when disarmed)
- `Metrics` D17 block (metrics.hpp): `residualCensusOn` (env `DARLING_SERVER_RESIDUAL_CENSUS=1`),
  `residualReason[RR_COUNT]` (mutually-exclusive buckets), `udsDespiteLaneByCallnum[]` (the B/D per-op
  signal), `maxRingThreadsPerProcess` / `totalRingThreadsRegistered` (server-side lane proxies).
  `recordResidualReason()` classifies at server.cpp:723 (the genuine UDS choke point).
- `Server`: bumps `totalRingThreadsRegistered` in `registerRingThread`; computes
  `maxRingThreadsPerProcess` off the hot path at snapshot time (only when armed).
- Guest (dserver-ring.c): per-thread lane stats (`g_stat_lanes_acquired/exhausted/reclaimed`) +
  `__dserver_ring_lane_stats_dump()` on the clean exit path (sys_exit), gated `DARLING_GUEST_LANE_STATS=1`.
- Gate: `tests/residual_census_gate_test.cpp` (GREEN + RED arms `-DRED_BREAK_DISARMED_NOOP`,
  `-DRED_BREAK_AB_SEPARATION`) wired into `run-ring-shm-validate.sh`.

## LIVE-RUN method
`tests/run-residual-census-d17.sh` — setuid `darling-prefix/bin/darling`,
`DPREFIX=darling-prefix-homebrew-test`; deploys the matched residual-census server + the D16 per-thread
dylib (with the lane-stats hook) + the matched dyld; warm-boots with all three censuses armed; runs the
workload (capturing a couple of guest `[dring-lane-stats]` lines); reads `tools/darling-stat`. Prod
binaries backed up to `.prod-bak`, restored + md5-verified after. NEVER pushed.
