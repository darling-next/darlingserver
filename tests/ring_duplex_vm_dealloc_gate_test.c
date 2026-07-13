// perf#18 Phase D5 (P8, dar-1il.3.2.2): RED->GREEN gate on the VM_DEALLOCATE-via-duplex S2C shape.
//
// D4 proved the duplex munmap-S2C transport is once-only + fail-closed-bounded for the GENERIC
// "destroy-capable op whose S2C is a real munmap" (ring_duplex_dealloc_gate_test.c, MODEL 4/5). D5 routes
// a DIFFERENT op -- vm_deallocate -- onto that SAME transport. vm_deallocate differs from
// mach_port_deallocate in one safety-relevant way that this gate makes explicit:
//
//   * mach_port_deallocate's munmap is a SIDE effect of destroying the last ref of a memory-entry-backed
//     port (and in Darling that path is unreachable -- the D4 finding -- so its S2C never fires).
//   * vm_deallocate's munmap IS the op: it removes the mapping from the caller's address space directly
//     (mach_vm_deallocate -> vm_map_remove -> dtape_hook_task_free_pages -> _munmap -> _s2cPerform). The
//     S2C munmap is the entire observable effect, and it MUST fire (ring_duplex_s2c > 0 is the D5 proof).
//
// Because the effect is a real munmap of a caller address range, the "exactly once" property is sharper:
// a double application (e.g. a mid-op UDS fallback after the in-place munmap already ran) would issue a
// SECOND munmap(2) of the same [addr,len). On a busy process that address may already have been re-mapped
// by an unrelated allocation between the two munmaps -> the second munmap silently destroys an UNRELATED
// mapping (memory corruption), strictly worse than the port case. So the once-only invariant is not just
// "don't double-free a refcount", it is "never issue a second munmap of a range we already unmapped".
//
// This gate proves, for the vm_deallocate munmap shape:
//   MODEL A — the munmap S2C is REQUIRED to fire (the op's whole effect); a design that completes the
//     parent WITHOUT delivering the S2C is wrong (it would tell the guest the range is gone while it is
//     still mapped). RED -DDUPLEX_VM_SKIP_S2C models "complete without the S2C" -> caught.
//   MODEL B — the munmap is issued EXACTLY ONCE; no partial-mutation-then-UDS second munmap (the
//     reused-address corruption hazard). RED -DDUPLEX_VM_DOUBLE_MUNMAP models a second munmap -> caught.
//   MODEL C — a dropped/unharvested upcall reply fails closed + bounded, never fabricates success (a
//     fabricated success on vm_deallocate tells the guest the range is unmapped when the munmap never ran
//     -> later access to a range the guest believes freed). RED -DDUPLEX_VM_FABRICATE_ON_DROP -> caught.
//
// Pure logic, hermetic, instant, deterministic -- dar-my8 / D4 discipline. GREEN before ANY vm_deallocate
// wiring is touched; the RED arms fail.

#define _POSIX_C_SOURCE 200112L
#define DSERVER_RING_TRANSPORT 1
#define DSERVER_RING_NO_ATTACH_CHECK 1
#include <darlingserver/rpc.h>
#include <darlingserver/rpc-supplement.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
	else         { fprintf(stderr, "ok:   %s\n", (msg)); } \
} while (0)

// The cap bit D5 negotiates MUST be distinct from the SELFTEST (0x1) and DEALLOCATE (0x2) bits, or a
// vm_deallocate routing decision could be confused with a (D4) deallocate one. Pin it at compile time.
#if DSERVER_RING_DUPLEX_CAP_VM_DEALLOCATE == DSERVER_RING_DUPLEX_CAP_SELFTEST || \
    DSERVER_RING_DUPLEX_CAP_VM_DEALLOCATE == DSERVER_RING_DUPLEX_CAP_DEALLOCATE
#error "DSERVER_RING_DUPLEX_CAP_VM_DEALLOCATE must be a distinct capability bit"
#endif

// ============================================================================================
// Model the lifecycle of ONE vm_deallocate routed onto the duplex lane. The munmap S2C is the op's whole
// effect; we count how many real munmap(2) calls the design issues for the caller range, and whether the
// S2C was actually delivered before the parent completed.
struct VMState {
	int routed_on_duplex;   // pre-dispatch routing decided duplex (cap negotiated)
	int s2c_delivered;      // the munmap S2C upcall was published + the guest pump ran the real munmap
	int munmap_count;       // number of real munmap(2) applications of the SAME [addr,len)
	int parent_completed;   // the parent vm_deallocate op returned a (claimed) success to the guest
	int fell_back_to_uds;   // the op was re-attempted over UDS after mutation began
};

