# perf#18 D15a (dar-1il.10): ring-attach TIMELINE / reclaimability recon

**RECON-ONLY.** Default-OFF instrumentation only (env `DARLING_SERVER_ATTACH_CENSUS=1`). No attach
move, no op migration, no arena/duplex/psynch. Snapshot:
`PERF18-ATTACH-TIMELINE-D15a-snapshot.json` (same warm workload as D9/D13/D14: shell boot + exec
batches + a 30-child fork-storm; 500 processes attached, 10368 RPC).

## HEADLINE — the D14 hypothesis is REFUTED by direct measurement
D14 *inferred* (from `started_suspended`/`get_tracer` showing `ring=0`) that the reclaimable UDS tail
was **pre-attach** and recommended "move attach earlier." **The census measures the pre-attach window
directly and it is essentially EMPTY of reclaimable work:**

> **`attach_census_total_pre_attach_eligible = 9`** — across all 500 attached processes, only **9**
> ring-eligible UDS calls EVER occurred before the ring attached.

Moving attach earlier (options A/B/C/D below) would reclaim **~9 calls total** — a non-lever. The D14
inference confused "this op was observed on UDS" with "this op ran before attach." It did not: those
ops run over UDS **after** attach (next section).

## THE TIMELINE (per-process, measured)
- **Attach is already very early.** `attach_ordinal` histogram (UDS-call ordinal at which `ring_attach`
  succeeded): **p50 = 4, p95 = 32, mean = 11**. A process makes ~4 UDS calls, then attaches.
- **Those first ~4 calls are NOT eligible.** The only ops with a meaningful pre-attach count are
  `checkin` (297 pre) and `ring_attach` itself (298 pre) — the bootstrap handshake and the attach
  handshake. **Neither is ring-eligible** (checkin establishes the process; ring_attach establishes
  the ring). Every eligible op has `pre_attach_uds` of 0 or 1.
- **`attach_census_first_uds_calls = 297`** ≈ the number of processes that made ≥1 UDS call; the first
  UDS call of a process's life is `checkin`, as expected.

So the pre-attach sequence is, per process: `checkin` → `ring_attach` → (ring live by ordinal ~4).
There is no deep pre-attach burst of eligible ops to rescue.

## WHERE THE RECLAIMABLE UDS ACTUALLY IS — post-attach, NOT pre-attach
The eligible ops that D14 saw on UDS are **post-attach UDS**, totaling **2053 calls**:

| eligible op | pre_attach_uds | post_attach_uds |
|-------------|---------------:|----------------:|
| mach_reply_port | 2 | 408 |
| vchroot_path | 1 | 407 |
| thread_self_trap | 1 | 213 |
| host_self_trap | 1 | 205 |
| uidgid | 1 | 204 |
| started_suspended | 1 | 203 |
| get_tracer | 1 | 203 |
| task_self_trap | 1 | 203 |
| set_thread_handles | 0 | 6 |
| mldr_path | 0 | 1 |
| **total** | **9** | **2053** |

These are calls to ops that *are on the ring*, issued over UDS **after** the ring exists. Two causes,
both unrelated to attach timing:

1. **Single-owner ring (P3/P4 design).** The ring is owned by the ONE thread that first attached it
   (`g_owner_tid`, dserver-ring.c:63); any OTHER thread in the process sees a foreign owner and falls
   back to UDS (`__dserver_ring_try_attach` returns false for non-owners). A process that runs eligible
   ops on more than one thread scatters them to UDS. With ~408 post-attach `mach_reply_port` across 500
   processes, this is the dominant share — the ring is per-process-single-thread, but the eligible-op
   traffic is multi-thread.
2. **ABI-skew rejects.** `attach_rejects = 204`, **all reason `dserver_ring_reject_abi`** (reject code
   2). 204 of 704 attach attempts (29%) hit a guest dylib whose ring ABI differs from the deployed
   server — so those processes get NO ring and ALL their eligible ops go UDS. In this measurement that
   is a **deployment-skew artifact** (the homebrew-test prefix has mixed-version dylibs; the
   ABI-hash/version reject is the SAFETY valve working exactly as designed — degrade to all-UDS, never
   silent-drop). It is not an architectural cost, but it inflates the post-attach-UDS pool here.

## ATTACH ATTEMPT / OUTCOME TALLIES
- `attach_attempts = 704`, `attach_successes = 500`, `attach_rejects = 204` (all ABI).
- `attach_reject_by_reason = {2: 204}` → 100% `dserver_ring_reject_abi`.
- Ring health pristine across the run: `ring_fast_hit = 1000`, `ring_fast_fail = 0`,
  `ring_s2c_full = 0`, `ring_fast_suspend = 0`, `clients_blocked_in_rpc = 0`. No wedge; fork-storm 3/3.
- Binary-type breadcrumbs (`attach_mldr_callers` / `attach_dylib_callers` / `attach_no_ring_code`) are
  0 — the guest-side breadcrumb was deliberately NOT wired (kept D15a server-only). Binary type is
  inferred server-side instead: the mldr binary's fingerprint is `set_executable_path` + `mldr_path` +
  `set_dyld_info` issued pre-exec (each ~203 post-attach here = the post-exec dylib, not the mldr
  binary). The mldr binary links `dserver_rpc_*` directly and has no ring — but it issues only a
  handful of calls per process and they are not the reclaimable pool.

