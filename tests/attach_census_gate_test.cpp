/**
 * perf #18 D15a (dar-1il.10) regression test: ring-attach TIMELINE / reclaimability census.
 *
 * Why this exists: D14 INFERRED that the reclaimable UDS tail was pre-attach and recommended moving
 * attach earlier. D15a measured the pre-attach window directly and found it ~empty (9 eligible calls);
 * the reclaimable UDS is post-attach non-owner-thread fallback. This test pins the census recording
 * semantics so that conclusion stays measurable and the metric can't silently regress:
 *   1. ARMING gate: when NOT armed, recordAttachCensusUdsCall / recordAttachOutcome are no-ops.
 *   2. PRE/POST split: a UDS call recorded with attachedYet=false lands in pre_attach (and, if
 *      eligible, pre_attach_eligible + the total pool); attachedYet=true lands in post_attach.
 *   3. ELIGIBLE accounting: an INELIGIBLE pre-attach call must NOT bump pre_attach_eligible.
 *   4. ATTACH outcome: success bumps successes + the ordinal histogram; reject bumps rejects +
 *      reject_by_reason[reason].
 *
 * RED->GREEN: each RED arm (-DRED_*) breaks one invariant and MUST fail. Hermetic: links metrics.cpp
 * only. Exit 0 = GREEN.
 */

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

// Read an int key from within a specific callnum's attach_census object.
static long acVal(const std::string& s, const char* op, const char* key) {
	std::string blk = "\"attach_census\":";
	auto bpos = s.find(blk);
	if (bpos == std::string::npos) return -3;
	std::string opKey = std::string("\"") + op + "\":";
	auto opPos = s.find(opKey, bpos);
	if (opPos == std::string::npos) return -2;
	auto kPos = s.find(std::string("\"") + key + "\":", opPos);
	if (kPos == std::string::npos) return -1;
	kPos = s.find(':', kPos) + 1;
	return std::strtol(s.c_str() + kPos, nullptr, 10);
}

// Read a top-level int key (aggregate counter).
static long topVal(const std::string& s, const char* key) {
	auto kPos = s.find(std::string("\"") + key + "\":");
	if (kPos == std::string::npos) return -1;
	kPos = s.find(':', kPos) + 1;
	return std::strtol(s.c_str() + kPos, nullptr, 10);
}

