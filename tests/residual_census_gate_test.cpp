/**
 * perf #18 D17 (dar-1il.12) regression test: POST-D16 residual-UDS classifier.
 *
 * Why this exists: after per-thread lanes (D16) the eligible-UDS pool dropped 1533->231. D17 must answer
 * WHY each residual eligible UDS call is still on UDS so we can tell reason A (unavoidable
 * first-eligible-call-before-this-thread's-lane) from B/D (an eligible op on UDS DESPITE a live lane = a
 * wrapper/coverage gap or forced fallback). recordResidualReason() does that classification from
 * server-observable per-(thread,process) state. This gate pins the classification semantics so the
 * A-vs-B/D verdict stays measurable and can't silently regress:
 *   1. ARMING gate: when NOT armed, recordResidualReason is a no-op.
 *   2. REASON A buckets: an eligible UDS call on a thread with NO ring lands in
 *      thread_no_ring_proc_none (process never attached) or thread_no_ring_proc_has (a sibling has a
 *      lane) -- NOT in the B/D bucket, and it does NOT bump uds_despite_lane.
 *   3. REASON B/D bucket: an eligible UDS call on a thread that HAS a live ring lands in thread_has_ring
 *      AND bumps uds_despite_lane[callnum] (the per-op B/D signal).
 *   4. control-plane / ineligible are bucketed separately (never counted as the A or B/D eligible signal).
 *
 * RED->GREEN: each RED arm (-DRED_*) breaks one invariant and MUST fail. Hermetic: links metrics.cpp.
 * Exit 0 = GREEN.
 */

#define DSERVER_RING_TRANSPORT 1
#include <darlingserver/rpc.h>
#include <darlingserver/rpc-supplement.h>
#include <darlingserver/metrics.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); ++failures; } \
} while (0)

// Read the int value of a key inside the "residual_reason" object.
static long reasonVal(const std::string& s, const char* key) {
	auto bpos = s.find("\"residual_reason\":");
	if (bpos == std::string::npos) return -3;
	auto kPos = s.find(std::string("\"") + key + "\":", bpos);
	if (kPos == std::string::npos) return -1;
	kPos = s.find(':', kPos) + 1;
	return std::strtol(s.c_str() + kPos, nullptr, 10);
}

// Read an int key inside the "residual_uds_despite_lane" object (per callnum name).
static long despiteVal(const std::string& s, const char* op) {
	auto bpos = s.find("\"residual_uds_despite_lane\":");
	if (bpos == std::string::npos) return -3;
	std::string opKey = std::string("\"") + op + "\":";
	auto opPos = s.find(opKey, bpos);
	if (opPos == std::string::npos) return -2; // not present == 0 occurrences
	auto kPos = s.find(':', opPos) + 1;
	return std::strtol(s.c_str() + kPos, nullptr, 10);
}

