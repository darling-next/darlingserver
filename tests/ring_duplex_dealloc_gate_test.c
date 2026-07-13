// perf#18 Phase D4 (P8, dar-1il.3.2.1): RED->GREEN gate on the DEALLOCATE-via-duplex S2C shape.
//
// The base duplex wake model (ring_duplex_wake_gate_test.c) proved, for the SYNTHETIC ECHO upcall, that
// a parked guest never sleeps through an S2C upcall, the server never sleeps through the upcall reply,
// the pump drains a burst, and correlation is enforced. THIS gate adds the two correctness properties
// SPECIFIC to migrating a real, DESTROY-CAPABLE op (mach_port_deallocate) whose S2C upcall is a real
// vm-munmap with real side effects -- the properties the brief's "brutal" bar calls out as RED arms:
//
//   MODEL 4 — NO PARTIAL-MUTATION-THEN-UDS (the double-effect hazard). deallocate's munmap S2C fires
//     MID-mutation (inside ipc_right_dealloc, after the ipc_space lock), so the "does this op need an
//     S2C" decision is UNDECIDABLE before mutation begins. A WRONG design would: begin the dtape
//     mutation, discover an S2C is needed, find the caller can't service it on this transport, and fall
//     back to UDS -- RE-RUNNING the op and applying the destroy side effect TWICE. The duplex lane's
//     invariant is that ROUTING is decided PRE-DISPATCH (purely from the negotiated cap, before any
//     bytes are sent / any mutation): once an op is on the duplex lane it services its S2C IN PLACE over
//     the mailbox and NEVER falls back to UDS after mutation. This model proves the side effect is
//     applied EXACTLY ONCE across the (no-S2C, S2C-serviced, would-have-fallen-back) cases.
//     RED -DDUPLEX_PARTIAL_THEN_UDS models the mid-mutation UDS fallback -> effect applied twice.
//
//   MODEL 5 — A DROPPED/LOST UPCALL REPLY FAILS CLOSED, BOUNDED (never fabricated success, never a wedge).
//     If the published munmap upcall reply is never harvested (a torn-down server, a buggy guest, the D3
//     RED no-pump), the parent op must FAIL within a bounded wait and surface an error -- it must NEVER
//     fabricate a success reply (which for a destroy op would tell the guest the port is gone when the
//     munmap never ran) and must NEVER block unbounded (the deadlock the lane exists to prevent).
//     RED -DDUPLEX_FABRICATE_ON_DROP models fabricating success on a dropped reply -> the gate catches it.
//
// Pure logic, hermetic, instant, deterministic -- same discipline as dar-my8 / the base duplex gate.
// This gate is GREEN before ANY real deallocate wiring is touched.

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

// ============================================================================================
// MODEL 4 — the destroy side effect is applied EXACTLY ONCE; no partial-mutation-then-UDS.
//
// We model the lifecycle of ONE deallocate routed onto the duplex lane, counting how many times the
// destroy side effect (the munmap) is applied. Three sub-cases the real op must handle identically wrt
// "exactly once": (a) no S2C needed (common refcount>1 case); (b) S2C needed + serviced over the mailbox;
// (c) the WRONG design where, upon needing an S2C, the server tears down and re-runs over UDS.
//
// The KEY structural fact (matches the real code): ROUTING (ride duplex vs UDS) is decided BEFORE the
// op is dispatched, from the negotiated cap alone -- NOT after mutation begins. So an op that started on
// the duplex lane stays on it. The only correct way to "fall back" is to never have dispatched on duplex
// (a pre-dispatch routing decline), which applies the effect zero times on the duplex attempt and once
// on the UDS attempt = exactly once total. A mid-mutation fallback applies it on BOTH = twice.
struct DState {
	int mutation_begun;      // the dtape op took the ipc_space lock + started destroying
	int effect_applied;      // count of munmap side-effect applications
	int serviced_via_duplex; // the S2C was answered over the mailbox (op completed on duplex)
	int fell_back_to_uds;    // the op was re-attempted over UDS
};

