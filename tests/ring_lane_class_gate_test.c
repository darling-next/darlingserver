// perf #18 Phase A (dar-dar6x4-perf-5dq.30.1): RED->GREEN gate on the THREE-LANE op classification
// + the static guardrail that a destroy-capable / caller-S2C op CANNOT ride the simple ring.
//
// plan.md froze the hybrid IPC target: Lane 0 UDS / Lane 1 simple closed req->reply ring / Lane 2
// future duplex. The membership canon (rpc-supplement.h) says a simple-ring op must NEVER be
// destroy-capable or caller-S2C-capable (a parked caller cannot service an S2C upcall -> deadlock;
// this sank deallocate + mod_refs). Phase A turns that prose rule into a machine-checked invariant:
// DSERVER_RING_OP_CLASS classifies each ring-relevant op, and dserver_ring_c2s_set_is_canon_safe()
// folds "every DSERVER_RING_C2S_OPCODES member is SimpleRingC2SEligible and is NOT Destroy/CallerS2C".
//
// This test pins the classification + the invariant from a HOST build (no server/boot), the same way
// the other ring gates do. It is compiled as C (the guest-side language) so the predicate is exercised
// in the libc-free dialect too; the C++ build separately enforces the same property at COMPILE TIME
// via the static_assert in the header (run-ring-shm-validate.sh drives that arm).
//
// GREEN (default): the real classification holds -- the simple set is canon-safe, the UDS-only ops
// are tagged destroy/caller-S2C, and the lane-bit policy relationships hold.
// RED arm -DLANECLASS_READD_DEALLOCATE: re-add mach_port_deallocate to a LOCAL copy of the C2S macro
// (modeling the dar-1il.1 regression). The "canon-safe over the drifted set" assertion MUST then fail,
// proving the fold actually rejects a destroy-capable member.
// RED arm -DLANECLASS_DESTROY_IN_RING: reclassify a real simple-ring op (mach_port_allocate) as
// DestroyCapable in a LOCAL class lookup. The "allocate is not destroy-capable" + "real set canon-safe"
// assertions MUST then fail, proving the fold actually reads the destroy/caller-S2C bits.

#define _POSIX_C_SOURCE 200112L
#define DSERVER_RING_TRANSPORT 1
#define DSERVER_RING_NO_ATTACH_CHECK 1
#include <darlingserver/rpc.h>
#include <darlingserver/rpc-supplement.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
	else         { fprintf(stderr, "ok:   %s\n", (msg)); } \
} while (0)

// --- RED arm helpers -------------------------------------------------------------------------------
// LANECLASS_DESTROY_IN_RING: a local class lookup that mis-tags mach_port_allocate as DestroyCapable,
// modeling someone wrongly classifying a real simple-ring op. We fold the canon predicate over THIS
// lookup so the RED arm can break the destroy-bit read without editing the shipped table.
static uint32_t op_class_local(uint32_t callnum) {
#ifdef LANECLASS_DESTROY_IN_RING
	if (callnum == (uint32_t)dserver_callnum_mach_port_allocate)
		return DSERVER_RING_CLASS_SIMPLE_C2S | DSERVER_RING_CLASS_DESTROY;
#endif
	return dserver_ring_op_class(callnum);
}

// LANECLASS_READD_DEALLOCATE: a local copy of the simple-ring set that RE-ADDS mach_port_deallocate
// (which the shipped table tags UDS_ONLY | DESTROY | CALLER_S2C). Fold the canon predicate over THIS
// set; it MUST come out unsafe.
#ifdef LANECLASS_READD_DEALLOCATE
#define LOCAL_C2S_OPCODES(X) \
	DSERVER_RING_C2S_OPCODES(X) \
	X(mach_port_deallocate)
