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

#include <darlingserver/metrics.hpp>

#include <darlingserver/rpc.h>
// perf #18 D9 (dar-1il.4): the static lane-class table (dserver_ring_op_class / DSERVER_RING_CLASS_*).
// Defined only when rpc.h is in scope (which it is, above), so the heatmap can annotate each measured
// callnum with its KNOWN canon class alongside the MEASURED runtime facts.
#include <darlingserver/rpc-supplement.h>

#include <time.h>
#include <sstream>
#include <cstdio>

DarlingServer::Metrics& DarlingServer::Metrics::shared() {
	static Metrics instance;
	return instance;
}

uint64_t DarlingServer::Metrics::nowMonoUs() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

static void appendHistogram(std::ostringstream& out, const char* name, const DarlingServer::LatencyHistogram& h) {
	out << "  \"" << name << "\": {"
	    << "\"count\": " << h.count()
	    << ", \"mean_us\": " << h.mean()
	    << ", \"p50_us\": " << h.quantile(0.50)
	    << ", \"p95_us\": " << h.quantile(0.95)
	    << ", \"p99_us\": " << h.quantile(0.99)
	    << ", \"max_us\": " << h.max()
	    << "}";
}

std::string DarlingServer::Metrics::snapshotJSON(const std::string& extraGauges) const {
	uint64_t now = nowMonoUs();
	uint64_t uptimeUs = (startMonoUs && now >= startMonoUs) ? (now - startMonoUs) : 0;
	uint64_t lastReply = lastReplyMonoUs.load(std::memory_order_relaxed);
	uint64_t lastReplyAgeMs = (lastReply && now >= lastReply) ? ((now - lastReply) / 1000ULL) : 0;

	std::ostringstream out;
	out << "{\n";
	out << "  \"uptime_s\": " << (uptimeUs / 1000000ULL) << ",\n";
	out << "  \"rpcs_serviced\": " << rpcsServiced.load(std::memory_order_relaxed) << ",\n";
	out << "  \"replies_sent\": " << repliesSent.load(std::memory_order_relaxed) << ",\n";
	out << "  \"messages_received\": " << messagesReceived.load(std::memory_order_relaxed) << ",\n";
	out << "  \"checkins\": " << checkins.load(std::memory_order_relaxed) << ",\n";
	out << "  \"forks\": " << forks.load(std::memory_order_relaxed) << ",\n";
	out << "  \"inline_handled\": " << inlineHandled.load(std::memory_order_relaxed) << ",\n";
	out << "  \"queued_to_pool\": " << queuedToPool.load(std::memory_order_relaxed) << ",\n";
#ifdef DSERVER_RING_TRANSPORT
	out << "  \"ring_serviced_spin\": " << ringServicedSpin.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_serviced_doorbell\": " << ringServicedDoorbell.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_doorbells_received\": " << ringDoorbellsReceived.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_wakes_issued\": " << ringWakesIssued.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_wakes_skipped\": " << ringWakesSkipped.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_fast_hit\": " << ringFastHit.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_fast_fallback\": " << ringFastFallback.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_fast_fail\": " << ringFastFail.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_s2c_full\": " << ringS2cFull.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_fast_suspend\": " << ringFastSuspend.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_s2c\": " << ringDuplexS2c.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_reject\": " << ringDuplexReject.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_parent\": " << ringDuplexParent.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_decline\": " << ringDuplexDecline.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_vmdealloc_parent\": " << ringDuplexVmdeallocParent.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_vmdealloc_decline\": " << ringDuplexVmdeallocDecline.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_vmdealloc_s2c\": " << ringDuplexVmdeallocS2c.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_vmdealloc_final\": " << ringDuplexVmdeallocFinal.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_vmdealloc_timeout\": " << ringDuplexVmdeallocTimeout.load(std::memory_order_relaxed) << ",\n";
	out << "  \"ring_duplex_vmdealloc_disarmed\": " << ringDuplexVmdeallocDisarmed.load(std::memory_order_relaxed) << ",\n";
	out << "  \"s2c_munmap_ring_parent\": " << s2cMunmapRingParent.load(std::memory_order_relaxed) << ",\n";
	out << "  \"s2c_munmap_uds_parent\": " << s2cMunmapUdsParent.load(std::memory_order_relaxed) << ",\n";
	out << "  \"s2c_munmap_no_parent\": " << s2cMunmapNoParent.load(std::memory_order_relaxed) << ",\n";
#ifdef DSERVER_RING_PHASE_PROF
	out << "  \"phase_samples\": " << phaseSamples.load(std::memory_order_relaxed) << ",\n";
	out << "  \"phase_drain_cycles\": " << phaseDrainCycles.load(std::memory_order_relaxed) << ",\n";
	out << "  \"phase_dispatch_cycles\": " << phaseDispatchCycles.load(std::memory_order_relaxed) << ",\n";
	out << "  \"phase_body_cycles\": " << phaseBodyCycles.load(std::memory_order_relaxed) << ",\n";
	out << "  \"phase_publish_cycles\": " << phasePublishCycles.load(std::memory_order_relaxed) << ",\n";
#endif
#endif
	// perf #18 P8 D8: mach_msg_overwrite shape census (always emitted; zeros unless armed by
	// DARLING_SERVER_MSG_CENSUS=1). Read msg_send_only_simple / msg_total to size the reclaimable share.
	out << "  \"msg_census_on\": " << (msgCensusOn.load(std::memory_order_relaxed) ? 1 : 0) << ",\n";
	out << "  \"msg_total\": " << msgTotal.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_send_msg\": " << msgSendMsg.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_rcv_msg\": " << msgRcvMsg.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_send_only\": " << msgSendOnly.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_receive_only\": " << msgReceiveOnly.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_send_receive\": " << msgSendReceive.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_rcv_size_nonzero\": " << msgRcvSizeNonzero.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_blocking_receive\": " << msgBlockingReceive.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_send_only_simple\": " << msgSendOnlySimple.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_send_only_complex\": " << msgSendOnlyComplex.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_send_only_ool\": " << msgSendOnlyOol.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_send_only_port_descriptors\": " << msgSendOnlyPortDesc.load(std::memory_order_relaxed) << ",\n";
	out << "  \"msg_census_hdr_read_fail\": " << msgCensusHdrReadFail.load(std::memory_order_relaxed) << ",\n";
	out << "  \"last_reply_age_ms\": " << lastReplyAgeMs << ",\n";
	if (!extraGauges.empty()) {
		out << "  " << extraGauges << ",\n";
	}
	appendHistogram(out, "rpc_latency", rpcLatency); out << ",\n";
	appendHistogram(out, "checkin_latency", checkinLatency); out << ",\n";
	appendHistogram(out, "fork_latency", forkLatency); out << ",\n";

	// perf #9 (dar-dar6x4-perf-5dq.16): per-call-number breakdown. Only calls actually
	// serviced are emitted, keyed by their callnum string, so the table stays sparse
	// (a build touches maybe 15-20 of the ~80 call numbers). This is what perf #7 reads
	// to decide which calls dominate the 57% recvmsg-wait and are worth spinning for.
	out << "  \"per_call\": {";
	bool first = true;
	for (size_t i = 0; i < kMaxCallNumbers; ++i) {
		uint64_t c = perCallCount[i].load(std::memory_order_relaxed);
		if (c == 0) {
			continue;
		}
		// i is the masked low byte. Most calls resolve directly; the 4 debug calls
		// (77-80) carry the UNMANAGED high bit, so retry with the flag if the plain
		// value is unknown. Anything still unknown is emitted by raw number.
		const char* name = dserver_callnum_to_string(static_cast<dserver_callnum_t>(i));
		if (!name) {
			name = dserver_callnum_to_string(static_cast<dserver_callnum_t>(DSERVER_CALL_UNMANAGED_FLAG | i));
		}
		char numbuf[32];
		if (!name) {
			std::snprintf(numbuf, sizeof(numbuf), "callnum_%zu", i);
			name = numbuf;
		}
		const LatencyHistogram& h = perCallLatency[i];
		out << (first ? "\n" : ",\n");
		first = false;
		out << "    \"" << name << "\": {"
		    << "\"count\": " << c
		    << ", \"mean_us\": " << h.mean()
		    << ", \"p50_us\": " << h.quantile(0.50)
		    << ", \"p95_us\": " << h.quantile(0.95)
		    << ", \"p99_us\": " << h.quantile(0.99)
		    << ", \"max_us\": " << h.max()
		    << "}";
	}
	out << (first ? "}\n" : "\n  }\n");

	// perf #18 D9 (dar-1il.4): global RPC heatmap + lane-eligibility census. Emitted only when armed
	// (heatmapOn, env DARLING_SERVER_RPC_HEATMAP=1) -- when off, this whole block is one cheap branch
	// and the table is `{}` so a reader can tell "armed but empty" from "not armed". For every call
	// number actually serviced while armed, emit BOTH the measured runtime facts (transport split,
	// per-transport p50, used_fiber/caller_s2c counts) AND the KNOWN static canon class (from
	// dserver_ring_op_class), then derive a lane VERDICT that combines them. The verdict is the lane
	// this op COULD ride next; the ranking (count x reclaimable-latency x eligibility) is computed by
	// the offline reader from these fields -- the server stays a dumb, cheap recorder.
	out << ",\n";
	out << "  \"rpc_heatmap_on\": " << (heatmapOn.load(std::memory_order_relaxed) ? 1 : 0) << ",\n";
	out << "  \"rpc_heatmap\": {";
	bool hfirst = true;
	for (size_t i = 0; i < kMaxCallNumbers; ++i) {
		uint64_t uds = perCallUdsCount[i].load(std::memory_order_relaxed);
		uint64_t ring = perCallRingCount[i].load(std::memory_order_relaxed);
		if (uds == 0 && ring == 0) {
			continue;
		}
		const char* name = dserver_callnum_to_string(static_cast<dserver_callnum_t>(i));
		if (!name) {
			name = dserver_callnum_to_string(static_cast<dserver_callnum_t>(DSERVER_CALL_UNMANAGED_FLAG | i));
		}
		char numbuf[32];
		if (!name) {
			std::snprintf(numbuf, sizeof(numbuf), "callnum_%zu", i);
			name = numbuf;
		}
		uint64_t total = uds + ring;
		uint64_t usedFiber = perCallUsedFiber[i].load(std::memory_order_relaxed);
		uint64_t callerS2c = perCallDidCallerS2c[i].load(std::memory_order_relaxed);
		const LatencyHistogram& uh = perCallUdsLatency[i];
		const LatencyHistogram& rh = perCallRingLatency[i];

		// Static canon class for this callnum (0 == unclassified, treat as UDS-only-by-default). The
		// table only lists ring-relevant ops; most hot ops are unclassified and judged by measured facts.
		uint32_t cls = dserver_ring_op_class(static_cast<uint32_t>(i));
		if (!cls) {
			cls = dserver_ring_op_class(static_cast<uint32_t>(DSERVER_CALL_UNMANAGED_FLAG | i));
		}
		const bool clsSimple  = (cls & DSERVER_RING_CLASS_SIMPLE_C2S) != 0;
		const bool clsNoFiber = (cls & DSERVER_RING_CLASS_NOFIBER_FAST) != 0;
		const bool clsDestroy = (cls & DSERVER_RING_CLASS_DESTROY) != 0;
		const bool clsS2c     = (cls & DSERVER_RING_CLASS_CALLER_S2C) != 0;

		// Lane VERDICT: combine the canon class (authoritative when present) with measured facts.
		//  - already-on-ring   : the op is in the simple-ring set and we observed it riding the ring.
		//  - duplex-only       : canon marks it destroy/caller-S2C, OR we MEASURED a caller-S2C -> Lane 2.
		//  - tier2-candidate   : never used a fiber AND never an S2C -> a no-fiber direct-dispatch candidate.
		//  - lane1-candidate   : completed on a fiber, no caller-S2C, currently (mostly) UDS -> Lane 1.
		//  - needs-review      : anything else (mixed/destroy-unknown) -- a human must classify.
		const char* verdict;
		if (clsDestroy || clsS2c || callerS2c > 0) {
			verdict = "duplex-only";
		} else if (clsSimple && ring > 0) {
			verdict = "already-on-ring";
		} else if (usedFiber == 0 && callerS2c == 0) {
			verdict = "tier2-candidate";
		} else if (callerS2c == 0) {
			verdict = "lane1-candidate";
		} else {
			verdict = "needs-review";
		}

		out << (hfirst ? "\n" : ",\n");
		hfirst = false;
		out << "    \"" << name << "\": {"
		    << "\"total\": " << total
		    << ", \"uds\": " << uds
		    << ", \"ring\": " << ring
		    << ", \"used_fiber\": " << usedFiber
		    << ", \"caller_s2c\": " << callerS2c
		    << ", \"uds_p50_us\": " << uh.quantile(0.50)
		    << ", \"uds_p99_us\": " << uh.quantile(0.99)
		    << ", \"ring_p50_us\": " << rh.quantile(0.50)
		    << ", \"ring_p99_us\": " << rh.quantile(0.99)
		    << ", \"class_simple\": " << (clsSimple ? 1 : 0)
		    << ", \"class_nofiber\": " << (clsNoFiber ? 1 : 0)
		    << ", \"class_destroy\": " << (clsDestroy ? 1 : 0)
		    << ", \"class_caller_s2c\": " << (clsS2c ? 1 : 0)
		    << ", \"verdict\": \"" << verdict << "\""
		    << "}";
	}
	out << (hfirst ? "}\n" : "\n  }\n");

	// perf #18 D15a (dar-1il.10): ring-attach TIMELINE / reclaimability census. Emitted only when armed
	// (attachCensusOn, env DARLING_SERVER_ATTACH_CENSUS=1); zeros/empty otherwise so a reader can tell
	// "armed but empty" from "not armed". The decision-critical field is attach_census_pre_attach by
	// callnum (eligible subset) -- the UDS calls that ran before the guest lazily attached its ring and
	// would have ridden the ring had attach happened earlier. The ordinal histogram shows HOW DEEP into
	// each process's UDS-call sequence attach lands.
	out << ",\n";
	out << "  \"attach_census_on\": " << (attachCensusOn.load(std::memory_order_relaxed) ? 1 : 0) << ",\n";
	out << "  \"attach_census_processes\": " << attachCensusProcesses.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_census_first_uds_calls\": " << attachCensusFirstUdsCalls.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_census_total_pre_attach_eligible\": " << attachCensusTotalPreAttachEligible.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_attempts\": " << attachAttempts.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_successes\": " << attachSuccesses.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_rejects\": " << attachRejects.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_mldr_callers\": " << attachMldrCallers.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_dylib_callers\": " << attachDylibCallers.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_no_ring_code\": " << attachNoRingCode.load(std::memory_order_relaxed) << ",\n";
	out << "  \"attach_reject_by_reason\": {";
	{
		bool rfirst = true;
		for (size_t i = 0; i < kMaxRejectReasons; ++i) {
			uint64_t c = attachRejectByReason[i].load(std::memory_order_relaxed);
			if (c == 0) continue;
			out << (rfirst ? "" : ", ") << "\"" << i << "\": " << c;
			rfirst = false;
		}
	}
	out << "},\n";
	appendHistogram(out, "attach_ordinal", attachOrdinalHistogram); out << ",\n";
	// Per-callnum pre/post-attach UDS split + the pre-attach eligible subset (the reclaimable pool).
	out << "  \"attach_census\": {";
	bool afirst = true;
	for (size_t i = 0; i < kMaxCallNumbers; ++i) {
		uint64_t pre = preAttachUdsByCallnum[i].load(std::memory_order_relaxed);
		uint64_t post = postAttachUdsByCallnum[i].load(std::memory_order_relaxed);
		if (pre == 0 && post == 0) continue;
		uint64_t preElig = preAttachEligibleUdsByCallnum[i].load(std::memory_order_relaxed);
		const char* name = dserver_callnum_to_string(static_cast<dserver_callnum_t>(i));
		if (!name) {
			name = dserver_callnum_to_string(static_cast<dserver_callnum_t>(DSERVER_CALL_UNMANAGED_FLAG | i));
		}
		char numbuf[32];
		if (!name) {
			std::snprintf(numbuf, sizeof(numbuf), "callnum_%zu", i);
			name = numbuf;
		}
		out << (afirst ? "\n" : ",\n");
		afirst = false;
		// ring_eligible: derive from the static lane-class table (SIMPLE_C2S members are exactly the
		// C2S allowlist). Keeps Metrics free of any Call coupling. A row with pre_attach_eligible>0 is
		// necessarily eligible; this flag also marks eligible ops whose pre-attach count happens to be 0.
		uint32_t cls = dserver_ring_op_class(static_cast<uint32_t>(i));
		if (!cls) cls = dserver_ring_op_class(static_cast<uint32_t>(DSERVER_CALL_UNMANAGED_FLAG | i));
		bool ringEligible = (cls & DSERVER_RING_CLASS_SIMPLE_C2S) != 0;
		out << "    \"" << name << "\": {"
		    << "\"pre_attach_uds\": " << pre
		    << ", \"post_attach_uds\": " << post
		    << ", \"pre_attach_eligible\": " << preElig
		    << ", \"ring_eligible\": " << (ringEligible ? 1 : 0)
		    << "}";
	}
	out << (afirst ? "}\n" : "\n  }\n");

	// perf #18 D17 (dar-1il.12): POST-D16 residual-UDS classifier. Emitted only when armed
	// (residualCensusOn, env DARLING_SERVER_RESIDUAL_CENSUS=1); zeros otherwise. The decision-critical
	// fields: residual_reason buckets separate reason A (first-eligible-before-this-thread's-lane:
	// thread_no_ring_proc_none + thread_no_ring_proc_has) from B/D (thread_has_ring = an eligible op on
	// UDS despite a live lane). residual_uds_despite_lane (per callnum) names any op exhibiting the B/D
	// signal; if empty, the residual is pure A and unavoidable without an attach-time change (D18).
	out << ",\n";
	out << "  \"residual_census_on\": " << (residualCensusOn.load(std::memory_order_relaxed) ? 1 : 0) << ",\n";
	{
		static const char* const reasonNames[RR_COUNT] = {
			"thread_no_ring_proc_none", // A: whole-process pre-attach (mldr/dyld/first op)
			"thread_no_ring_proc_has",  // A: this thread's lane not up yet (a sibling's is)
			"thread_has_ring",          // B/D: eligible op on UDS despite a live lane
			"control_plane",            // checkin / ring_attach
			"ineligible",               // a non-eligible op on UDS (expected)
		};
		out << "  \"residual_reason\": {";
		bool rrfirst = true;
		for (size_t i = 0; i < RR_COUNT; ++i) {
			out << (rrfirst ? "\n" : ",\n");
			rrfirst = false;
			out << "    \"" << reasonNames[i] << "\": " << residualReason[i].load(std::memory_order_relaxed);
		}
		out << "\n  },\n";
	}
	out << "  \"residual_max_ring_threads_per_process\": " << maxRingThreadsPerProcess.load(std::memory_order_relaxed) << ",\n";
	out << "  \"residual_total_ring_threads_registered\": " << totalRingThreadsRegistered.load(std::memory_order_relaxed) << ",\n";
	out << "  \"residual_uds_despite_lane\": {";
	{
		bool ufirst = true;
		for (size_t i = 0; i < kMaxCallNumbers; ++i) {
			uint64_t c = udsDespiteLaneByCallnum[i].load(std::memory_order_relaxed);
			if (c == 0) continue;
			const char* name = dserver_callnum_to_string(static_cast<dserver_callnum_t>(i));
			if (!name) {
				name = dserver_callnum_to_string(static_cast<dserver_callnum_t>(DSERVER_CALL_UNMANAGED_FLAG | i));
			}
			char numbuf[32];
			if (!name) {
				std::snprintf(numbuf, sizeof(numbuf), "callnum_%zu", i);
				name = numbuf;
			}
			out << (ufirst ? "\n" : ",\n");
			ufirst = false;
			out << "    \"" << name << "\": " << c;
		}
		out << (ufirst ? "}\n" : "\n  }\n");
	}

	out << "}\n";
	return out.str();
}
