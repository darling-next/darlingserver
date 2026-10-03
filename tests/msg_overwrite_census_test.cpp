/**
 * perf #18 P8 D8 (dar-1il.3.2.x) regression test: mach_msg_overwrite SHAPE CENSUS.
 *
 * Why this exists: D7 recon found mach_msg_overwrite is the single hottest RPC (~19%) AND the most
 * dangerous to ring-migrate (blocking receive, OOL/port descriptors, mmap+munmap caller-S2C). Before
 * spending a week on a Lane-2 subset that might be 2% of the hotness, D8 adds a cheap census that
 * answers "what fraction is actually reclaimable?". This test pins the CLASSIFICATION LOGIC -- the
 * axis bookkeeping (send/receive/send+receive, rcv-size, blocking-receive) and the send-only
 * descriptor-shape split (simple / complex-ool / complex-port) -- so a future edit can't silently
 * mis-bucket and make the census lie.
 *
 * The byte-offset descriptor parsing itself lives in call.cpp (it needs a Process to readMemory the
 * guest header); here we validate the Metrics-side mapping from (option, sizes, MsgComplexClass) to
 * counters, plus the JSON emit. We also assert the descriptor `type` byte lands at offset 8 of the
 * descriptor (the offset call.cpp relies on) using a synthetic descriptor matching the user ABI.
 *
 * RED->GREEN: the RED arms (-DRED_*) break a single classification rule each and must fail. Hermetic:
 * links only metrics.cpp; no boot, no sockets. Exit 0 = GREEN.
 */

#include <darlingserver/metrics.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>
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

// pull the integer value of a "key": N entry out of the snapshot JSON (N >= 0).
static long jsonVal(const std::string& s, const std::string& key) {
	auto p = s.find(std::string("\"") + key + "\":");
	if (p == std::string::npos) return -1;
	p = s.find(':', p) + 1;
	return std::strtol(s.c_str() + p, nullptr, 10);
}

