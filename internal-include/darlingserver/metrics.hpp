/**
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling developers
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Darling is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Darling.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _DARLINGSERVER_METRICS_HPP_
#define _DARLINGSERVER_METRICS_HPP_

// perf #0 (dar-dar6x4-perf-5dq.6): in-server progress + timing metrics.
//
// Why this exists: from OUTSIDE the server you cannot reliably tell "the Darling
// workload is slow but progressing" from "it is genuinely wedged" -- log-file size,
// CPU%, and single /proc snapshots all conflate the two (this misdiagnosed dar-l3a as
// a stall when gettext was simply building slowly). The robust signal is a monotonic
// count of work actually done plus the latency of that work, read straight from the
// server over a dedicated UNIX socket.
//
// Design constraints:
//  - hot-path increments must be cheap and lock-free -> std::atomic<uint64_t> with
//    relaxed ordering (these are statistics, not synchronization).
//  - latency is recorded into a fixed log2-bucketed histogram (no per-sample storage,
//    no allocation); quantiles are computed from the buckets at snapshot time.
//  - the snapshot is produced on the main event-loop thread (the stat socket is just
//    another epoll fd), so reading atomics there is naturally consistent enough.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <array>

namespace DarlingServer {
	// A lock-free latency histogram with log2 buckets.
	// bucket i counts samples whose value (in microseconds) is in [2^(i-1), 2^i)
	// (bucket 0 = [0,1)). With 32 buckets we cover up to ~2^31 us (~35 min), which
	// is far beyond any sane per-RPC latency.
	class LatencyHistogram {
	public:
		static constexpr size_t kBucketCount = 32;

		void record(uint64_t microseconds) {
			size_t idx = bucketFor(microseconds);
			_buckets[idx].fetch_add(1, std::memory_order_relaxed);
			_count.fetch_add(1, std::memory_order_relaxed);
			_sum.fetch_add(microseconds, std::memory_order_relaxed);
			// monotonic running max
			uint64_t prev = _max.load(std::memory_order_relaxed);
			while (microseconds > prev && !_max.compare_exchange_weak(prev, microseconds, std::memory_order_relaxed)) {
				// prev was reloaded by compare_exchange_weak; loop
			}
		}

		uint64_t count() const { return _count.load(std::memory_order_relaxed); }
		uint64_t sum()   const { return _sum.load(std::memory_order_relaxed); }
		uint64_t max()   const { return _max.load(std::memory_order_relaxed); }
		uint64_t mean()  const {
			uint64_t c = count();
			return (c == 0) ? 0 : (sum() / c);
		}

		// Approximate quantile (in microseconds) from the buckets. We return the upper
		// edge of the bucket in which the requested rank falls -- an over-estimate
		// bounded by the bucket width (<=2x), which is fine for "is it 5us or 500ms".
		uint64_t quantile(double q) const {
			uint64_t c = count();
			if (c == 0) {
				return 0;
			}
			uint64_t target = static_cast<uint64_t>(q * static_cast<double>(c));
			if (target >= c) {
				target = c - 1;
			}
			uint64_t cumulative = 0;
			for (size_t i = 0; i < kBucketCount; ++i) {
				cumulative += _buckets[i].load(std::memory_order_relaxed);
				if (cumulative > target) {
					return bucketUpperEdge(i);
				}
			}
			return bucketUpperEdge(kBucketCount - 1);
		}

	private:
		static size_t bucketFor(uint64_t microseconds) {
			if (microseconds == 0) {
				return 0;
			}
			// index = floor(log2(us)) + 1, clamped to the last bucket
			size_t idx = 0;
			uint64_t v = microseconds;
			while (v > 0) {
				++idx;
				v >>= 1;
			}
			// now idx == floor(log2(us)) + 1
			return (idx >= kBucketCount) ? (kBucketCount - 1) : idx;
		}

		static uint64_t bucketUpperEdge(size_t i) {
			if (i == 0) {
				return 1;
			}
			if (i >= kBucketCount - 1) {
				return UINT64_C(1) << (kBucketCount - 1);
			}
			return UINT64_C(1) << i;
		}

		std::array<std::atomic<uint64_t>, kBucketCount> _buckets {};
		std::atomic<uint64_t> _count {0};
		std::atomic<uint64_t> _sum {0};
		std::atomic<uint64_t> _max {0};
	};

	class Metrics {
	public:
		static Metrics& shared();

		// ---- monotonic counters (hot path) ----
		std::atomic<uint64_t> rpcsServiced {0};   // RPC calls fully processed
		std::atomic<uint64_t> repliesSent {0};    // reply datagrams sent
		std::atomic<uint64_t> checkins {0};       // checkin RPCs (process/thread registration)
		std::atomic<uint64_t> forks {0};          // fork-checkins specifically
		std::atomic<uint64_t> messagesReceived {0};
		// perf #2b (dar-dar6x4-perf-5dq.8): inline fast-path accounting. A call run to
		// completion directly on the main event loop (no worker wakeup) increments
		// inlineHandled; a call that suspended its microthread (so it must finish on the
		// worker pool when resumed) increments queuedToPool. Under a healthy fast-path the
		// vast majority of cheap RPCs (checkin etc.) should be inlineHandled.
		std::atomic<uint64_t> inlineHandled {0};
		std::atomic<uint64_t> queuedToPool {0};

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 P4 (dar-dar6x4-perf-5dq.33): wake-model accounting. ringServicedSpin =
		// requests drained by the main-loop spin phase (the HOT path: guest skipped the
		// doorbell, server found the request by polling). ringServicedDoorbell = requests
		// drained by the eventfd Monitor callback (the COLD path: guest doorbelled because the
		// server was sleeping/armed). ringWakesIssued/ringWakesSkipped = FUTEX_WAKE on the
		// reply path actually done vs elided because no guest was parked. A healthy hot stream
		// is mostly ringServicedSpin + ringWakesSkipped; both prove zero per-call syscalls.
		std::atomic<uint64_t> ringServicedSpin {0};
		std::atomic<uint64_t> ringServicedDoorbell {0};
		std::atomic<uint64_t> ringWakesIssued {0};
		std::atomic<uint64_t> ringWakesSkipped {0};
		std::atomic<uint64_t> ringDoorbellsReceived {0}; // eventfd wakes the server actually drained

		// perf #18 P6.1 step 2 (dar-ohp): surgical direct-dispatch fast path for mach_reply_port.
		// ringFastHit = a mach_reply_port request serviced WITHOUT building a Call/Message or
		// re-looking-up the thread in the registry (doMachReplyPortInline -> dtape trap -> ring
		// publish). ringFastFallback = a request that matched the callnum but failed the shape
		// guard or whose inline dispatch declined -> took the generic step-1 callFromMessage path.
		std::atomic<uint64_t> ringFastHit {0};
		std::atomic<uint64_t> ringFastFallback {0};
		// perf #18 P7 (dar-my8): the rest of the gist-required per-op fast-path accounting.
		// ringFastFail  = an inline fast op that could not complete cleanly (declined late / publish
		//                 lost) -- distinct from a fallback (which is a deliberate route to generic).
		// ringS2cFull   = publishReply() found the s2c ring full (reply could not be published there;
		//                 the caller UDS-falls-back). A nonzero value under load = guest not draining.
		// ringFastSuspend = an op the fast path ran as non-blocking actually SUSPENDED -- a contract
		//                 violation (allowlist misclassification). MUST stay 0; nonzero = a bug.
		std::atomic<uint64_t> ringFastFail {0};
		std::atomic<uint64_t> ringS2cFull {0};
		std::atomic<uint64_t> ringFastSuspend {0};
		// perf #18 P8 D3 (dar-1il.3.1.1): the duplex lane. ringDuplexS2c = an S2C upcall delivered to
		// a ring-parked caller via the duplex mailbox (instead of the UDS S2C path) AND completed (the
		// correlated reply came back + the server resumed). ringDuplexReject = a duplex reply that
		// failed correlation (wrong parent/upcall id) -- the server refused to resume on it and fell
		// the op back to UDS. Both MUST be 0 unless the duplex selftest is actively driven; a nonzero
		// ringDuplexS2c on a normal boot would mean a real op wrongly took the duplex path (a bug --
		// the conjunction guard is supposed to gate it to the env-driven selftest only).
		std::atomic<uint64_t> ringDuplexS2c {0};
		std::atomic<uint64_t> ringDuplexReject {0};
		// perf #18 P8 D4 (dar-1il.3.2.1): duplex PARENT routing counters (distinct from the S2C-upcall
		// counters above). ringDuplexParent = a real op (mach_port_deallocate) was DISPATCHED onto the
		// duplex lane (the caller advertised the cap, the shape was accepted, it ran on the fiber). This
		// is the proof a deallocate actually RODE the duplex lane -- it bumps even when the op needs NO
		// S2C (the common refcount>1 case), unlike ringDuplexS2c which only counts an actual munmap S2C.
		// ringDuplexDecline = a duplex-routed op was DECLINED pre-dispatch (no cap / bad shape) and a
		// DSERVER_RING_DUPLEX_DECLINE reply was sent so the guest UDS-falls-back (no mutation, no
		// double-effect). NOTE: in Darling, mach_port_deallocate does not drive a munmap S2C in practice
		// (mach_make_memory_entry_64 is stubbed), so ringDuplexParent can be >0 while ringDuplexS2c stays
		// 0 -- the deallocate rode the lane but never needed the caller-S2C the lane exists to service.
		std::atomic<uint64_t> ringDuplexParent {0};
		std::atomic<uint64_t> ringDuplexDecline {0};
		// perf #18 P8 D5/D6: the vm_deallocate-via-duplex proof harness
		// counters (option 1, gist 3e928115). The proof is armed by a MARKER FILE (never inherited env) and
		// AUTO-DISARMS after the first successful caller-S2C. A GREEN proof is:
		//   s2cMunmapToCaller>=1 AND ringDuplexVmdeallocParent>=1 AND _s2c>=1 AND _final>=1 AND _timeout==0
		//   AND boot reaches shellspawn.
		// vmdeallocParent  = a launchd vm_deallocate was DISPATCHED onto the duplex lane (rode the lane).
		// vmdeallocDecline = a vm_deallocate routing was DECLINED pre-dispatch (not armed / bad shape / budget 0).
		// vmdeallocS2c     = a real UPCALL_MUNMAP for a vm_deallocate parent completed over the duplex mailbox.
		// vmdeallocFinal   = the parent vm_deallocate op's final ring reply was published (op finished on lane).
		// vmdeallocTimeout = a duplex munmap upcall reply was NOT harvested in the bound -> failed closed.
		// vmdeallocDisarmed= the proof auto-disarmed (budget hit 0 after a success) -> no further routing.
		std::atomic<uint64_t> ringDuplexVmdeallocParent {0};
		std::atomic<uint64_t> ringDuplexVmdeallocDecline {0};
		std::atomic<uint64_t> ringDuplexVmdeallocS2c {0};
		std::atomic<uint64_t> ringDuplexVmdeallocFinal {0};
		std::atomic<uint64_t> ringDuplexVmdeallocTimeout {0};
		std::atomic<uint64_t> ringDuplexVmdeallocDisarmed {0};
		// perf #18 P8 D6 (caller-S2C sideband) ATTRIBUTION: for every caller-S2C munmap upcall (_s2cPerform
		// munmap to the CURRENT thread), classify the active PARENT op's lane. munmapRingParent = the caller
		// has an active ring-originated parent (_ringReplyPending) -> the direct cure target for the duplex
		// sideband. munmapUdsParent = an active UDS-originated call (today serviced via recvmsg = NOT a
		// ring-deadlock, but a FUTURE hazard if that op is ring-migrated). munmapNoParent = no active managed
		// call (pure server-internal teardown). The measured Darling boot: ALL caller-S2C munmaps go under a
		// UDS parent (mach_msg_overwrite OOL teardown) -> the hazard is reachable but UDS-only today.
		std::atomic<uint64_t> s2cMunmapRingParent {0};
		std::atomic<uint64_t> s2cMunmapUdsParent {0};
		std::atomic<uint64_t> s2cMunmapNoParent {0};

#endif // DSERVER_RING_TRANSPORT

		// perf #18 P8 D8 (dar-1il.3.2.x): mach_msg_overwrite SHAPE CENSUS. A pure measurement to
		// answer "what fraction of the ~19% msg_overwrite hotness is actually reclaimable by a ring
		// migration?" -- NOT a behavior change and NOT gated on DSERVER_RING_TRANSPORT (msg_overwrite
		// rides UDS today, so the census must count on the UDS path too). It is OFF by default and only
		// classifies when armed by env DARLING_SERVER_MSG_CENSUS=1 on a warm server (Metrics::msgCensusOn);
		// when off, the classification (incl. the cheap header readMemory for the COMPLEX bit/descriptors)
		// is skipped entirely so the hot path is byte-identical to today. Buckets are NOT mutually
		// exclusive across axes -- a single call bumps msgTotal plus the axis bits that apply, so the
		// reader cross-tabulates. The decision fork (see PERF18-MSG-OVERWRITE-RECON.md / the D8 brief):
		//   high msgSendOnlySimple share  -> worth a future Lane-2/simple-send subset build
		//   high msgSendOnlyOol share     -> must extend the D6 sideband to mmap FIRST (the stopper)
		//   high msgReceive/blocking share-> STOP, receive stays UDS/generic, look elsewhere
		std::atomic<bool> msgCensusOn {false};      // armed by DARLING_SERVER_MSG_CENSUS=1 (warm-server hatch)
		std::atomic<uint64_t> msgTotal {0};         // every mach_msg_overwrite seen by the census
		std::atomic<uint64_t> msgSendMsg {0};       // option & MACH_SEND_MSG
		std::atomic<uint64_t> msgRcvMsg {0};        // option & MACH_RCV_MSG
		std::atomic<uint64_t> msgSendOnly {0};      // SEND_MSG && !RCV_MSG
		std::atomic<uint64_t> msgReceiveOnly {0};   // RCV_MSG && !SEND_MSG
		std::atomic<uint64_t> msgSendReceive {0};   // SEND_MSG && RCV_MSG
		std::atomic<uint64_t> msgRcvSizeNonzero {0};// rcv_size != 0
		std::atomic<uint64_t> msgBlockingReceive {0};// RCV_MSG && NOT (RCV_TIMEOUT with finite timeout) -> can park unbounded
		// send-only sub-classification (the candidate Lane-2 subset lives in msgSendOnlySimple):
		std::atomic<uint64_t> msgSendOnlySimple {0};// SEND_MSG && !RCV_MSG && !COMPLEX (no descriptors)
		std::atomic<uint64_t> msgSendOnlyComplex {0};// SEND_MSG && !RCV_MSG && COMPLEX (has descriptors)
		std::atomic<uint64_t> msgSendOnlyOol {0};   // ... COMPLEX && >=1 OOL (memory) descriptor -> drives mmap/munmap S2C
		std::atomic<uint64_t> msgSendOnlyPortDesc {0};// ... COMPLEX && >=1 port / ool-ports descriptor -> namespace/refcount
		std::atomic<uint64_t> msgCensusHdrReadFail {0};// header readMemory failed (classified as complex-unknown, counted here)

		// ---- perf #18 D15a (dar-1il.10): ring-attach TIMELINE / reclaimability census ----
		// RECON-ONLY. The guest attaches its ring LAZILY on its first eligible op (gr_*_trap ->
		// __dserver_ring_try_attach), so every process runs a burst of UDS calls before the ring
		// exists. D14 showed the reclaimable UDS tail is dominated by this pre-attach window. This
		// census measures the window so D15 can choose among: A) attach during the checkin handshake,
		// B) explicit early attach in mldr/init, C) attach-on-first-eligible-op (status quo, just
		// earlier), D) server-side prewarm. OFF by default; armed by DARLING_SERVER_ATTACH_CENSUS=1 on
		// a warm server (attachCensusOn). When off, recording is skipped so the hot path is unchanged.
		// All buckets are lock-free atomics; recorded at the callFromMessage UDS choke point + the
		// ring_attach handler. "pre-attach" for a call == its process has not yet latched a successful
		// ring_attach at the moment the call arrives (Process::ringAttachedYet()).
		std::atomic<bool> attachCensusOn {false};   // armed by DARLING_SERVER_ATTACH_CENSUS=1
		// Per-callnum tables (preAttachUdsByCallnum / postAttachUdsByCallnum / preAttachEligibleUdsByCallnum)
		// are declared further down, after kMaxCallNumbers is in scope. Aggregate timeline facts:
		std::atomic<uint64_t> attachCensusProcesses {0};      // distinct processes whose ring_attach SUCCEEDED (= timeline samples)
		std::atomic<uint64_t> attachCensusFirstUdsCalls {0};  // count of "ordinal==1" UDS calls seen (== distinct processes that made >=1 UDS call)
		std::atomic<uint64_t> attachCensusTotalPreAttachEligible {0}; // total pre-attach eligible UDS calls across all processes (the reclaimable pool)
		// Histogram of the UDS-call ordinal at which ring_attach succeeded (1 = attached before/at its
		// first UDS call; large = many UDS calls ran first). log2 buckets reuse LatencyHistogram.
		LatencyHistogram attachOrdinalHistogram;
		// ring_attach attempt/outcome tallies (the handler sees every attach RPC).
		std::atomic<uint64_t> attachAttempts {0};   // ring_attach RPCs processed
		std::atomic<uint64_t> attachSuccesses {0};  // accepted (ring mapped, thread registered)
		std::atomic<uint64_t> attachRejects {0};    // rejected for any reason
		// reject reasons, indexed by dserver_ring_reject_t (small enum; 16 covers it with margin).
		static constexpr size_t kMaxRejectReasons = 16;
		std::array<std::atomic<uint64_t>, kMaxRejectReasons> attachRejectByReason {};
		// Guest-reported binary facts (set via a tiny env-gated checkin-time breadcrumb; OPTIONAL --
		// if the guest side is not wired, these stay 0 and the doc notes the binary type was inferred
		// server-side from the early-call fingerprint instead). attachMldrCallers / attachDylibCallers
		// are mutually exclusive guesses; attachNoRingCode = a caller that reported no ring transport
		// compiled in (e.g. the mldr binary).
		std::atomic<uint64_t> attachMldrCallers {0};
		std::atomic<uint64_t> attachDylibCallers {0};
		std::atomic<uint64_t> attachNoRingCode {0};

		// Forward copy of kMaxCallNumbers (the canonical one is declared later, next to perCallLatency);
		// the D17 per-callnum array below needs the size in scope here. static_assert below the canonical
		// definition pins the two equal so they can never drift.
		static constexpr size_t kMaxCallNumbersFwd = 256;

		// perf #18 D17 (dar-1il.12): POST-D16 RESIDUAL UDS classifier (default-OFF; armed by
		// DARLING_SERVER_RESIDUAL_CENSUS=1). After per-thread lanes (D16) the eligible-UDS pool dropped
		// 1533->231; D17 answers WHY each residual eligible UDS call is still on UDS, to decide if it is
		// (A) unavoidable first-eligible-call-before-this-thread's-lane-attaches, (B) a wrapper coverage
		// gap, (C) lane exhaustion, or (D) a call site in a no-ring image (mldr). The classification is
		// done from SERVER-OBSERVABLE per-(thread,process) state at the UDS receive choke point -- it needs
		// NO lane-ownership change (a D17 constraint). The reason buckets are mutually exclusive per call:
		enum ResidualReason : uint32_t {
			RR_THREAD_NO_RING_PROC_NONE = 0, // thread has no ring AND its process never attached one yet
			                                 //   => whole-process pre-attach window (mldr/dyld phase, or the
			                                 //   very first eligible op of a fresh process). REASON A.
			RR_THREAD_NO_RING_PROC_HAS  = 1, // thread has no ring NOW but a SIBLING thread's lane is up
			                                 //   (process attached >=1). => this thread's first eligible op
			                                 //   before ITS OWN lane attaches. REASON A (per-thread variant).
			RR_THREAD_HAS_RING          = 2, // the thread HAS a live ring yet this eligible op still came over
			                                 //   UDS. This is the B/D signal: either a call SITE that does not
			                                 //   use the ring wrapper, or a transient ring-full forced fallback.
			RR_CONTROL_PLANE            = 3, // checkin / ring_attach themselves (not eligible; counted for shape)
			RR_INELIGIBLE               = 4, // a non-eligible op on UDS (expected; counted for completeness)
			RR_COUNT                    = 5,
		};
		std::atomic<bool> residualCensusOn {false};
		std::array<std::atomic<uint64_t>, RR_COUNT> residualReason {};
		// Per-callnum count of the RR_THREAD_HAS_RING bucket (the only B/D-interesting one): an eligible op
		// that came UDS despite the thread already owning a lane. If this is ~0 the residual is pure
		// first-before-attach (A); if a specific op shows up here it is a wrapper gap / forced fallback (B/D).
		std::array<std::atomic<uint64_t>, kMaxCallNumbersFwd> udsDespiteLaneByCallnum {};
		// Peak number of threads with a live ring in any single process (a server-side proxy for the guest's
		// "max lanes/process" -- the server registers one ring thread per attached guest thread). Set at
		// snapshot time from Server (it owns the per-process thread sets); a gauge, not a hot-path counter.
		std::atomic<uint64_t> maxRingThreadsPerProcess {0};
		std::atomic<uint64_t> totalRingThreadsRegistered {0}; // cumulative ring-thread registrations (a server-side "lanes acquired" proxy)

		// Record one UDS-transported call for the attach-timeline census. `callNumber` raw; `ordinal`
		// is the process's 1-based UDS-call sequence number; `attachedYet` is whether the process had
		// already latched a successful ring_attach; `eligible` is static ring-eligibility. No-op unless
		// armed. Called from callFromMessage (the UDS choke point -- ring calls never pass through it).
		void recordAttachCensusUdsCall(uint32_t callNumber, uint64_t ordinal, bool attachedYet, bool eligible) {
			if (!attachCensusOn.load(std::memory_order_relaxed)) {
				return;
			}
			size_t idx = callNumber & 0xffu;
			if (idx >= kMaxCallNumbers) {
				return;
			}
			if (ordinal == 1) {
				attachCensusFirstUdsCalls.fetch_add(1, std::memory_order_relaxed);
			}
			if (attachedYet) {
				postAttachUdsByCallnum[idx].fetch_add(1, std::memory_order_relaxed);
			} else {
				preAttachUdsByCallnum[idx].fetch_add(1, std::memory_order_relaxed);
				if (eligible) {
					preAttachEligibleUdsByCallnum[idx].fetch_add(1, std::memory_order_relaxed);
					attachCensusTotalPreAttachEligible.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		// Record a ring_attach outcome (called from the RingAttach handler). On success, `ordinal` is
		// the process's UDS-call ordinal AT attach time (how many UDS calls preceded the ring). No-op
		// unless armed.
		void recordAttachOutcome(bool success, uint32_t rejectReason, uint64_t ordinalAtAttach) {
			if (!attachCensusOn.load(std::memory_order_relaxed)) {
				return;
			}
			attachAttempts.fetch_add(1, std::memory_order_relaxed);
			if (success) {
				attachSuccesses.fetch_add(1, std::memory_order_relaxed);
				attachCensusProcesses.fetch_add(1, std::memory_order_relaxed);
				attachOrdinalHistogram.record(ordinalAtAttach);
			} else {
				attachRejects.fetch_add(1, std::memory_order_relaxed);
				if (rejectReason < kMaxRejectReasons) {
					attachRejectByReason[rejectReason].fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		// Record a guest-reported binary fact at checkin (env-gated guest breadcrumb; optional).
		void recordAttachBinaryFact(bool isMldr, bool hasRingCode) {
			if (!attachCensusOn.load(std::memory_order_relaxed)) {
				return;
			}
			if (isMldr) attachMldrCallers.fetch_add(1, std::memory_order_relaxed);
			else        attachDylibCallers.fetch_add(1, std::memory_order_relaxed);
			if (!hasRingCode) attachNoRingCode.fetch_add(1, std::memory_order_relaxed);
		}

		// perf #18 D17: classify ONE UDS-transported call by the reason it is still on UDS, from
		// server-observable per-(thread,process) state at the UDS receive choke point. `threadHasRing` =
		// the thread owns a live ring NOW; `procEverAttached` = the process latched >=1 successful
		// ring_attach (Process::ringAttachedYet); `eligible` = static ring-eligibility; `controlPlane` =
		// the call IS checkin/ring_attach. No-op unless the residual census is armed. The reason buckets
		// are mutually exclusive; the per-callnum udsDespiteLane table is bumped only for the B/D-signal
		// bucket (eligible op on a thread that already has a ring).
		void recordResidualReason(uint32_t callNumber, bool threadHasRing, bool procEverAttached,
		                          bool eligible, bool controlPlane) {
			if (!residualCensusOn.load(std::memory_order_relaxed)) {
				return;
			}
			ResidualReason r;
			if (controlPlane) {
				r = RR_CONTROL_PLANE;
			} else if (!eligible) {
				r = RR_INELIGIBLE;
			} else if (threadHasRing) {
				r = RR_THREAD_HAS_RING; // eligible op on UDS despite a live lane -> B/D signal
				size_t idx = callNumber & 0xffu;
				if (idx < kMaxCallNumbersFwd) {
					udsDespiteLaneByCallnum[idx].fetch_add(1, std::memory_order_relaxed);
				}
			} else if (procEverAttached) {
				r = RR_THREAD_NO_RING_PROC_HAS; // this thread's lane not up yet (sibling's is) -> A
			} else {
				r = RR_THREAD_NO_RING_PROC_NONE; // whole-process pre-attach (mldr/dyld/first op) -> A
			}
			residualReason[r].fetch_add(1, std::memory_order_relaxed);
		}

#ifdef DSERVER_RING_TRANSPORT
#ifdef DSERVER_RING_PHASE_PROF
		// perf #18 P6 (dar-aw2): cycle-decompose the hot ring RPC. rdtsc brackets in
		// ringServiceThread/publishReply accumulate per-phase TSC cycles + a sample count, so we
		// can read mean cycles/phase from the stat socket. Pure diagnostic (default OFF, this whole
		// block compiles out); the brackets read the TSC, which is far below a phase's own cost so
		// they don't perturb the breakdown the way clock_gettime would.
		//   drain    = consumer_begin + copy the slot transport header out
		//   dispatch = callFromMessage (rebuild Message, registry lookup, decode)
		//   body     = doWork() -- the inline Mach operation itself
		//   publish  = publishReply (claim s2c slot, write reply, release-store)
		std::atomic<uint64_t> phaseDrainCycles {0};
		std::atomic<uint64_t> phaseDispatchCycles {0};
		std::atomic<uint64_t> phaseBodyCycles {0};
		std::atomic<uint64_t> phasePublishCycles {0};
		std::atomic<uint64_t> phaseSamples {0};
#endif
#endif

		// ---- gauges sampled at snapshot time (set by the owner) ----
		// These are filled in by Server when producing a snapshot, not on the hot path.

		// ---- latency histograms ----
		LatencyHistogram rpcLatency;       // service time of any RPC (worker entry -> reply)
		LatencyHistogram checkinLatency;   // service time of checkin RPCs
		LatencyHistogram forkLatency;      // service time of fork-checkin RPCs (incl. parent coordination)

		// ---- per-call-number breakdown (perf #9 / dar-dar6x4-perf-5dq.16) ----
		// The real-build profile (perf #6) put ~57% of guest wall-clock in recvmsg
		// waiting for an RPC reply. To aim the recv-spin tuning (perf #7) at the calls
		// that actually dominate, we record, per call number, how many times it was
		// serviced and the distribution of its server-side service time. Frequency is
		// the key lever: every call pays one full round-trip wakeup that the spin can
		// elide, so the most NUMEROUS calls are the ones worth spinning for.
		//
		// Call numbers are sequential 1..~80 (the high UNMANAGED bit is masked off when
		// indexing), so a fixed lock-free array indexed by the low bits is both cheap
		// and allocation-free on the hot path. 256 covers the whole range with margin.
		static constexpr size_t kMaxCallNumbers = 256;
		static_assert(kMaxCallNumbers == kMaxCallNumbersFwd, "D17 forward copy of kMaxCallNumbers drifted");
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> perCallCount {};
		std::array<LatencyHistogram, kMaxCallNumbers> perCallLatency {};

		// perf #18 D15a (dar-1il.10): attach-timeline per-callnum tables (declared here so kMaxCallNumbers
		// is in scope). preAttach = UDS calls seen BEFORE this process's ring attached (reclaimable-if-
		// attach-were-earlier); postAttach = residual UDS once the ring exists (foreign-thread/non-owner
		// or ineligible); preAttachEligible = the subset of preAttach that is ring-eligible (the pool a
		// migration of attach-time would actually move onto the ring). See recordAttachCensusUdsCall.
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> preAttachUdsByCallnum {};
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> postAttachUdsByCallnum {};
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> preAttachEligibleUdsByCallnum {};

		// Record one serviced RPC of the given call number with the given service time.
		// callNumber is the raw dserver_callnum value (the UNMANAGED flag, if set, is
		// masked off for indexing). Safe to call from any worker; lock-free.
		void recordCall(uint32_t callNumber, uint64_t microseconds) {
			size_t idx = callNumber & 0xffu; // low byte: call numbers are < 256
			if (idx >= kMaxCallNumbers) {
				return;
			}
			perCallCount[idx].fetch_add(1, std::memory_order_relaxed);
			perCallLatency[idx].record(microseconds);
		}

		// perf #18 P8 D8: how the caller classified the message body's descriptor shape (resolved by
		// the caller, which holds the Process needed to readMemory the message header -- keeps Metrics
		// free of Process coupling). Unknown = census off or header read failed.
		enum class MsgComplexClass { Unknown, Simple, ComplexOol, ComplexPort, ComplexOther };

		// Record one mach_msg_overwrite for the shape census. Cheap, lock-free; no-op unless armed.
		// `option`/`send_size`/`rcv_size`/`timeout` are the inline RPC args; `cclass` is the body
		// classification (only meaningful for the send path). NOT mutually exclusive across axes.
		void recordMsgOverwriteCensus(int32_t option, uint32_t send_size, uint32_t rcv_size, uint32_t timeout, MsgComplexClass cclass) {
			// MACH_SEND_MSG = 0x1, MACH_RCV_MSG = 0x2, MACH_RCV_TIMEOUT = 0x100 (mach/message.h).
			const bool send = (option & 0x1) != 0;
			const bool recv = (option & 0x2) != 0;
			const bool rcvTimeout = (option & 0x100) != 0;
			msgTotal.fetch_add(1, std::memory_order_relaxed);
			if (send) msgSendMsg.fetch_add(1, std::memory_order_relaxed);
			if (recv) msgRcvMsg.fetch_add(1, std::memory_order_relaxed);
			if (send && !recv) msgSendOnly.fetch_add(1, std::memory_order_relaxed);
			if (recv && !send) msgReceiveOnly.fetch_add(1, std::memory_order_relaxed);
			if (send && recv) msgSendReceive.fetch_add(1, std::memory_order_relaxed);
			if (rcv_size != 0) msgRcvSizeNonzero.fetch_add(1, std::memory_order_relaxed);
			// "blocking receive" = a receive that can park unbounded: RCV_MSG with no finite timeout
			// (either RCV_TIMEOUT unset, or set but timeout==0 which XNU treats as a poll -- so a true
			// unbounded park is RCV_MSG && !(RCV_TIMEOUT && timeout>0)).
			if (recv && !(rcvTimeout && timeout > 0)) msgBlockingReceive.fetch_add(1, std::memory_order_relaxed);
			if (send && !recv) {
				switch (cclass) {
					case MsgComplexClass::Simple:       msgSendOnlySimple.fetch_add(1, std::memory_order_relaxed); break;
					case MsgComplexClass::ComplexOol:   msgSendOnlyComplex.fetch_add(1, std::memory_order_relaxed); msgSendOnlyOol.fetch_add(1, std::memory_order_relaxed); break;
					case MsgComplexClass::ComplexPort:  msgSendOnlyComplex.fetch_add(1, std::memory_order_relaxed); msgSendOnlyPortDesc.fetch_add(1, std::memory_order_relaxed); break;
					case MsgComplexClass::ComplexOther: msgSendOnlyComplex.fetch_add(1, std::memory_order_relaxed); break;
					case MsgComplexClass::Unknown:      msgCensusHdrReadFail.fetch_add(1, std::memory_order_relaxed); break;
				}
			}
		}

		// ---- perf #18 D9 (dar-1il.4): global RPC heatmap + lane-eligibility census ----
		// Goal: stop guessing the next op to ring-migrate; rank candidates by DATA. The D8 census
		// taught the lesson that "op is hot" != "op is reclaimable" (mach_msg_overwrite was the hottest
		// RPC but ~88% blocking-receive -> only ~1.6% of all RPC reclaimable -> STOP). So D9 measures, per
		// call number, BOTH hotness (already in perCallCount/perCallLatency) AND the runtime facts that
		// decide lane eligibility:
		//   - TRANSPORT split: how many of this op's calls were serviced over the ring vs UDS today. An op
		//     already mostly-ring has little headroom; a hot op still 100% UDS is the prize.
		//   - latency split per transport: uds_p50 vs ring_p50 sizes the per-call win a migration buys.
		//   - used_fiber: did the op suspend its microthread (generic doWork) rather than run inline? A
		//     blocking/fiber op is NOT a Tier-2 no-fiber candidate.
		//   - did_caller_s2c: did servicing this op drive a caller-S2C upcall (the duplex-lane hazard)? A
		//     nonzero count means the op needs the duplex lane (Lane 2), not the simple ring (Lane 1).
		// These are ACCUMULATED per call number, lock-free, and -- like the D8 census -- only recorded
		// when ARMED (heatmapOn, env DARLING_SERVER_RPC_HEATMAP=1 on a warm server). When off, the
		// recording is skipped entirely so the hot path is byte-identical to today (no behavior change).
		// The remaining eligibility axes (may_block, destroy-capable, complex/OOL payload, closed
		// req->reply) are STATIC properties of a call number, so they are classified once at snapshot
		// time from a table -- not threaded through the hot path. See snapshotJSON / classifyCallnum.
		std::atomic<bool> heatmapOn {false}; // armed by DARLING_SERVER_RPC_HEATMAP=1 (warm-server hatch)
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> perCallUdsCount {};      // serviced over UDS
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> perCallRingCount {};     // serviced over the ring (any lane)
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> perCallUsedFiber {};     // suspended the microthread (generic doWork)
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> perCallDidCallerS2c {};  // drove a caller-S2C upcall while servicing
		std::array<LatencyHistogram, kMaxCallNumbers> perCallUdsLatency {};         // service time on the UDS path
		std::array<LatencyHistogram, kMaxCallNumbers> perCallRingLatency {};        // service time on the ring path

		// Transport tag for the heatmap (what lane actually serviced this call).
		enum class CallTransport { Uds, Ring };

		// Record one serviced RPC for the D9 heatmap. Cheap, lock-free; no-op unless armed. Called
		// alongside recordCall() from the same service-completion sites, passing the runtime facts those
		// sites already know (which transport drained it, whether it suspended a fiber, whether it drove a
		// caller-S2C). `usedFiber`/`didCallerS2c` are best-effort booleans, not perfect profiling.
		void recordCallHeatmap(uint32_t callNumber, uint64_t microseconds, CallTransport transport, bool usedFiber, bool didCallerS2c) {
			if (!heatmapOn.load(std::memory_order_relaxed)) {
				return;
			}
			size_t idx = callNumber & 0xffu;
			if (idx >= kMaxCallNumbers) {
				return;
			}
			if (transport == CallTransport::Ring) {
				perCallRingCount[idx].fetch_add(1, std::memory_order_relaxed);
				perCallRingLatency[idx].record(microseconds);
			} else {
				perCallUdsCount[idx].fetch_add(1, std::memory_order_relaxed);
				perCallUdsLatency[idx].record(microseconds);
			}
			if (usedFiber)    perCallUsedFiber[idx].fetch_add(1, std::memory_order_relaxed);
			if (didCallerS2c) perCallDidCallerS2c[idx].fetch_add(1, std::memory_order_relaxed);
		}

		// perf #18 D14 (dar-1il.9): per-CALL-scoped caller-S2C attribution. The D9 caller_s2c column
		// used to be a sticky per-thread bool (_heatmapCallDidS2c) latched in _s2cPerform and consumed
		// at the NEXT recordCall -- which OVER-ATTRIBUTED an exec/teardown munmap S2C to whatever op was
		// recorded next on that thread (D13 saw mldr_path caller_s2c=3 frozen while ring calls grew to
		// 284, even though the transport-correct s2c_munmap_ring_parent stayed 0). This bumps the bucket
		// for the op that is ACTUALLY executing at the moment the S2C fires (the active call's number),
		// so there is no cross-op leak. Call from _s2cPerform with the active parent's callnum. Cheap,
		// lock-free; no-op unless armed -- same guard as recordCallHeatmap.
		void recordCallerS2cFor(uint32_t callNumber) {
			if (!heatmapOn.load(std::memory_order_relaxed)) {
				return;
			}
			size_t idx = callNumber & 0xffu;
			if (idx >= kMaxCallNumbers) {
				return;
			}
			perCallDidCallerS2c[idx].fetch_add(1, std::memory_order_relaxed);
		}

		// last reply timestamp (CLOCK_MONOTONIC microseconds), for last_reply_age_ms
		std::atomic<uint64_t> lastReplyMonoUs {0};

		// process start timestamp (CLOCK_MONOTONIC microseconds)
		uint64_t startMonoUs {0};

		// Current monotonic time in microseconds.
		static uint64_t nowMonoUs();

#ifdef DSERVER_RING_PHASE_PROF
		// perf #18 P6: raw TSC read for sub-microsecond phase brackets. x86-only diagnostic.
		static inline uint64_t rdtscCycles() {
#if defined(__x86_64__) || defined(__i386__)
			unsigned hi, lo;
			__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
			return ((uint64_t)hi << 32) | lo;
#else
			return nowMonoUs() * 1000; // fallback (coarse)
#endif
		}
#endif

		// Build a JSON snapshot string. `extra` lets the caller inject gauges it owns
		// (workqueue_depth, workers_busy/total, clients_blocked_in_rpc) as already-
		// formatted "\"key\": value" fragments, comma-separated, no surrounding braces.
		std::string snapshotJSON(const std::string& extraGauges) const;

	private:
		Metrics() = default;
	};
};

#endif // _DARLINGSERVER_METRICS_HPP_