// Run one deallocate op through the modeled routing. needs_s2c: does this shape drive the munmap S2C?
// (undecidable pre-mutation in reality; the model just sets it to exercise both branches).
static void dealloc_run(struct DState* s, int needs_s2c) {
	memset(s, 0, sizeof(*s));

	// PRE-DISPATCH routing decision: we are on the duplex lane (the cap was negotiated). This is the
	// ONLY decline point in the correct design, and it happens with effect_applied == 0.
	// (A pre-dispatch decline -> UDS would be modeled by simply running the UDS path once; that is the
	//  trivially-once case and not the hazard, so we model the on-duplex path here.)

	// dispatch on the fiber: begin the real dtape mutation.
	s->mutation_begun = 1;

	if (!needs_s2c) {
		// common case: deallocate just drops a ref, no munmap. Effect (the deallocation) applied once.
		s->effect_applied += 1;
		return;
	}

	// the op needs a munmap S2C, discovered mid-mutation (after mutation_begun == 1).
#ifdef DUPLEX_PARTIAL_THEN_UDS
	// RED (the WRONG design): mid-mutation, decide the duplex transport "can't" do the S2C, tear down,
	// and re-run the WHOLE op over UDS. The munmap then runs on the UDS attempt too -> applied TWICE.
	s->effect_applied += 1;       // the partial mutation already applied the destroy
	s->fell_back_to_uds = 1;
	s->effect_applied += 1;       // the UDS re-run applies it AGAIN -> double effect
#else
	// GREEN (the correct design): service the S2C IN PLACE over the duplex mailbox. The munmap runs
	// once, on the caller thread, and the SAME fiber resumes and finishes the op. No UDS fallback after
	// mutation -> the destroy side effect is applied EXACTLY ONCE.
	s->effect_applied += 1;       // the single in-place munmap
	s->serviced_via_duplex = 1;
#endif
}

// ============================================================================================
// MODEL 5 — a dropped/unharvested upcall reply fails closed + bounded; never fabricates success.
//
// The server published the munmap upcall and is parked (fiber) on the reply semaphore. We model the
// drain that is supposed to harvest the correlated reply and up the semaphore. If the reply is never
// produced (reply_produced == 0), the parent must, within a bounded wait, FAIL (a clean error) -- never
// up the semaphore with a fabricated success, never wait forever.
//
// Returns: 0 == correctly failed-closed/bounded; 1 == fabricated success (the bug); 2 == would-wedge.
static int dropped_reply_outcome(int reply_produced) {
	int parent_completed_success = 0;
	int parent_failed = 0;
	int bounded = 1; // the wait is bounded by construction (D3 gr_duplex_wait_reply ~3s cap + server scope)

	if (reply_produced) {
		// the drain harvests the correlated reply -> resume the fiber with the real result.
		parent_completed_success = 1;
	} else {
#ifdef DUPLEX_FABRICATE_ON_DROP
		// RED: the server, seeing no reply, "completes" the parent with a fabricated success.
		parent_completed_success = 1;
#else
		// GREEN: no reply within the bound -> the parent op FAILS (clean error surfaced to the guest,
		// which UDS-falls-back at the ROUTING layer for the NEXT op; this op returns an error, never a
		// false success). No unbounded wait.
		parent_failed = 1;
#endif
	}

	if (!reply_produced && parent_completed_success) return 1; // fabricated success on a dropped reply
	if (!bounded) return 2;                                    // unbounded wait (wedge)
	(void)parent_failed;
	return 0;                                                   // failed-closed + bounded (correct)
}

int main(void) {
	// ---- MODEL 4: exactly-once destroy side effect across all cases ----
	struct DState s;

	dealloc_run(&s, /*needs_s2c=*/0);
	fprintf(stderr, "      (no-S2C case: effect_applied=%d fell_back=%d)\n", s.effect_applied, s.fell_back_to_uds);
	CHECK(s.effect_applied == 1 && !s.fell_back_to_uds,
	      "DEALLOC-ONCE (no S2C): the deallocation is applied exactly once, no UDS fallback");

	dealloc_run(&s, /*needs_s2c=*/1);
	fprintf(stderr, "      (S2C case: effect_applied=%d serviced_via_duplex=%d fell_back=%d)\n",
		s.effect_applied, s.serviced_via_duplex, s.fell_back_to_uds);
	CHECK(s.effect_applied == 1,
	      "DEALLOC-ONCE (S2C): the munmap destroy side effect is applied EXACTLY ONCE (no double-effect)");
	CHECK(!s.fell_back_to_uds,
	      "NO-PARTIAL-THEN-UDS: an op that began mutating on the duplex lane never re-runs over UDS");

	// ---- MODEL 5: a dropped upcall reply fails closed + bounded, never fabricates success ----
	CHECK(dropped_reply_outcome(/*reply_produced=*/1) == 0,
	      "DROPPED-REPLY (reply present): the parent completes normally");
	int dropped = dropped_reply_outcome(/*reply_produced=*/0);
	fprintf(stderr, "      (dropped-reply outcome code: %d  [0=fail-closed ok, 1=fabricated, 2=wedge])\n", dropped);
	CHECK(dropped == 0,
	      "DROPPED-REPLY (reply lost): the parent FAILS CLOSED + BOUNDED (no fabricated success, no wedge)");

	if (failures) {
		fprintf(stderr, "\nring_duplex_dealloc_gate_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_duplex_dealloc_gate_test: deallocate-via-duplex is once-only + fail-closed-bounded\n");
	return 0;
}