int main() {
	using namespace DarlingServer;
	Metrics& m = Metrics::shared();
	m.startMonoUs = Metrics::nowMonoUs();

	const char* OP_REPLY = "dserver_callnum_mach_reply_port"; // ring-eligible
	const char* OP_CHECKIN = "dserver_callnum_checkin";       // NOT ring-eligible
	const uint32_t CN_REPLY = (uint32_t)dserver_callnum_mach_reply_port;
	const uint32_t CN_CHECKIN = (uint32_t)dserver_callnum_checkin;

	// --- INVARIANT 1: NOT armed -> recording is a no-op ------------------------------------------
	CHECK(!m.attachCensusOn.load(std::memory_order_relaxed), "attach census starts disarmed");
	for (int i = 0; i < 20; ++i) m.recordAttachCensusUdsCall(CN_REPLY, i + 1, false, true);
	m.recordAttachOutcome(true, 0, 3);
	{
		std::string j = m.snapshotJSON("");
		CHECK(j.find("\"attach_census_on\": 0") != std::string::npos, "disarmed: attach_census_on==0");
		CHECK(acVal(j, OP_REPLY, "pre_attach_uds") == -2, "disarmed: no attach_census rows");
		CHECK(topVal(j, "attach_successes") == 0, "disarmed: no attach successes recorded");
	}

	// --- arm and drive a representative mix ------------------------------------------------------
	m.attachCensusOn.store(true, std::memory_order_relaxed);

	// 5 PRE-attach eligible mach_reply_port (attachedYet=false, eligible=true).
	for (int i = 0; i < 5; ++i) m.recordAttachCensusUdsCall(CN_REPLY, i + 1, /*attachedYet*/false, /*eligible*/true);
	// 7 POST-attach eligible mach_reply_port (attachedYet=true).
	for (int i = 0; i < 7; ++i) m.recordAttachCensusUdsCall(CN_REPLY, i + 10, /*attachedYet*/true, /*eligible*/true);
	// 3 PRE-attach INELIGIBLE checkin (eligible=false) -- must NOT bump pre_attach_eligible.
	for (int i = 0; i < 3; ++i) m.recordAttachCensusUdsCall(CN_CHECKIN, i + 1, /*attachedYet*/false, /*eligible*/false);

	// attach outcomes: 2 successes (at ordinals 4 and 8), 1 reject (reason abi==2).
	m.recordAttachOutcome(true, 0, 4);
	m.recordAttachOutcome(true, 0, 8);
	m.recordAttachOutcome(false, (uint32_t)dserver_ring_reject_abi, 0);

	std::string j = m.snapshotJSON("");
	CHECK(j.find("\"attach_census_on\": 1") != std::string::npos, "armed: attach_census_on==1");

	// --- INVARIANT 2: pre/post split -------------------------------------------------------------
	CHECK(acVal(j, OP_REPLY, "pre_attach_uds") == 5, "reply pre_attach_uds==5");
	CHECK(acVal(j, OP_REPLY, "post_attach_uds") == 7, "reply post_attach_uds==7");
	CHECK(acVal(j, OP_REPLY, "pre_attach_eligible") == 5, "reply pre_attach_eligible==5");
	CHECK(acVal(j, OP_REPLY, "ring_eligible") == 1, "reply is ring_eligible");
	CHECK(topVal(j, "attach_census_total_pre_attach_eligible") == 5, "total pre-attach eligible pool==5");

	// --- INVARIANT 3: ineligible pre-attach call does NOT bump pre_attach_eligible ----------------
	CHECK(acVal(j, OP_CHECKIN, "pre_attach_uds") == 3, "checkin pre_attach_uds==3");
	CHECK(acVal(j, OP_CHECKIN, "pre_attach_eligible") == 0, "checkin pre_attach_eligible==0 (ineligible)");
	CHECK(acVal(j, OP_CHECKIN, "ring_eligible") == 0, "checkin is NOT ring_eligible");

	// --- INVARIANT 4: attach outcomes -------------------------------------------------------------
	CHECK(topVal(j, "attach_attempts") == 3, "attach_attempts==3");
	CHECK(topVal(j, "attach_successes") == 2, "attach_successes==2");
	CHECK(topVal(j, "attach_rejects") == 1, "attach_rejects==1");
	CHECK(topVal(j, "attach_census_processes") == 2, "attach_census_processes==2 (one per success)");
	// ordinal histogram recorded 2 samples (4 and 8):
	CHECK(j.find("\"attach_ordinal\":") != std::string::npos, "attach_ordinal histogram present");

#ifdef RED_BREAK_DISARMED_NOOP
	// RED: claim the disarmed path recorded (it must not).
	{
		Metrics& m2 = Metrics::shared();
		bool saved = m2.attachCensusOn.load(std::memory_order_relaxed);
		m2.attachCensusOn.store(false, std::memory_order_relaxed);
		long before = acVal(m2.snapshotJSON(""), OP_REPLY, "pre_attach_uds");
		for (int i = 0; i < 10; ++i) m2.recordAttachCensusUdsCall(CN_REPLY, i + 1, false, true);
		long after = acVal(m2.snapshotJSON(""), OP_REPLY, "pre_attach_uds");
		m2.attachCensusOn.store(saved, std::memory_order_relaxed);
		CHECK(after == before + 10, "RED arm: disarmed recording must change the count (it must NOT)");
	}
#endif
#ifdef RED_BREAK_ELIGIBLE_SPLIT
	// RED: claim the INELIGIBLE pre-attach checkin bumped pre_attach_eligible (it must stay 0).
	CHECK(acVal(j, OP_CHECKIN, "pre_attach_eligible") == 3, "RED arm: ineligible call must not count as eligible");
#endif

	if (failures == 0) {
		std::printf("GREEN: attach census records pre/post split + eligible pool + attach outcomes correctly\n");
		return 0;
	}
	std::fprintf(stderr, "RED: %d attach-census check(s) failed\n", failures);
	return 1;
}
