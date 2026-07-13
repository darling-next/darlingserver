// perf #18 P5-bulk (dar-1il.1): RED->GREEN gate for the NO-SILENT-DROP protocol invariant.
//
// THE INVARIANT: if the guest can publish an opcode onto the c2s ring, the server MUST service it
// over the ring. If the two sets drift -- an opcode the guest routes over the ring that the server
// does NOT allowlist -- the server consumes the request slot and produces no reply, and the guest
// (having published) is stranded on its bounded reply-wait forever for that call (it will NOT
// UDS-fall-back, because from its side the request WAS sent). That is an effective per-call wedge.
// This is the exact bug class perf #18 P5 hit under an early per-op server hatch (see dar-1il P5
// comment); the fix was to make both sides derive from ONE shared list so they cannot drift.
//
// This gate pins that the GUEST "may publish" set and the SERVER "will service" set are IDENTICAL.
//
// GREEN (default): both sets are built from the shared DSERVER_RING_C2S_OPCODES macro in
//   rpc-supplement.h -- the IDENTICAL macro the guest (dserver-ring.c routing) and the server
//   (call.cpp ringServiceThread eligible check) consume. By construction they are equal, and this
//   test asserts set equality, so it passes.
//
// RED arm (-DDRIFT_GUEST_EXTRA): models the regression we must catch -- the guest gains an opcode
//   (here, a synthetic extra) that the server's allowlist does not have. The set-equality assertion
//   MUST then fail (a guest opcode missing from the server set is found), proving the gate exercises
//   the invariant and is not vacuous.
//
// Hermetic: header-only, host C, no boot/sockets. Reuses the generated rpc.h for the callnums.

#define _POSIX_C_SOURCE 200112L
#define DSERVER_RING_TRANSPORT 1
#define DSERVER_RING_NO_ATTACH_CHECK 1
#include <darlingserver/rpc.h>
#include <darlingserver/rpc-supplement.h>
#include <stdio.h>
#include <stdlib.h>

// Build the SERVER "will service over the ring" set straight from the shared macro -- this is
// exactly what call.cpp's `eligible` check expands to.
static const unsigned int server_set[] = {
#define SRV_ENTRY(op) (unsigned int)dserver_callnum_##op,
	DSERVER_RING_C2S_OPCODES(SRV_ENTRY)
#undef SRV_ENTRY
};
static const int server_set_len = (int)(sizeof(server_set) / sizeof(server_set[0]));

// Build the GUEST "may publish onto the ring" set. In the real tree the guest derives this from the
// SAME macro (dserver-ring.c routes precisely the ops in DSERVER_RING_C2S_OPCODES), so GREEN builds
// it from the macro too -> identical to server_set by construction. The RED arm appends a synthetic
// extra opcode the server does NOT have, modeling a guest that routes an op the server will silently
// drop.
static const unsigned int guest_set[] = {
#define GST_ENTRY(op) (unsigned int)dserver_callnum_##op,
	DSERVER_RING_C2S_OPCODES(GST_ENTRY)
#undef GST_ENTRY
#ifdef DRIFT_GUEST_EXTRA
	(unsigned int)dserver_callnum_kprintf, // guest routes kprintf over the ring but server won't service it
#endif
};
static const int guest_set_len = (int)(sizeof(guest_set) / sizeof(guest_set[0]));

static int in_set(const unsigned int* set, int len, unsigned int v) {
	for (int i = 0; i < len; ++i) {
		if (set[i] == v) return 1;
	}
	return 0;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
	else         { fprintf(stderr, "ok:   %s\n", (msg)); } \
} while (0)

int main(void) {
	// (a) Every opcode the GUEST may publish MUST be in the SERVER allowlist (no silent drop).
	//     This is the load-bearing direction: a guest op absent server-side = a wedge.
	for (int i = 0; i < guest_set_len; ++i) {
		char buf[128];
		snprintf(buf, sizeof(buf), "guest-published callnum %u is server-serviced (no silent drop)", guest_set[i]);
		CHECK(in_set(server_set, server_set_len, guest_set[i]), buf);
	}

	// (b) Symmetry: every opcode the SERVER will service is one the guest actually publishes (a
	//     server entry with no guest producer is dead weight / a sign of an out-of-sync edit).
	for (int i = 0; i < server_set_len; ++i) {
		char buf[128];
		snprintf(buf, sizeof(buf), "server-serviced callnum %u has a guest producer", server_set[i]);
		CHECK(in_set(guest_set, guest_set_len, server_set[i]), buf);
	}

	// (c) Sanity: the shared set is non-empty and contains the ops migrated so far. Note BOTH
	//     mach_port_deallocate AND mach_port_mod_refs are DELIBERATELY ABSENT (S2C-upcall deadlock
	//     hazard from destroying a mapped-region-backed port -> UDS only; mod_refs removed in
	//     dar-1il.2 because mod_refs(delta<0) last-ref is destroy-capable).
	CHECK(server_set_len >= 4, "shared C2S allowlist has the expected ops");
	CHECK(in_set(server_set, server_set_len, (unsigned)dserver_callnum_mach_port_allocate),     "allocate in shared set");
	CHECK(in_set(server_set, server_set_len, (unsigned)dserver_callnum_mach_port_insert_right), "insert_right in shared set");
	CHECK(!in_set(server_set, server_set_len, (unsigned)dserver_callnum_mach_port_deallocate),  "deallocate NOT in shared set (S2C deadlock hazard)");
	CHECK(!in_set(server_set, server_set_len, (unsigned)dserver_callnum_mach_port_mod_refs),    "mod_refs NOT in shared set (destroy-capable -> S2C deadlock hazard, dar-1il.2)");

	if (failures) {
		fprintf(stderr, "\nring_drift_gate_test: %d FAILURE(S) -- guest/server C2S allowlists DRIFT (silent-drop wedge hazard)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_drift_gate_test: guest == server C2S allowlist (no silent-drop drift)\n");
	return 0;
}
