// perf #18 (dar-dar6x4-perf-5dq.30) P3 RED->GREEN gate for the server-side C2S ring ELIGIBILITY
// ALLOWLIST (call.cpp's darRingServiceC2S). Unlike ring_datapath_test.cpp (hermetic, callnum-
// agnostic), this gate pins the EXACT set of real dserver_callnum_* values that may ride the
// ring -- the actual new code added when mach_reply_port was migrated.
//
// It must use the GENERATED rpc.h (build-dir only) for the real enum values, so it is compiled
// with the build include dir injected by run-ring-shm-validate.sh (DSERVER_GEN_INC).
//
//   GREEN arm: the current allowlist, derived from the AUTHORITATIVE shared macro
//              DSERVER_RING_C2S_OPCODES (rpc-supplement.h) -- the single set the server allowlist and
//              the guest dispatch both expand (drift-gated by ring_drift_gate_test.c). Asserts every
//              migrated op is eligible and a representative non-migrated op (kprintf) is NOT.
//   RED   arm (-DALLOWLIST_OLD): a stale hand-list (task_self_trap only). The "mach_reply_port is
//              eligible" assertion MUST then fail -> proves the gate exercises the allowlist, not a
//              tautology.
//
// SECURITY note: the allowlist is the trust boundary that keeps an untrusted guest from driving
// arbitrary server calls over the ring. Only ops that pass the 5-rule membership canon belong here.
//
// perf #18 D10 (dar-1il.5): thread_self_trap + host_self_trap migrated (Tier-2 no-fiber), so they are
// now ring-eligible -- this gate is updated to derive from the macro rather than a hand-mirror, which
// is what made the prior "host_self_trap is NOT eligible (P3)" assertion go stale.

#include <darlingserver/rpc.h>
#include <darlingserver/rpc-supplement.h>

#include <cstdint>
#include <cstdio>

// Derive eligibility from the SAME macro the real code uses, so this gate can never drift from the
// server allowlist. (The RED arm flips to the stale pre-migration hand-list to prove non-tautology.)
static bool ringEligible(uint32_t callnum) {
#ifdef ALLOWLIST_OLD
	// stale pre-migration hand-list: task_self_trap only
	return (callnum == dserver_callnum_task_self_trap);
#else
	bool eligible = false;
#define DSERVER_RING_C2S_ELIGIBLE_TEST(op) || (callnum == (uint32_t)dserver_callnum_##op)
	eligible = (false DSERVER_RING_C2S_OPCODES(DSERVER_RING_C2S_ELIGIBLE_TEST));
#undef DSERVER_RING_C2S_ELIGIBLE_TEST
	return eligible;
#endif
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } } while (0)

int main(void) {
	// Every migrated op must be eligible.
	CHECK(ringEligible(dserver_callnum_task_self_trap), "task_self_trap is ring-eligible");
	CHECK(ringEligible(dserver_callnum_mach_reply_port), "mach_reply_port is ring-eligible");
	CHECK(ringEligible(dserver_callnum_mach_port_allocate), "mach_port_allocate is ring-eligible");
	CHECK(ringEligible(dserver_callnum_mach_port_insert_right), "mach_port_insert_right is ring-eligible");
	// perf #18 D10: the self-trap family is now fully migrated.
	CHECK(ringEligible(dserver_callnum_thread_self_trap), "thread_self_trap is ring-eligible [D10]");
	CHECK(ringEligible(dserver_callnum_host_self_trap), "host_self_trap is ring-eligible [D10]");
	// perf #18 D11 (dar-1il.6): the bulk closed-fast Lane-1 batch (Tier-1 generic fiber).
	CHECK(ringEligible(dserver_callnum_uidgid), "uidgid is ring-eligible [D11]");
	CHECK(ringEligible(dserver_callnum_set_thread_handles), "set_thread_handles is ring-eligible [D11]");
	CHECK(ringEligible(dserver_callnum_started_suspended), "started_suspended is ring-eligible [D11]");
	CHECK(ringEligible(dserver_callnum_get_tracer), "get_tracer is ring-eligible [D11]");
	CHECK(ringEligible(dserver_callnum_task_is_64_bit), "task_is_64_bit is ring-eligible [D11]");
	// perf #18 D13 (dar-1il.8): the path ops (fixed 16B body / 8B reply; path via /proc/mem, no arena).
	CHECK(ringEligible(dserver_callnum_mldr_path), "mldr_path is ring-eligible [D13]");
	CHECK(ringEligible(dserver_callnum_vchroot_path), "vchroot_path is ring-eligible [D13]");

	// A representative op that is NOT migrated must be rejected (stays on UDS). kprintf takes a
	// request body and is not in the macro, so it must NEVER be ring-eligible.
	CHECK(!ringEligible(dserver_callnum_kprintf), "kprintf is NOT ring-eligible (has a body)");
	// destroy-capable ops stay off the simple ring (canon rules 1-2).
	CHECK(!ringEligible(dserver_callnum_mach_port_deallocate), "mach_port_deallocate is NOT ring-eligible (destroy-capable)");

	if (failures == 0) {
		printf("ring allowlist gate: all checks passed (%s)\n",
#ifdef ALLOWLIST_OLD
		       "OLD"
#else
		       "CURRENT"
#endif
		);
		return 0;
	}
	fprintf(stderr, "ring allowlist gate: %d FAILURES\n", failures);
	return 1;
}