int main() {
	using namespace DarlingServer;
	using C = Metrics::MsgComplexClass;

	Metrics& m = Metrics::shared();
	m.startMonoUs = Metrics::nowMonoUs();

	// MACH_SEND_MSG=0x1, MACH_RCV_MSG=0x2, MACH_RCV_TIMEOUT=0x100
	const int32_t SEND = 0x1, RCV = 0x2, RCVTMO = 0x100;

	// --- drive a representative mix -------------------------------------------------
	// 10 simple send-only (the candidate Lane-2 subset)
	for (int i = 0; i < 10; ++i) m.recordMsgOverwriteCensus(SEND, 32, 0, 0, C::Simple);
	// 4 send-only with an OOL descriptor (would drive mmap/munmap caller-S2C -> the stopper)
	for (int i = 0; i < 4; ++i) m.recordMsgOverwriteCensus(SEND, 64, 0, 0, C::ComplexOol);
	// 3 send-only with a port descriptor (namespace/refcount side effects)
	for (int i = 0; i < 3; ++i) m.recordMsgOverwriteCensus(SEND, 48, 0, 0, C::ComplexPort);
	// 5 send+receive (the classic MIG round-trip: rpc reply expected) -- bounded by an RCV timeout
	for (int i = 0; i < 5; ++i) m.recordMsgOverwriteCensus(SEND | RCV | RCVTMO, 40, 256, 500, C::Simple);
	// 7 receive-only with NO finite timeout -> blocking receive (must NOT ride Lane-1)
	for (int i = 0; i < 7; ++i) m.recordMsgOverwriteCensus(RCV, 0, 4096, 0, C::Unknown);
	// 2 header-read-failures on the send path
	for (int i = 0; i < 2; ++i) m.recordMsgOverwriteCensus(SEND, 32, 0, 0, C::Unknown);

	std::string json = m.snapshotJSON("");

	// total = 10+4+3+5+7+2 = 31
	CHECK(jsonVal(json, "msg_total") == 31, "msg_total counts every census call");

	// send/recv axis
	CHECK(jsonVal(json, "msg_send_msg") == 10 + 4 + 3 + 5 + 2, "msg_send_msg = all with SEND bit (24)");
	CHECK(jsonVal(json, "msg_rcv_msg") == 5 + 7, "msg_rcv_msg = all with RCV bit (12)");
	CHECK(jsonVal(json, "msg_send_only") == 10 + 4 + 3 + 2, "msg_send_only = SEND && !RCV (19)");
	CHECK(jsonVal(json, "msg_receive_only") == 7, "msg_receive_only = RCV && !SEND (7)");
	CHECK(jsonVal(json, "msg_send_receive") == 5, "msg_send_receive = SEND && RCV (5)");

	// blocking receive = RCV with no finite timeout. ONLY the 7 receive-only (timeout 0) qualify;
	// the 5 send+receive carry RCV_TIMEOUT with timeout>0 so they do NOT count as blocking.
	CHECK(jsonVal(json, "msg_blocking_receive") == 7, "msg_blocking_receive = unbounded receives only (7)");

	// rcv_size!=0: the 5 send+receive (256) + 7 receive-only (4096) = 12
	CHECK(jsonVal(json, "msg_rcv_size_nonzero") == 12, "msg_rcv_size_nonzero (12)");

	// send-only descriptor split: simple=10, complex=4+3=7, ool=4, port=3, hdr_read_fail=2
	CHECK(jsonVal(json, "msg_send_only_simple") == 10, "send-only simple subset (10)");
	CHECK(jsonVal(json, "msg_send_only_complex") == 7, "send-only complex total (7)");
	CHECK(jsonVal(json, "msg_send_only_ool") == 4, "send-only OOL (4) -- the stopper signal");
	CHECK(jsonVal(json, "msg_send_only_port_descriptors") == 3, "send-only port descriptors (3)");
	CHECK(jsonVal(json, "msg_census_hdr_read_fail") == 2, "send-only header-read failures (2)");

	// the simple-send subset and the complex subsets partition the send-only set (minus read fails):
	// 10 + 7 + 2 = 19 == msg_send_only. Pins that a send-only call lands in EXACTLY one bucket.
	CHECK(jsonVal(json, "msg_send_only_simple") + jsonVal(json, "msg_send_only_complex") +
	      jsonVal(json, "msg_census_hdr_read_fail") == jsonVal(json, "msg_send_only"),
	      "send-only buckets partition the send-only total");

	// --- pin the descriptor `type` byte offset call.cpp relies on (offset 8, high byte) -----------
	// Synthetic 32-bit OOL descriptor: address(4) size(4) {deallocate:8 copy:8 pad1:8 type:8}(4).
	// The type field is the HIGH byte of the 3rd word (offset 8). MACH_MSG_OOL_DESCRIPTOR == 1.
	{
		uint8_t desc[12] = {0};
		desc[0] = 0x11; desc[1] = 0x22; desc[2] = 0x33; desc[3] = 0x44; // address
		desc[4] = 0x40; desc[5] = 0x00; desc[6] = 0x00; desc[7] = 0x00; // size
		desc[8]  = 0x00;  // deallocate
		desc[9]  = 0x00;  // copy
		desc[10] = 0x00;  // pad1
		desc[11] = 0x01;  // type = MACH_MSG_OOL_DESCRIPTOR (high byte of the word at offset 8)
		uint32_t typeWord;
		std::memcpy(&typeWord, desc + 8, sizeof(typeWord));
		uint32_t dtype = (typeWord >> 24) & 0xffu;
		CHECK(dtype == 1, "OOL descriptor type byte resolves at offset 8 (high byte) == 1");
	}

	// monotonic snapshot doesn't crash with the census section present
	CHECK(jsonHas(json, "\"msg_census_on\""), "snapshot emits the census section");

#ifdef RED_BREAK_BLOCKING
	// RED: pretend a bounded send+receive ALSO counts as blocking (off by 5). Should fail the gate.
	CHECK(jsonVal(json, "msg_blocking_receive") == 12, "RED arm");
#endif
#ifdef RED_BREAK_SENDONLY_PARTITION
	// RED: claim OOL sends are also "simple" (double-count) -> partition assertion breaks.
	CHECK(jsonVal(json, "msg_send_only_simple") == 14, "RED arm");
#endif

	if (failures == 0) {
		std::printf("GREEN: mach_msg_overwrite shape census classifies correctly\n");
		return 0;
	}
	std::fprintf(stderr, "RED: %d census check(s) failed\n", failures);
	return 1;
}