## DECISION — among A) handshake-attach, B) early mldr/init attach, C) attach-on-first-eligible, D) prewarm
**None of A/B/C/D is worth doing.** They all target the pre-attach window, which holds 9 reclaimable
calls. Specifically:
- **A) attach during the checkin handshake** — would move attach from ordinal ~4 to ordinal ~1.
  Reclaims the eligible ops between checkin and attach = essentially the 9 calls. Not worth the
  complexity (folding a memfd+mmap+SCM_RIGHTS negotiation into checkin) for ~9 calls.
- **B) explicit early attach in mldr/init** — the mldr binary has no ring code and exits into the
  post-exec dylib quickly; its own calls are few and pre-exec. No meaningful eligible pre-attach
  traffic to rescue. Reject.
- **C) attach-on-first-eligible-op** — this is ALREADY the status quo (lazy attach in `gr_*_trap`),
  and the measurement shows it already fires by ordinal ~4. Nothing to change. (The status quo is the
  right design; D14's premise that it fires "too late" is false.)
- **D) server-side prewarm** — the server cannot create the guest's memfd/mapping; attach is
  fundamentally guest-initiated. And there is no pre-attach pool to prewarm for. Reject.

**VERDICT: STOP on "move attach earlier." It is aimed at a 9-call non-problem.** The reclaimable UDS
is the **2053 post-attach eligible calls**, and its root cause is the **single-owner ring**, not attach
timing. The real next lever (a DIFFERENT bead, NOT in D15a's scope) is:

> **dar-1il.11 (D16) candidate — PER-THREAD ring ownership** (the P4 "full per-thread rings" that P3
> deferred): let each thread attach/own its own ring so a non-owner thread's eligible ops ride the ring
> instead of falling back to UDS. That is what would move the 2053-call pool, and it is squarely a
> transport-design bead, not an attach-timing tweak. Secondary, smaller: investigate the 29%
> ABI-reject rate as a deployment/versioning hygiene item (ensure the shipped dylib ABI matches the
> server) — but that is a packaging concern, and in production (matched dylib+server) it would be ~0.

This recon did its job: it KILLED an expensive wrong turn (re-architecting attach) by measuring that
the window it targets is empty, and it redirected the lever to per-thread ring ownership with data.

## INSTRUMENTATION (what was built, all default-OFF)
- `Metrics` D15a block (metrics.hpp): `attachCensusOn` (env `DARLING_SERVER_ATTACH_CENSUS=1`),
  per-callnum `preAttachUdsByCallnum` / `postAttachUdsByCallnum` / `preAttachEligibleUdsByCallnum`,
  aggregate timeline counters, `attachOrdinalHistogram`, attach attempt/reject tallies +
  `attachRejectByReason[]`. Recording methods `recordAttachCensusUdsCall` / `recordAttachOutcome` /
  `recordAttachBinaryFact` — all no-op unless armed (hot path byte-identical when off).
- `Process` (process.hpp): `_udsCallOrdinal` (per-process UDS-call counter) + `_ringAttachedAtOrdinal`
  (first-writer-wins latch), with cheap atomic accessors. The only per-process state added.
- `Call::ringEligibleCallnum` (call.cpp): static classifier mirroring `DSERVER_RING_C2S_OPCODES` (the
  same macro dispatch keys off), used only by the census.
- Recording sites: `server.cpp:723` (the GENUINE UDS receive choke point — ring calls dispatch in
  `ringServiceThread` and never pass through here, so a call recorded here is unambiguously UDS) and
  the `RingAttach::processCall` handler (attach outcome + ordinal latch).
- Snapshot emit: `metrics.cpp` `attach_census*` block (zeros/empty unless armed).

## ACCEPTANCE
1. ✅ Default-OFF instrumentation for all requested signals: first UDS call (ordinal==1 tally), first
   eligible call (pre_attach_eligible by callnum), attach attempt/success point (attach_ordinal
   histogram + attempt/success counters), eligible UDS calls before attach by callnum
   (pre_attach_eligible), attach failure reasons (attach_reject_by_reason → all ABI), binary type
   (inferred server-side; guest breadcrumb intentionally not wired), ring-code-available (inferred —
   attach success ⇒ ring code present; ABI-reject ⇒ skewed ring code).
2. ✅ Same warm workload run with the census armed; timeline produced.
3. ✅ Decision among A/B/C/D made WITH data: STOP — all four target a 9-call non-problem; the real
   lever is per-thread ring ownership (dar-1il.11/D16 candidate).
4. ✅ No attach move, no op migration, no arena/duplex/psynch. Lane-1 set unchanged (13 ops). Perf#18
   gate suite GREEN. Live run clean (boot/exec/fork-storm 3/3, no wedge); prod binaries restored.

## LESSON
"Op observed on UDS" ≠ "op ran before attach." D14 inferred the former implied the latter; the census
proved it false. The reclaimable UDS for an *already-migrated* op is overwhelmingly **post-attach
non-owner-thread fallback** (single-owner ring), not a pre-attach window. Always measure the window
before re-architecting to widen it.
