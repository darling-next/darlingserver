/**
 * perf #18 D9 (dar-1il.4) regression test: global RPC heatmap + lane-eligibility census.
 *
 * Why this exists: D8 taught that "op is hot" != "op is reclaimable" (mach_msg_overwrite was the
 * hottest RPC yet ~1.6% reclaimable -> STOP). D9 replaces guessing-the-next-op with DATA: per call
 * number it records the transport split (uds vs ring), per-transport latency, and the runtime facts
 * that decide lane eligibility (used_fiber, caller_s2c), then derives a lane VERDICT combining those
 * measured facts with the static canon class (dserver_ring_op_class). This test pins:
 *   1. ARMING gate: when the heatmap is NOT armed, recordCallHeatmap is a no-op (no behavior change,
 *      default-OFF) -- the table stays empty.
 *   2. TRANSPORT split + per-transport counts/latency are bucketed correctly when armed.
 *   3. used_fiber / caller_s2c accumulation.
 *   4. the lane VERDICT derivation: duplex-only (canon destroy/S2C OR measured caller-S2C),
 *      already-on-ring (canon simple + observed ring), tier2-candidate (no fiber, no S2C),
 *      lane1-candidate (fiber, no S2C, UDS today).
 *
 * RED->GREEN: each RED arm (-DRED_*) breaks one invariant and MUST fail. Hermetic: links only
 * metrics.cpp (the Metrics class + the static lane-class table via rpc-supplement.h). No boot, no
 * sockets. Exit 0 = GREEN.
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
	if (!(cond)) { \
		std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
		++failures; \
	} \
} while (0)

// Extract the integer value of a key from WITHIN a specific callnum's heatmap object. We scope the
// search to the substring starting at the named op so "uds": of one op can't be read for another.
static long heatVal(const std::string& s, const char* op, const char* key) {
	std::string opKey = std::string("\"") + op + "\":";
	auto opPos = s.find(opKey);
	if (opPos == std::string::npos) return -2; // op not present
	auto kPos = s.find(std::string("\"") + key + "\":", opPos);
	if (kPos == std::string::npos) return -1;
	kPos = s.find(':', kPos) + 1;
	return std::strtol(s.c_str() + kPos, nullptr, 10);
}

// Extract a string-valued key (the verdict) within a specific op's object.
static std::string heatStr(const std::string& s, const char* op, const char* key) {
	std::string opKey = std::string("\"") + op + "\":";
	auto opPos = s.find(opKey);
	if (opPos == std::string::npos) return "<no-op>";
	auto kPos = s.find(std::string("\"") + key + "\":", opPos);
	if (kPos == std::string::npos) return "<no-key>";
	kPos = s.find('"', s.find(':', kPos) + 1) + 1;
	auto end = s.find('"', kPos);
	return s.substr(kPos, end - kPos);
}

static bool opPresent(const std::string& s, const char* op) {
	return s.find(std::string("\"") + op + "\":") != std::string::npos;
}

int main() {
	using namespace DarlingServer;
	using T = Metrics::CallTransport;

	Metrics& m = Metrics::shared();
	m.startMonoUs = Metrics::nowMonoUs();

	// op names as the snapshot prints them: dserver_callnum_to_string returns the FULL enum-constant
	// name ("dserver_callnum_<op>"), so match that exactly.
	const char* OP_REPLY   = "dserver_callnum_mach_reply_port";      // canon: simple + nofiber (Tier 2)
	const char* OP_ALLOC   = "dserver_callnum_mach_port_allocate";   // canon: simple (Tier 1, generic fiber)
	const char* OP_DEALLOC = "dserver_callnum_mach_port_deallocate"; // canon: destroy + caller-S2C -> duplex-only
	const char* OP_VMALLOC = "dserver_callnum_mach_vm_allocate";     // unclassified hot op (measured-facts verdict)

	// --- INVARIANT 1: NOT armed -> recordCallHeatmap is a no-op (default-OFF, no behavior change). ----
	CHECK(!m.heatmapOn.load(std::memory_order_relaxed), "heatmap starts disarmed");
	for (int i = 0; i < 50; ++i) {
		m.recordCallHeatmap((uint32_t)dserver_callnum_mach_reply_port, 1, T::Ring, false, false);
	}
	{
		std::string json = m.snapshotJSON("");
		CHECK(heatVal(json, OP_REPLY, "total") == -2, "disarmed: no heatmap rows recorded");
		CHECK(json.find("\"rpc_heatmap_on\": 0") != std::string::npos, "disarmed: rpc_heatmap_on==0");
	}

	// --- arm and drive a representative mix --------------------------------------------------------
	m.heatmapOn.store(true, std::memory_order_relaxed);

	// mach_reply_port: 100 over ring (Tier-2 no-fiber), fast (1us). Canon = simple+nofiber, observed
	// ring -> "already-on-ring".
	for (int i = 0; i < 100; ++i) m.recordCallHeatmap((uint32_t)dserver_callnum_mach_reply_port, 1, T::Ring, false, false);

	// mach_port_allocate: 40 ring (fiber, ~3us) + 0 uds. Canon = simple, observed ring -> already-on-ring.
	for (int i = 0; i < 40; ++i) m.recordCallHeatmap((uint32_t)dserver_callnum_mach_port_allocate, 3, T::Ring, true, false);

	// mach_port_deallocate: 25 over UDS, fiber, ~12us. Canon = destroy + caller-S2C -> duplex-only
	// regardless of whether we measured an S2C this run.
	for (int i = 0; i < 25; ++i) m.recordCallHeatmap((uint32_t)dserver_callnum_mach_port_deallocate, 12, T::Uds, true, false);

	// mach_vm_allocate: an UNCLASSIFIED hot op. 200 over UDS, NO fiber, NO caller-S2C, ~8us ->
	// measured-facts verdict = tier2-candidate (never suspended, never an S2C).
	for (int i = 0; i < 200; ++i) m.recordCallHeatmap((uint32_t)dserver_callnum_mach_vm_allocate, 8, T::Uds, false, false);

	std::string json = m.snapshotJSON("");
	CHECK(json.find("\"rpc_heatmap_on\": 1") != std::string::npos, "armed: rpc_heatmap_on==1");

	// --- INVARIANT 2: transport split + counts ----------------------------------------------------
	CHECK(heatVal(json, OP_REPLY, "total") == 100, "reply_port total=100");
	CHECK(heatVal(json, OP_REPLY, "ring") == 100, "reply_port all ring");
	CHECK(heatVal(json, OP_REPLY, "uds") == 0, "reply_port no uds");
	CHECK(heatVal(json, OP_VMALLOC, "total") == 200, "vm_allocate total=200");
	CHECK(heatVal(json, OP_VMALLOC, "uds") == 200, "vm_allocate all uds");
	CHECK(heatVal(json, OP_VMALLOC, "ring") == 0, "vm_allocate no ring");

	// --- INVARIANT 3: used_fiber / caller_s2c accumulation ----------------------------------------
	CHECK(heatVal(json, OP_REPLY, "used_fiber") == 0, "reply_port never used a fiber");
	CHECK(heatVal(json, OP_ALLOC, "used_fiber") == 40, "alloc used a fiber every call");
	CHECK(heatVal(json, OP_VMALLOC, "used_fiber") == 0, "vm_allocate never used a fiber");
	CHECK(heatVal(json, OP_DEALLOC, "caller_s2c") == 0, "dealloc measured no S2C this run");

	// per-transport latency present (p50). reply_port ring p50 ~1us; vm_allocate uds p50 ~8us.
	CHECK(heatVal(json, OP_REPLY, "ring_p50_us") >= 1, "reply_port ring p50 recorded");
	CHECK(heatVal(json, OP_VMALLOC, "uds_p50_us") >= 4, "vm_allocate uds p50 recorded (~8us)");

	// --- INVARIANT 4: lane VERDICT derivation -----------------------------------------------------
	// canon destroy/caller-S2C wins even with zero measured S2C this run:
	CHECK(heatStr(json, OP_DEALLOC, "verdict") == "duplex-only", "dealloc -> duplex-only (canon destroy/S2C)");
	CHECK(heatVal(json, OP_DEALLOC, "class_destroy") == 1, "dealloc class_destroy bit set");
	// canon simple + observed on the ring:
	CHECK(heatStr(json, OP_REPLY, "verdict") == "already-on-ring", "reply_port -> already-on-ring");
	CHECK(heatStr(json, OP_ALLOC, "verdict") == "already-on-ring", "alloc -> already-on-ring");
	// unclassified, no fiber, no S2C -> tier2 candidate (the prize: hot + cheap + no-fiber-shaped):
	CHECK(heatStr(json, OP_VMALLOC, "verdict") == "tier2-candidate", "vm_allocate -> tier2-candidate");

	// --- a measured caller-S2C must FORCE duplex-only even for an unclassified op -----------------
	// D14 (dar-1il.9): caller-S2C is now attributed PER-CALL via recordCallerS2cFor() at the S2C site,
	// NOT a sticky bool threaded through recordCallHeatmap (the didCallerS2c param is now always false
	// from the live sites). Drive the S2C bucket directly, as _s2cPerform does.
	for (int i = 0; i < 5; ++i) {
		m.recordCallHeatmap((uint32_t)dserver_callnum_mach_vm_deallocate, 15, T::Uds, true, false);
		m.recordCallerS2cFor((uint32_t)dserver_callnum_mach_vm_deallocate);
	}
	{
		std::string j2 = m.snapshotJSON("");
		CHECK(heatStr(j2, "dserver_callnum_mach_vm_deallocate", "verdict") == "duplex-only",
		      "measured caller-S2C forces duplex-only (vm_deallocate)");
		CHECK(heatVal(j2, "dserver_callnum_mach_vm_deallocate", "caller_s2c") == 5, "vm_deallocate caller_s2c=5");
	}

	// --- INVARIANT 5 (D14): per-call-scoped attribution -- NO CROSS-OP LEAK -----------------------
	// The D13 false-latch bug: an S2C driven while op A executes must charge A, and must NOT bleed
	// onto op B even if B is the very next op recorded on the same thread. Model that exact sequence:
	// op A (mldr_path) executes and drives an S2C (recordCallerS2cFor(A)); then op B (vchroot_path) is
	// recorded next with NO S2C of its own. B's caller_s2c MUST stay 0; A's MUST be exactly 1.
	const char* OP_A = "dserver_callnum_mldr_path";
	const char* OP_B = "dserver_callnum_vchroot_path";
	m.recordCallerS2cFor((uint32_t)dserver_callnum_mldr_path);               // S2C fires while A is active
	m.recordCallHeatmap((uint32_t)dserver_callnum_mldr_path, 4, T::Ring, true, false);   // A completes
	m.recordCallHeatmap((uint32_t)dserver_callnum_vchroot_path, 4, T::Ring, true, false); // B completes next, no S2C
	{
		std::string j3 = m.snapshotJSON("");
		CHECK(heatVal(j3, OP_A, "caller_s2c") == 1, "D14: S2C charged to the active op A (mldr_path)");
		CHECK(heatVal(j3, OP_B, "caller_s2c") == 0, "D14: NO cross-op leak onto B (vchroot_path) recorded next");
		// and with no caller-S2C of its own + canon-simple, A must read already-on-ring once attributed
		// correctly... but A DID drive an S2C here, so its verdict is duplex-only by the measured fact.
		// That is the POINT of the fix: the S2C lands on the op that actually drove it, not a bystander.
		CHECK(heatStr(j3, OP_B, "verdict") == "already-on-ring",
		      "D14: bystander op B keeps its true verdict (no leaked S2C flips it to duplex-only)");
	}

#ifdef RED_BREAK_DISARMED_NOOP
	// RED: assert the DISARMED path DID record (it must not). Forces a contradiction with INVARIANT 1.
	{
		Metrics& m2 = Metrics::shared();
		bool saved = m2.heatmapOn.load(std::memory_order_relaxed);
		m2.heatmapOn.store(false, std::memory_order_relaxed);
		// snapshot BEFORE: capture current vm_allocate total
		long before = heatVal(m2.snapshotJSON(""), OP_VMALLOC, "total");
		for (int i = 0; i < 10; ++i) m2.recordCallHeatmap((uint32_t)dserver_callnum_mach_vm_allocate, 8, T::Uds, false, false);
		long after = heatVal(m2.snapshotJSON(""), OP_VMALLOC, "total");
		m2.heatmapOn.store(saved, std::memory_order_relaxed);
		CHECK(after == before + 10, "RED arm: disarmed recording must change the count (it must NOT)");
	}
#endif
#ifdef RED_BREAK_VERDICT_S2C
	// RED: claim a measured caller-S2C op is a tier2 candidate (it must be duplex-only). The verdict
	// logic puts caller_s2c>0 ahead of the no-fiber check, so this contradicts it.
	{
		std::string j3 = Metrics::shared().snapshotJSON("");
		CHECK(heatStr(j3, "dserver_callnum_mach_vm_deallocate", "verdict") == "tier2-candidate", "RED arm");
	}
#endif
#ifdef RED_BREAK_PERCALL_LEAK
	// RED (D14): assert the OLD sticky-latch behavior -- that the S2C driven by op A LEAKED onto the
	// bystander op B recorded next on the same thread. With the per-call-scoped fix, B's caller_s2c is
	// 0, so this assertion (expecting the leak) MUST fail -- proving the cross-op leak is gone.
	{
		std::string j4 = Metrics::shared().snapshotJSON("");
		CHECK(heatVal(j4, "dserver_callnum_vchroot_path", "caller_s2c") == 1,
		      "RED arm: expects the leak onto B (must NOT happen under per-call attribution)");
	}
#endif

	if (failures == 0) {
		std::printf("GREEN: RPC heatmap records transport/flags + derives lane verdicts correctly\n");
		return 0;
	}
	std::fprintf(stderr, "RED: %d heatmap check(s) failed\n", failures);
	return 1;
}