// Run one vm_deallocate op through the modeled duplex routing.
static void vm_dealloc_run(struct VMState* s) {
	memset(s, 0, sizeof(*s));

	// PRE-DISPATCH routing: cap negotiated -> ride the duplex lane. (A pre-dispatch decline would simply
	// run the UDS path once and is the trivially-once case, not the hazard; we model the on-duplex path.)
	s->routed_on_duplex = 1;

	// dispatch on the fiber; the dtape vm_map_remove drives the munmap S2C to the caller.
#ifdef DUPLEX_VM_SKIP_S2C
	// RED (wrong): complete the parent WITHOUT publishing/servicing the munmap S2C. The guest is told the
	// range is freed but the munmap never ran -> the mapping is still live. s2c_delivered stays 0.
	s->parent_completed = 1;
#else
	// GREEN: publish the munmap S2C; the guest pump runs the REAL munmap(2) on the caller thread once.
	s->s2c_delivered = 1;
	s->munmap_count += 1;

#ifdef DUPLEX_VM_DOUBLE_MUNMAP
	// RED (wrong): mid-op, "fall back" to UDS after the in-place munmap already ran -> the UDS re-run
	// issues a SECOND munmap of the same range (which may have been re-mapped) -> corruption.
	s->fell_back_to_uds = 1;
	s->munmap_count += 1;
#endif

	s->parent_completed = 1;
#endif
}

// ============================================================================================
// MODEL C — a dropped/unharvested upcall reply fails closed + bounded; never fabricates success.
// (Structurally identical to D4 MODEL 5, restated for the vm_deallocate effect: a fabricated success here
//  tells the guest a still-mapped range is unmapped.)
// Returns: 0 == correctly failed-closed/bounded; 1 == fabricated success (bug); 2 == would-wedge.
static int vm_dropped_reply_outcome(int reply_produced) {
	int parent_completed_success = 0;
	int parent_failed = 0;
	int bounded = 1; // bounded by construction (gr_duplex_wait_reply ~3s cap + server scope)

	if (reply_produced) {
		parent_completed_success = 1;
	} else {
#ifdef DUPLEX_VM_FABRICATE_ON_DROP
		parent_completed_success = 1; // RED: fabricate success on a dropped reply
#else
		parent_failed = 1;            // GREEN: fail closed within the bound
#endif
	}

	if (!reply_produced && parent_completed_success) return 1;
	if (!bounded) return 2;
	(void)parent_failed;
	return 0;
}

int main(void) {
	struct VMState s;
	vm_dealloc_run(&s);
	fprintf(stderr, "      (vm_dealloc: routed_on_duplex=%d s2c_delivered=%d munmap_count=%d parent_completed=%d fell_back=%d)\n",
		s.routed_on_duplex, s.s2c_delivered, s.munmap_count, s.parent_completed, s.fell_back_to_uds);

	// MODEL A: the munmap S2C MUST fire -- it is the op's whole effect.
	CHECK(s.s2c_delivered == 1,
	      "VM-S2C-REQUIRED: vm_deallocate over duplex MUST deliver the munmap S2C (ring_duplex_s2c > 0)");
	CHECK(!(s.parent_completed && !s.s2c_delivered),
	      "VM-NO-FALSE-COMPLETE: the parent never completes-success without the munmap S2C having run");

	// MODEL B: exactly one munmap of the caller range; no partial-then-UDS second munmap.
	CHECK(s.munmap_count == 1,
	      "VM-MUNMAP-ONCE: the caller range is munmap'd EXACTLY ONCE (no reused-address double-munmap)");
	CHECK(!s.fell_back_to_uds,
	      "VM-NO-PARTIAL-THEN-UDS: an op that began mutating on the duplex lane never re-runs over UDS");

	// MODEL C: dropped reply fails closed + bounded.
	CHECK(vm_dropped_reply_outcome(/*reply_produced=*/1) == 0,
	      "VM-DROPPED-REPLY (reply present): the parent completes normally");
	int dropped = vm_dropped_reply_outcome(/*reply_produced=*/0);
	fprintf(stderr, "      (dropped-reply outcome code: %d  [0=fail-closed ok, 1=fabricated, 2=wedge])\n", dropped);
	CHECK(dropped == 0,
	      "VM-DROPPED-REPLY (reply lost): the parent FAILS CLOSED + BOUNDED (no fabricated success, no wedge)");

	if (failures) {
		fprintf(stderr, "\nring_duplex_vm_dealloc_gate_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_duplex_vm_dealloc_gate_test: vm_deallocate-via-duplex S2C-required + once-only + fail-closed-bounded\n");
	return 0;
}
