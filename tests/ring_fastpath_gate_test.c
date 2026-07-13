// perf #18 P6.1 (dar-ohp): RED->GREEN gate on the ring fast-path eligibility + escape hatches.
//
// The fast inline path (doWorkInline, no fiber) runs a proven-non-blocking op directly on the
// main loop. Two safety properties MUST hold or we risk running a blocking/ineligible op without
// a fiber (UB) or being unable to disable the specialized semantics if a bug surfaces:
//   1. ONLY allowlisted no-block ops are eligible (mach_reply_port, task_self_trap) -- nothing else.
//   2. The escape hatches actually gate it: DARLING_SERVER_FAST_OPS=0 disables ALL fast paths;
//      DARLING_SERVER_FAST_MACH_REPLY_PORT=0 disables just mach_reply_port's.
// This test pins both. It mirrors the real ringFastPathEligible() logic from call.cpp against the
// generated callnums.
//
// GREEN (default): the real hatch-aware predicate -- eligibility flips with the env knobs.
// RED arm (-DFASTPATH_NO_HATCH): a predicate that ignores the hatches (always-eligible for the
// allowlist); the "hatch disables it" assertions MUST then fail, proving the test exercises the
// hatch logic, not just the allowlist.
//
// P6.1 step 2 (dar-ohp) also pins the SURGICAL direct-dispatch shape guard for mach_reply_port
// (fastMachReplyPortEligible in call.cpp): only an empty-body/no-arena mach_reply_port slot may
// take the no-Call path. RED arm (-DNO_SHAPE_GUARD) drops that guard; the "rejects a payload/arena
// slot" assertions MUST then fail.

#define _POSIX_C_SOURCE 200112L
// Make the shared C2S opcode allowlist macro (DSERVER_RING_C2S_OPCODES) visible -- it lives inside
// the DSERVER_RING_TRANSPORT block in rpc-supplement.h, the SINGLE source of truth both the server
// (call.cpp) and the guest (dserver-ring.c) key off. We gate the (unused-here) attach-check out.
#define DSERVER_RING_TRANSPORT 1
#define DSERVER_RING_NO_ATTACH_CHECK 1
#include <darlingserver/rpc.h>
#include <darlingserver/rpc-supplement.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int env_off(const char* name) {
	const char* e = getenv(name);
	return (e && e[0] == '0' && e[1] == '\0');
}

// The predicate under test, mirroring call.cpp's ringFastPathEligible() (the no-fiber INLINE
// fast path). perf #18 P5 (dar-1il): mach_port_mod_refs is DELIBERATELY NOT inline-eligible --
// running it off the microthread fiber corrupted the thread's stack bookkeeping (server abort in
// StackPool::free). It rides the ring via the GENERIC fiber doWork() instead (see c2s_allowlisted).
// So the inline predicate must REJECT mod_refs; the assertion below pins that.
static int fast_eligible(unsigned int callnum) {
#ifndef FASTPATH_NO_HATCH
	if (env_off("DARLING_SERVER_FAST_OPS")) return 0;            // global kill-switch
	if (callnum == (unsigned)dserver_callnum_mach_reply_port)
		return !env_off("DARLING_SERVER_FAST_MACH_REPLY_PORT");  // per-op kill-switch
	if (callnum == (unsigned)dserver_callnum_task_self_trap)
		return 1;
	return 0;
#else
	// RED arm: ignores the hatches entirely (the bug we must catch).
	if (callnum == (unsigned)dserver_callnum_mach_reply_port)    return 1;
	if (callnum == (unsigned)dserver_callnum_task_self_trap)     return 1;
	return 0;
#endif
}

// perf #18 P5 (dar-1il) / P5-bulk (dar-1il.1): the C2S allowlist predicate, mirroring the `eligible`
// check in call.cpp's ringServiceThread(). The port/right bookkeeping ops (mach_port_mod_refs,
// _deallocate, _allocate, _insert_right) ride the GENERIC fiber doWork() path on the ring (they
// carry a request body; they are NOT inline/surgical-eligible -- see fast_eligible). The body must
// fit one inline slot (reqlen <= inlineCap). CRITICALLY this allowlist is NOT gated by any per-op
// env hatch: a request the guest published is parked waiting for a ring reply, so silently dropping
// it (gating off server-side) would strand the guest on its bounded reply-wait every call.
//
// THE allowlist is now GENERATED from the SHARED DSERVER_RING_C2S_OPCODES macro -- the IDENTICAL
// macro the server (call.cpp) and the guest (dserver-ring.c) consume -- so this mirror cannot drift
// from the real predicate. RED arm -DC2S_READD_MODREFS RE-ADDS mach_port_mod_refs to the local copy
// of the macro below (modeling the dar-1il.2 regression of returning the destroy-capable op to the
// ring); the "mach_port_mod_refs is NOT C2S-allowlisted" GREEN assertion MUST then fail, proving the
// gate pins mod_refs OUT of the ring set. (ring_drift_gate_test.c separately proves guest==server
// set equality.)
#ifdef C2S_READD_MODREFS
// RED arm: a deliberately drifted allowlist that RE-ADDS mach_port_mod_refs (the S2C-deadlock hazard
// dar-1il.2 removed). The "mach_port_mod_refs is NOT C2S-allowlisted" GREEN assertion MUST then fail.
#define C2S_GATE_OPCODES(X) \
	DSERVER_RING_C2S_OPCODES(X) \
	X(mach_port_mod_refs)
