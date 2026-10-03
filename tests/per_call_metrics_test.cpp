/**
 * perf #9 (dar-dar6x4-perf-5dq.16) regression test: per-call-number RPC breakdown.
 *
 * Why this exists: the real-build profile (perf #6) showed guest threads spend ~57%
 * of wall-clock asleep in recvmsg waiting for an RPC reply (__skb_wait_for_more_packets).
 * To target the recv-spin tuning (perf #7) at the RIGHT calls we need to know WHICH
 * RPC call numbers dominate -- both by frequency (each call pays a full round-trip
 * wakeup the spin can eliminate) and by server service time. The aggregate rpc_latency
 * histogram cannot tell us that; we need a per-call-number breakdown.
 *
 * RED->GREEN intent (standing TDD rule): before Metrics::recordCall() and the per_call
 * snapshot section exist, this test fails to compile/link (RED). After, it asserts:
 *   1. recordCall(N, us) accumulates an independent count + latency histogram per call
 *      number, so two different call numbers are reported separately (not merged).
 *   2. snapshotJSON() emits a "per_call" object that names each call that was recorded
 *      (by callnum string) with its count, and OMITS calls never seen (so the table
 *      stays small and readable on a real build with ~80 call numbers).
 *
 * Hermetic: links only metrics.cpp; uses the generated rpc.h enum + its inline string
 * table (pure C, no registry/message/logging deps). No boot, no sockets. Exit 0 = GREEN.
 */

#include <darlingserver/metrics.hpp>
#include <darlingserver/rpc.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
		++failures; \
	} \
} while (0)

static bool jsonHas(const std::string& s, const std::string& key) {
	return s.find(key) != std::string::npos;
}

int main() {
	using namespace DarlingServer;

	Metrics& m = Metrics::shared();
	m.startMonoUs = Metrics::nowMonoUs();

	// Record many fast calls of one number and a few slow calls of another, so the
	// per-call table must keep them separate and expose the slow one's tail.
	const uint32_t fastCall = dserver_callnum_checkin;
	const uint32_t slowCall = dserver_callnum_mach_msg_overwrite;

	for (int i = 0; i < 500; ++i) m.recordCall(fastCall, 5);       // 5us each
	for (int i = 0; i < 20;  ++i) m.recordCall(slowCall, 300000);  // 300ms each

	std::string json = m.snapshotJSON("");

	// 1. a per_call section exists
	CHECK(jsonHas(json, "\"per_call\""), "snapshot has per_call section");

	// 2. both recorded calls appear by NAME (so perf#7 can read which calls dominate)
	const char* fastName = dserver_callnum_to_string((dserver_callnum_t)fastCall);
	const char* slowName = dserver_callnum_to_string((dserver_callnum_t)slowCall);
	CHECK(jsonHas(json, std::string("\"") + fastName + "\""), "per_call names the fast call");
	CHECK(jsonHas(json, std::string("\"") + slowName + "\""), "per_call names the slow call");

	// 3. counts are kept separate, not merged: the fast call shows 500
	CHECK(jsonHas(json, "\"count\": 500"), "fast call count == 500 (kept separate)");

	// 4. the slow call's tail is visible: 300ms = 300000us lands in bucket whose upper
	//    edge is 2^19 = 524288 (or, conservatively, 262144). Its presence proves the
	//    histogram is per-call, not the global one.
	CHECK(jsonHas(json, "524288") || jsonHas(json, "262144"),
	      "slow call's hundreds-of-ms latency is recorded per-call");

	// 5. a call that was NEVER recorded must NOT appear (table stays sparse)
	const char* unrecorded = dserver_callnum_to_string((dserver_callnum_t)dserver_callnum_vchroot);
	CHECK(!jsonHas(json, std::string("\"") + unrecorded + "\""),
	      "an unrecorded call does NOT appear in per_call (table stays sparse)");

	if (failures == 0) {
		std::printf("GREEN: per-call metrics breakdown behaves correctly\n");
		return 0;
	}
	std::fprintf(stderr, "RED: %d per-call metric check(s) failed\n", failures);
	return 1;
}