int main() {
	using namespace DarlingServer;
	Metrics& m = Metrics::shared();
	m.startMonoUs = Metrics::nowMonoUs();

	const uint32_t CN_REPLY = (uint32_t)dserver_callnum_mach_reply_port;       // ring-eligible
	const uint32_t CN_VCHROOT = (uint32_t)dserver_callnum_vchroot_path;        // ring-eligible
	const uint32_t CN_CHECKIN = (uint32_t)dserver_callnum_checkin;             // control plane
	const char* OP_REPLY = "dserver_callnum_mach_reply_port";

	// --- INVARIANT 1: NOT armed -> no-op ---------------------------------------------------------
	CHECK(!m.residualCensusOn.load(std::memory_order_relaxed), "residual census starts disarmed");
	for (int i = 0; i < 20; ++i) m.recordResidualReason(CN_REPLY, /*hasRing*/true, /*procEver*/true, /*elig*/true, /*ctrl*/false);
	{
		std::string j = m.snapshotJSON("");
		CHECK(j.find("\"residual_census_on\": 0") != std::string::npos, "disarmed: residual_census_on==0");
		CHECK(reasonVal(j, "thread_has_ring") == 0, "disarmed: thread_has_ring stays 0");
	}

	// --- arm + drive a representative mix --------------------------------------------------------
	m.residualCensusOn.store(true, std::memory_order_relaxed);

	// REASON A, whole-process pre-attach: eligible, no ring, process never attached.
	for (int i = 0; i < 5; ++i) m.recordResidualReason(CN_REPLY, /*hasRing*/false, /*procEver*/false, /*elig*/true, /*ctrl*/false);
	// REASON A, this-thread-not-yet: eligible, no ring, but a sibling attached (procEver true).
	for (int i = 0; i < 3; ++i) m.recordResidualReason(CN_VCHROOT, /*hasRing*/false, /*procEver*/true, /*elig*/true, /*ctrl*/false);
	// REASON B/D: eligible, but the thread HAS a live ring (an op on UDS despite a lane).
	for (int i = 0; i < 2; ++i) m.recordResidualReason(CN_REPLY, /*hasRing*/true, /*procEver*/true, /*elig*/true, /*ctrl*/false);
	// control-plane: checkin.
	m.recordResidualReason(CN_CHECKIN, /*hasRing*/false, /*procEver*/false, /*elig*/false, /*ctrl*/true);
	// ineligible (a non-eligible op on UDS, not control-plane).
	m.recordResidualReason((uint32_t)dserver_callnum_checkout, /*hasRing*/false, /*procEver*/true, /*elig*/false, /*ctrl*/false);

	std::string j = m.snapshotJSON("");
	CHECK(j.find("\"residual_census_on\": 1") != std::string::npos, "armed: residual_census_on==1");

	// --- INVARIANT 2: reason-A buckets ----------------------------------------------------------
	CHECK(reasonVal(j, "thread_no_ring_proc_none") == 5, "proc-none (whole-process pre-attach) == 5");
	CHECK(reasonVal(j, "thread_no_ring_proc_has") == 3, "proc-has (this-thread-not-yet) == 3");

	// --- INVARIANT 3: reason-B/D bucket + per-op signal -----------------------------------------
	CHECK(reasonVal(j, "thread_has_ring") == 2, "thread_has_ring (B/D signal) == 2");
	CHECK(despiteVal(j, OP_REPLY) == 2, "uds_despite_lane[mach_reply_port] == 2");

	// --- INVARIANT 4: control-plane / ineligible separate ---------------------------------------
	CHECK(reasonVal(j, "control_plane") == 1, "control_plane == 1");
	CHECK(reasonVal(j, "ineligible") == 1, "ineligible == 1");
	// A reason-A call must NOT have bumped the per-op despite-lane signal (only the B/D bucket does).
	CHECK(despiteVal(j, "dserver_callnum_vchroot_path") == -2, "reason-A op never appears in uds_despite_lane");

#ifdef RED_BREAK_DISARMED_NOOP
	{
		Metrics& m2 = Metrics::shared();
		bool saved = m2.residualCensusOn.load(std::memory_order_relaxed);
		m2.residualCensusOn.store(false, std::memory_order_relaxed);
		long before = reasonVal(m2.snapshotJSON(""), "thread_has_ring");
		for (int i = 0; i < 10; ++i) m2.recordResidualReason(CN_REPLY, true, true, true, false);
		long after = reasonVal(m2.snapshotJSON(""), "thread_has_ring");
		m2.residualCensusOn.store(saved, std::memory_order_relaxed);
		CHECK(after == before + 10, "RED arm: disarmed recording must change the count (it must NOT)");
	}
#endif
#ifdef RED_BREAK_AB_SEPARATION
	// RED: claim a thread-HAS-ring eligible call landed in a reason-A bucket (it must land in B/D).
	// With the real classifier the 2 B/D calls are NOT in proc_none, so this over-counts -> fails.
	CHECK(reasonVal(j, "thread_no_ring_proc_none") == 7, "RED arm: B/D call must not be counted as reason A");
#endif

	if (failures == 0) {
		std::printf("GREEN: residual classifier separates reason A (first-before-lane) from B/D (uds-despite-lane) correctly\n");
		return 0;
	}
	std::fprintf(stderr, "RED: %d residual-census check(s) failed\n", failures);
	return 1;
}