#else
#define C2S_GATE_OPCODES(X) DSERVER_RING_C2S_OPCODES(X)
#endif
static int c2s_allowlisted(unsigned int callnum, unsigned int reqlen, unsigned int inlinecap) {
	int eligible = 0;
#define C2S_GATE_MATCH(op) || (callnum == (unsigned)dserver_callnum_##op)
	eligible = (0 C2S_GATE_OPCODES(C2S_GATE_MATCH));
#undef C2S_GATE_MATCH
	if (!eligible || reqlen > inlinecap) return 0;
	return 1;
}

// perf #18 P6.1 step 2 (dar-ohp): the SURGICAL direct-dispatch predicate, mirroring call.cpp's
// fastMachReplyPortEligible(). Stricter than fast_eligible: EXACTLY mach_reply_port AND a
// no-payload/no-arena shape. The shape guard is the security boundary for the no-Call path -- a
// slot carrying a body or arena must be rejected (it would mean a malformed/unexpected request we
// must not run without the framed validation). RED arm (-DNO_SHAPE_GUARD) drops the shape check;
// the "rejects a payload/arena slot" assertions MUST then fail, proving the test exercises it.
static int fast_mach_reply_port_eligible(unsigned int callnum, unsigned int reqlen, unsigned int arenalen) {
#ifndef NO_SHAPE_GUARD
	if (reqlen != 0 || arenalen != 0) return 0;
#endif
	if (callnum != (unsigned)dserver_callnum_mach_reply_port) return 0;
	if (env_off("DARLING_SERVER_FAST_OPS")) return 0;
	if (env_off("DARLING_SERVER_FAST_MACH_REPLY_PORT")) return 0;
	return 1;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
	else         { fprintf(stderr, "ok:   %s\n", (msg)); } \
} while (0)

static void setenv01(const char* n, int off) {
	if (off) setenv(n, "0", 1); else unsetenv(n);
}