#else
#define LOCAL_C2S_OPCODES(X) DSERVER_RING_C2S_OPCODES(X)
#endif
#define LOCAL_MEMBER_IS_SAFE(op) \
	&& ((op_class_local((uint32_t)dserver_callnum_##op) & DSERVER_RING_CLASS_SIMPLE_C2S) != 0u) \
	&& ((op_class_local((uint32_t)dserver_callnum_##op) & (DSERVER_RING_CLASS_DESTROY | DSERVER_RING_CLASS_CALLER_S2C)) == 0u)
static int local_set_is_canon_safe(void) {
	return (1 LOCAL_C2S_OPCODES(LOCAL_MEMBER_IS_SAFE)) ? 1 : 0;
}

int main(void) {
	// --- the shipped simple-ring set obeys the canon (GREEN; the static_assert pins this at compile
	//     time in C++ -- this is the C-dialect runtime mirror) -----------------------------------
	CHECK(dserver_ring_c2s_set_is_canon_safe(),
	      "shipped DSERVER_RING_C2S_OPCODES set is canon-safe (no destroy/caller-S2C member)");

	// --- the folded predicate over the LOCAL set: GREEN matches the shipped fold; RED arms break it.
	//     -DLANECLASS_READD_DEALLOCATE makes this set contain a destroy-capable op -> MUST be unsafe.
	//     -DLANECLASS_DESTROY_IN_RING mis-tags allocate -> MUST be unsafe.
	CHECK(local_set_is_canon_safe(),
	      "local (test-fold) C2S set is canon-safe");

	// --- per-op classification: the simple-ring members are SimpleRingC2SEligible, none destroy/S2C
	CHECK(dserver_ring_op_class(dserver_callnum_task_self_trap) & DSERVER_RING_CLASS_SIMPLE_C2S,
	      "task_self_trap is SimpleRingC2SEligible");
	CHECK(dserver_ring_op_class(dserver_callnum_mach_reply_port) & DSERVER_RING_CLASS_SIMPLE_C2S,
	      "mach_reply_port is SimpleRingC2SEligible");
	CHECK(dserver_ring_op_class(dserver_callnum_mach_port_allocate) & DSERVER_RING_CLASS_SIMPLE_C2S,
	      "mach_port_allocate is SimpleRingC2SEligible");
	CHECK(dserver_ring_op_class(dserver_callnum_mach_port_insert_right) & DSERVER_RING_CLASS_SIMPLE_C2S,
	      "mach_port_insert_right is SimpleRingC2SEligible");
	// allocate must NOT be destroy-capable (RED -DLANECLASS_DESTROY_IN_RING flips op_class_local, so
	// assert via op_class_local to make the RED arm break this exact line).
	CHECK((op_class_local(dserver_callnum_mach_port_allocate) & DSERVER_RING_CLASS_DESTROY) == 0u,
	      "mach_port_allocate is NOT DestroyCapable");

	// --- Tier-2 (NoFiberFast) is a SUB-LANE of Tier-1: NoFiberFast => SimpleRingC2S (policy #4) -----
	CHECK(dserver_ring_op_class(dserver_callnum_mach_reply_port) & DSERVER_RING_CLASS_NOFIBER_FAST,
	      "mach_reply_port is NoFiberFastEligible (Tier 2)");
	CHECK(dserver_ring_op_class(dserver_callnum_task_self_trap) & DSERVER_RING_CLASS_NOFIBER_FAST,
	      "task_self_trap is NoFiberFastEligible (Tier 2)");
	// perf #18 D10 (dar-1il.5): the rest of the pure-mint self-trap family joined Tier 2. Pin that
	// they are BOTH SimpleRingC2S (Lane 1) AND NoFiberFast (Tier 2) AND carry NEITHER destroy nor
	// caller-S2C (so the canon-safe fold over the whole C2S set still holds -- see the static_assert).
	CHECK(dserver_ring_op_class(dserver_callnum_thread_self_trap) & DSERVER_RING_CLASS_NOFIBER_FAST,
	      "thread_self_trap is NoFiberFastEligible (Tier 2) [D10]");
	CHECK(dserver_ring_op_class(dserver_callnum_thread_self_trap) & DSERVER_RING_CLASS_SIMPLE_C2S,
	      "thread_self_trap is SimpleRingC2SEligible [D10]");
	CHECK((dserver_ring_op_class(dserver_callnum_thread_self_trap) & (DSERVER_RING_CLASS_DESTROY | DSERVER_RING_CLASS_CALLER_S2C)) == 0u,
	      "thread_self_trap is NEITHER destroy nor caller-S2C [D10]");
	CHECK(dserver_ring_op_class(dserver_callnum_host_self_trap) & DSERVER_RING_CLASS_NOFIBER_FAST,
	      "host_self_trap is NoFiberFastEligible (Tier 2) [D10]");
	CHECK(dserver_ring_op_class(dserver_callnum_host_self_trap) & DSERVER_RING_CLASS_SIMPLE_C2S,
	      "host_self_trap is SimpleRingC2SEligible [D10]");
	CHECK((dserver_ring_op_class(dserver_callnum_host_self_trap) & (DSERVER_RING_CLASS_DESTROY | DSERVER_RING_CLASS_CALLER_S2C)) == 0u,
	      "host_self_trap is NEITHER destroy nor caller-S2C [D10]");
	// allocate/insert_right ride Tier 1 only (generic fiber), NOT the no-fiber path.
	CHECK((dserver_ring_op_class(dserver_callnum_mach_port_allocate) & DSERVER_RING_CLASS_NOFIBER_FAST) == 0u,
	      "mach_port_allocate is NOT NoFiberFastEligible (Tier 1 only)");

	// perf #18 D11 (dar-1il.6): the bulk closed-fast Lane-1 batch. Each must be SimpleRingC2SEligible
	// (Lane 1), Tier-1 ONLY (NOT NoFiberFast -- they read/mutate process/thread state via the generic
	// Call path, not pure mint), and carry NEITHER destroy nor caller-S2C (so the canon-safe fold holds).
#define D11_OP_CHECK(op) do { \
		uint32_t _c = dserver_ring_op_class((uint32_t)dserver_callnum_##op); \
		CHECK((_c & DSERVER_RING_CLASS_SIMPLE_C2S) != 0u, #op " is SimpleRingC2SEligible [D11]"); \
		CHECK((_c & DSERVER_RING_CLASS_NOFIBER_FAST) == 0u, #op " is NOT NoFiberFastEligible (Tier 1 only) [D11]"); \
		CHECK((_c & (DSERVER_RING_CLASS_DESTROY | DSERVER_RING_CLASS_CALLER_S2C)) == 0u, #op " is NEITHER destroy nor caller-S2C [D11]"); \
	} while (0);
	D11_OP_CHECK(uidgid);
	D11_OP_CHECK(set_thread_handles);
	D11_OP_CHECK(started_suspended);
	D11_OP_CHECK(get_tracer);
	D11_OP_CHECK(task_is_64_bit);
	// perf #18 D13 (dar-1il.8): the path ops have the SAME lane-class as the D11 batch -- SimpleRingC2S,
	// Tier-1 ONLY, neither destroy nor caller-S2C. (The path bytes ride /proc/mem, not the payload, so
	// there is no arena and /proc/mem access is not a caller-S2C.)
	D11_OP_CHECK(mldr_path);
	D11_OP_CHECK(vchroot_path);
#undef D11_OP_CHECK

	// --- the UDS-only ops are tagged destroy-capable + caller-S2C, and are NOT SimpleRingC2SEligible
	//     (the canon's whole point: "wrong lane, not bad op") ------------------------------------
	uint32_t dealloc = dserver_ring_op_class(dserver_callnum_mach_port_deallocate);
	CHECK((dealloc & DSERVER_RING_CLASS_SIMPLE_C2S) == 0u,
	      "mach_port_deallocate is NOT SimpleRingC2SEligible");
	CHECK((dealloc & DSERVER_RING_CLASS_DESTROY) != 0u,
	      "mach_port_deallocate is DestroyCapable");
	CHECK((dealloc & DSERVER_RING_CLASS_CALLER_S2C) != 0u,
	      "mach_port_deallocate is CallerS2CCapable");
	CHECK((dealloc & DSERVER_RING_CLASS_UDS_ONLY) != 0u,
	      "mach_port_deallocate is UdsOnly (today)");

	uint32_t modrefs = dserver_ring_op_class(dserver_callnum_mach_port_mod_refs);
	CHECK((modrefs & DSERVER_RING_CLASS_SIMPLE_C2S) == 0u,
	      "mach_port_mod_refs is NOT SimpleRingC2SEligible");
	CHECK((modrefs & DSERVER_RING_CLASS_DESTROY) != 0u,
	      "mach_port_mod_refs is DestroyCapable");
	CHECK((modrefs & DSERVER_RING_CLASS_CALLER_S2C) != 0u,
	      "mach_port_mod_refs is CallerS2CCapable");

	// --- POLICY relationships hold across the whole table -----------------------------------------
	// #2 DestroyCapable => NOT SimpleRingC2S ; #3 CallerS2C => NOT SimpleRingC2S ; #4 NoFiber => SimpleC2S
#define POLICY_CHECK(op, bits) do { \
		uint32_t _c = dserver_ring_op_class((uint32_t)dserver_callnum_##op); \
		if (_c & (DSERVER_RING_CLASS_DESTROY | DSERVER_RING_CLASS_CALLER_S2C)) \
			CHECK((_c & DSERVER_RING_CLASS_SIMPLE_C2S) == 0u, #op " : destroy/caller-S2C => NOT SimpleRingC2S"); \
		if (_c & DSERVER_RING_CLASS_NOFIBER_FAST) \
			CHECK((_c & DSERVER_RING_CLASS_SIMPLE_C2S) != 0u, #op " : NoFiberFast => SimpleRingC2S"); \
	} while (0);
	DSERVER_RING_OP_CLASS(POLICY_CHECK)
#undef POLICY_CHECK

	if (failures) {
		fprintf(stderr, "\nring_lane_class_gate_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_lane_class_gate_test: three-lane classification + canon guardrail hold\n");
	return 0;
}