int main(void) {
	// --- INLINE allowlist: only the trivial pure-mint port traps are inline-eligible ---------
	// mach_port_mod_refs is INTENTIONALLY excluded (it crashed the server off the fiber; and as of
	// dar-1il.2 it is off the ring entirely -- UDS only -- so it is neither inline- nor C2S-eligible).
	setenv01("DARLING_SERVER_FAST_OPS", 0);
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 0);
	CHECK(fast_eligible(dserver_callnum_mach_reply_port),     "mach_reply_port is inline-eligible by default");
	CHECK(fast_eligible(dserver_callnum_task_self_trap),      "task_self_trap is inline-eligible by default");
	CHECK(!fast_eligible(dserver_callnum_mach_port_mod_refs), "mach_port_mod_refs is NOT inline-eligible (UDS only)");
	// perf #18 P5-bulk (dar-1il.1): the new port/right ops ALSO ride the generic fiber path, never
	// the no-fiber inline path (mach_port_mod_refs proved these are not off-fiber-safe).
	CHECK(!fast_eligible(dserver_callnum_mach_port_allocate),     "mach_port_allocate is NOT inline-eligible (rides the fiber path)");
	CHECK(!fast_eligible(dserver_callnum_mach_port_insert_right), "mach_port_insert_right is NOT inline-eligible (rides the fiber path)");
	CHECK(!fast_eligible(dserver_callnum_kprintf),            "kprintf is NOT inline-eligible");
	CHECK(!fast_eligible(dserver_callnum_checkin),            "checkin is NOT inline-eligible");

	// --- escape hatch: global kill-switch disables ALL inline fast paths --------------------
	setenv01("DARLING_SERVER_FAST_OPS", 1);
	CHECK(!fast_eligible(dserver_callnum_mach_reply_port),    "FAST_OPS=0 disables mach_reply_port inline path");
	CHECK(!fast_eligible(dserver_callnum_task_self_trap),     "FAST_OPS=0 disables task_self_trap inline path");
	setenv01("DARLING_SERVER_FAST_OPS", 0);

	// --- escape hatch: per-op kill-switch disables just mach_reply_port ----------------------
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 1);
	CHECK(!fast_eligible(dserver_callnum_mach_reply_port), "FAST_MACH_REPLY_PORT=0 disables mach_reply_port inline path");
	CHECK(fast_eligible(dserver_callnum_task_self_trap),   "FAST_MACH_REPLY_PORT=0 leaves task_self_trap inline path on");
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 0);

	// --- P5 C2S allowlist: the Tier-1 ring ops ride the GENERIC fiber path on the ring -----------
	// inlineCap for the guest's 128B slot is 128 - sizeof(dserver_ring_slot_t); the 16-byte bodies
	// fit easily. Use a representative cap of 96 (>= 16). Allowlisted with a body, dropped if the body
	// would overflow the slot. The allowlist is NOT env-gated (gating off a guest-published op would
	// strand the guest on its reply-wait).
	CHECK(c2s_allowlisted(dserver_callnum_mach_reply_port, 0, 96),
	      "mach_reply_port (empty body) is C2S-allowlisted");
	// perf #18 P5-bulk (dar-1il.1): allocate + insert_right are C2S-allowlisted (Tier-1 ring).
	// Bodies: allocate=16B {target,right,name-ptr}; insert_right=16B {target,name,poly,polyPoly} --
	// within a 96-byte inline cap.
	CHECK(c2s_allowlisted(dserver_callnum_mach_port_allocate, 16, 96),
	      "mach_port_allocate (16-byte body) is C2S-allowlisted");
	CHECK(c2s_allowlisted(dserver_callnum_mach_port_insert_right, 16, 96),
	      "mach_port_insert_right (16-byte body) is C2S-allowlisted");
	CHECK(!c2s_allowlisted(dserver_callnum_mach_port_allocate, 200, 96),
	      "mach_port_allocate with an over-cap body is dropped (UDS fallback)");
	// mach_port_deallocate is DELIBERATELY NOT allowlisted: it can trigger a vm munmap S2C upcall to
	// the caller, which a ring-waiting (non-recvmsg) thread cannot service -> deadlock. Stays on UDS.
	CHECK(!c2s_allowlisted(dserver_callnum_mach_port_deallocate, 8, 96),
	      "mach_port_deallocate is NOT C2S-allowlisted (S2C-upcall deadlock hazard -> UDS only)");
	// mach_port_mod_refs is DELIBERATELY NOT allowlisted (dar-1il.2): it is destroy-capable
	// (mod_refs(delta<0) last-ref destroys the port -> same vm munmap S2C upcall hazard as
	// deallocate). Stays on UDS. RED arm -DC2S_READD_MODREFS re-adds it -> this assertion fails.
	CHECK(!c2s_allowlisted(dserver_callnum_mach_port_mod_refs, 16, 96),
	      "mach_port_mod_refs is NOT C2S-allowlisted (destroy-capable -> S2C-upcall deadlock hazard -> UDS only)");
	CHECK(!c2s_allowlisted(dserver_callnum_kprintf, 4, 96),
	      "kprintf is NOT C2S-allowlisted");

	// --- P6.1 step 2: surgical mach_reply_port direct dispatch + shape guard -----------------
	setenv01("DARLING_SERVER_FAST_OPS", 0);
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 0);
	// accepted: mach_reply_port with an empty body and no arena (the only valid shape)
	CHECK(fast_mach_reply_port_eligible(dserver_callnum_mach_reply_port, 0, 0),
	      "mach_reply_port w/ empty body + no arena is direct-dispatch eligible");
	// rejected by shape: a body or arena present (RED arm without NO_SHAPE_GUARD must fail these)
	CHECK(!fast_mach_reply_port_eligible(dserver_callnum_mach_reply_port, 4, 0),
	      "mach_reply_port w/ inline body is REJECTED by shape guard");
	CHECK(!fast_mach_reply_port_eligible(dserver_callnum_mach_reply_port, 0, 16),
	      "mach_reply_port w/ arena is REJECTED by shape guard");
	// rejected by callnum: only mach_reply_port may take the no-Call path (task_self_trap may NOT)
	CHECK(!fast_mach_reply_port_eligible(dserver_callnum_task_self_trap, 0, 0),
	      "task_self_trap is NOT direct-dispatch eligible (mach_reply_port only)");
	CHECK(!fast_mach_reply_port_eligible(dserver_callnum_kprintf, 0, 0),
	      "kprintf is NOT direct-dispatch eligible");
	// hatches still gate the direct path
	setenv01("DARLING_SERVER_FAST_OPS", 1);
	CHECK(!fast_mach_reply_port_eligible(dserver_callnum_mach_reply_port, 0, 0),
	      "FAST_OPS=0 disables the direct mach_reply_port path");
	setenv01("DARLING_SERVER_FAST_OPS", 0);
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 1);
	CHECK(!fast_mach_reply_port_eligible(dserver_callnum_mach_reply_port, 0, 0),
	      "FAST_MACH_REPLY_PORT=0 disables the direct mach_reply_port path");
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 0);

	if (failures) {
		fprintf(stderr, "\nring_fastpath_gate_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_fastpath_gate_test: allowlist + escape hatches hold\n");
	return 0;
}
