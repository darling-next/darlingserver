// perf#18 Phase C/D (P8, dar-1il.3 / dar-1il.3.1): RED->GREEN gate on the DUPLEX-lane wake model.
//
// This is the HARD GATE the duplex lane is built on -- the load-bearing correctness artifact. It is
// pure logic (no boot, no sockets, no live ring), hermetic, instant, deterministic. It proves the
// wake model is lost-wake-free BEFORE any transport is wired, exactly as dar-my8 (ring_wake_race_test)
// did for the simple ring. The duplex lane is rejected if this gate is not GREEN.
//
// THE DIFFERENCE FROM THE SIMPLE RING: the simple ring has ONE waiter relationship (guest waits for
// its single reply; server wakes it). The duplex lane has the guest, WHILE parked, simultaneously
//   (a) waiting for its FINAL reply, AND
//   (b) obligated to wake for an S2C UPCALL request from the server (which it services on THIS thread).
// So a parked guest watches TWO producer streams: the s2c-UPCALL ring and the s2c-FINAL-REPLY ring.
// And the server, parked waiting for the C2S UPCALL REPLY, watches the c2s-upcall-reply ring.
//
// We enumerate EVERY interleaving of each side's race-window micro-ops (same deterministic enumerator
// shape as dar-my8) and assert ZERO lost-wake / lost-progress / mis-correlation. The RED arms break
// each mechanism so the enumeration finds > 0 and the binary exits nonzero (the gate's RED contract).
//
// GREEN (default): guest drains ALL upcalls before parking (while, not if); guest sets its waiter bit
//   BEFORE its pre-park recheck of BOTH rings; replies are accepted ONLY on matching correlation id.
// RED -DDUPLEX_NO_GUEST_PUMP: guest parks for the final reply WITHOUT draining the upcall ring first
//   -> an outstanding S2C upcall is stranded (server blocked on its reply, guest asleep) -> the
//   enumeration finds lost-progress > 0.
// RED -DDUPLEX_WAITERBIT_AFTER_RECHECK: guest sets its waiter bit AFTER rechecking the rings -> the
//   server can read bit==0 between recheck and park and skip the wake -> lost wake > 0.
// RED -DDUPLEX_WRONG_CORRELATION: an upcall reply with a mismatched id is accepted -> the server
//   resumes on the wrong reply -> the correlation check finds a protocol error > 0.

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
// Generic interleaving enumerator (same shape as dar-my8 ring_wake_race_test): run every relative
// ordering of two sides' in-order micro-ops; tally the states where `lost(state)` holds.
// ============================================================================================
typedef void (*step_fn)(void* st, int pc);
typedef void (*reset_fn)(void* st);
typedef int  (*lost_fn)(void* st);

static long enumerate(int nA, int nB, size_t stsz,
                      reset_fn reset, step_fn aStep, step_fn bStep, lost_fn lost) {
	long lostCount = 0;
	int total = nA + nB;
	_Alignas(64) unsigned char st[1024];
	if (stsz > sizeof(st)) { fprintf(stderr, "state too big\n"); exit(2); }
	for (long mask = 0; mask < (1L << total); ++mask) {
		if (__builtin_popcountl(mask) != nB) continue;
		reset(st);
		int ap = 0, bp = 0;
		for (int k = 0; k < total; ++k) {
			if (mask & (1L << k)) bStep(st, bp++); else aStep(st, ap++);
		}
		if (lost(st)) ++lostCount;
	}
	return lostCount;
}

// ============================================================================================
// MODEL 1 — DUPLEX-GUEST park: a guest parked for its FINAL reply must also wake for an S2C UPCALL.
//
// Scenario: the server, mid-parent-op, has just published an S2C UPCALL to the caller (the event the
// guest must not sleep through). We model the guest's pre-park sequence racing the server's
// publish+conditional-wake, with the guest obligated to drain the upcall before parking.
//
// We model the guest's PRE-PARK decision sequence (it is about to FUTEX_WAIT awaiting EITHER an S2C
// upcall OR its final reply) racing the server's publish-an-upcall + conditional-wake. This is the
// response-side discipline (publish-before-read-bit) but the producer is the S2C UPCALL ring -- the
// new stream the duplex guest must also watch. Identical lost-wake invariant as the simple ring's
// reply side, now proven for the upcall stream.
//
// Server micro-ops, in order:  S0 publish S2C upcall;  S1 conditional wake (read bit AFTER publish).
// Guest micro-ops (pre-park), in order:
//   G0 set waiter bit         [GREEN: before recheck]   (RED ...AFTER_RECHECK: skip here)
//   G1 recheck the upcall ring; if an upcall is visible, handle it (do not park)
//   G2 set waiter bit [RED ...AFTER_RECHECK only]; FUTEX_WAIT: park iff no upcall visible & no wake pending
// LOST iff: guest parks AND an upcall is pending+unhandled AND no wake pending (sleeps through it).
struct GState {
	dserver_ring_shm_t cb;
	int upcall_pending;   // an S2C upcall published, not yet handled
	int wake_pending;     // server's FUTEX_WAKE token
	int guest_parked;     // committed to FUTEX_WAIT, not released
	int upcall_handled;   // guest serviced the upcall
};
static void g_reset(void* p) {
	struct GState* s = (struct GState*)p;
	memset(&s->cb, 0, sizeof(s->cb));
	s->upcall_pending = 0; s->wake_pending = 0; s->guest_parked = 0; s->upcall_handled = 0;
}
static void g_server_step(void* p, int pc) {
	struct GState* s = (struct GState*)p;
	switch (pc) {
		case 0: s->upcall_pending = 1; break; // publish S2C upcall
		case 1: // conditional wake: read the waiter bit AFTER publishing
			if (dserver_ring_server_should_wake(&s->cb)) {
				if (s->guest_parked) { s->guest_parked = 0; if (s->upcall_pending) { s->upcall_pending = 0; s->upcall_handled = 1; } }
				else s->wake_pending = 1;
			}
			break;
	}
}
static void g_guest_step(void* p, int pc) {
	struct GState* s = (struct GState*)p;
	switch (pc) {
		case 0:
#ifndef DUPLEX_WAITERBIT_AFTER_RECHECK
			__atomic_store_n(&s->cb.s2c_waiters, 1u, __ATOMIC_RELEASE); // GREEN: bit before recheck
#endif
			break;
		case 1: // pre-park recheck of the upcall ring
			if (s->upcall_pending) { s->upcall_pending = 0; s->upcall_handled = 1; }
			break;
		case 2:
#ifdef DUPLEX_WAITERBIT_AFTER_RECHECK
			__atomic_store_n(&s->cb.s2c_waiters, 1u, __ATOMIC_RELEASE); // RED: bit AFTER recheck
#endif
			if (s->upcall_handled) break;
			if (s->wake_pending) { s->wake_pending = 0; if (s->upcall_pending) { s->upcall_pending = 0; s->upcall_handled = 1; } }
			else s->guest_parked = 1;
			break;
	}
}
// LOST: guest parked with an upcall still pending+unhandled and no wake pending.
static int g_lost(void* p) {
	struct GState* s = (struct GState*)p;
	return s->guest_parked && s->upcall_pending && !s->upcall_handled && !s->wake_pending;
}

// ---- MODEL 1b — PUMP DRAINS ALL (the DUPLEX_NO_GUEST_PUMP bug) -------------------------------
// A burst of TWO S2C upcalls is delivered (or a 2nd arrives while the guest services the 1st). The
// pump must drain ALL available upcalls (a `while`), not just one (an `if`), before parking -- else
// the 2nd is stranded while the guest sleeps for the final reply and the server blocks on its reply.
// This is a sequential property (no interleaving needed): GREEN `while` drains both; RED
// DUPLEX_NO_GUEST_PUMP models the `if`/one-shot drain that leaves the 2nd upcall pending.
static int pump_strands_an_upcall(void) {
	int upcalls_pending = 2; // a burst of two
	int handled = 0;
#ifdef DUPLEX_NO_GUEST_PUMP
	if (upcalls_pending > 0) { upcalls_pending--; handled++; }      // RED: one-shot drain
#else
	while (upcalls_pending > 0) { upcalls_pending--; handled++; }   // GREEN: drain ALL
#endif
	(void)handled;
	return upcalls_pending != 0; // true == an upcall was stranded (the bug)
}

// ============================================================================================
// MODEL 2 — DUPLEX-SERVER park: the server, parked waiting for the C2S UPCALL REPLY, must wake when
// the guest publishes it. Symmetric to the simple ring's response side, on the upcall-reply ring's
// own waiter bit. (We reuse s2c_waiters as the modeled bit; in the real lane it is a distinct word.)
//
// Guest micro-ops (publishing the upcall reply + conditional wake): G0 publish reply; G1 wake if bit set.
// Server micro-ops (pre-park): S0 set waiter bit [GREEN before recheck]; S1 recheck reply ring;
//   S2 set bit [RED AFTER] + park iff no reply & no wake pending.
struct SState {
	dserver_ring_shm_t cb;
	int reply_pending; int wake_pending; int server_parked; int observed;
};
static void s_reset(void* p) {
	struct SState* s = (struct SState*)p;
	memset(&s->cb, 0, sizeof(s->cb));
	s->reply_pending = 0; s->wake_pending = 0; s->server_parked = 0; s->observed = 0;
}
static void s_guest_step(void* p, int pc) {
	struct SState* s = (struct SState*)p;
	switch (pc) {
		case 0: s->reply_pending = 1; break; // publish C2S upcall reply
		case 1:
			if (dserver_ring_server_should_wake(&s->cb)) {
				if (s->server_parked) { s->server_parked = 0; if (s->reply_pending) { s->reply_pending = 0; s->observed = 1; } }
				else s->wake_pending = 1;
			}
			break;
	}
}
static void s_server_step(void* p, int pc) {
	struct SState* s = (struct SState*)p;
	switch (pc) {
		case 0:
#ifndef DUPLEX_WAITERBIT_AFTER_RECHECK
			__atomic_store_n(&s->cb.s2c_waiters, 1u, __ATOMIC_RELEASE); // GREEN
#endif
			break;
		case 1: if (s->reply_pending) { s->reply_pending = 0; s->observed = 1; } break; // recheck
		case 2:
#ifdef DUPLEX_WAITERBIT_AFTER_RECHECK
			__atomic_store_n(&s->cb.s2c_waiters, 1u, __ATOMIC_RELEASE); // RED
#endif
			if (s->observed) break;
			if (s->wake_pending) { s->wake_pending = 0; if (s->reply_pending) { s->reply_pending = 0; s->observed = 1; } }
			else s->server_parked = 1;
			break;
	}
}
static int s_lost(void* p) {
	struct SState* s = (struct SState*)p;
	return s->server_parked && s->reply_pending && !s->observed;
}

// ============================================================================================
// MODEL 3 — CORRELATION: a reply is accepted ONLY if its id matches the awaited id. We model the
// server awaiting upcall_id=A and a reply arriving with some id; a mismatched accept is a protocol
// error. The GREEN code checks the id; RED -DDUPLEX_WRONG_CORRELATION drops the check.
static int correlation_accepts_mismatch(uint32_t awaited, uint32_t arrived) {
#ifdef DUPLEX_WRONG_CORRELATION
	(void)awaited; (void)arrived; return 1; // RED: accept anything
#else
	return awaited == arrived; // GREEN: accept only the matching id
#endif
}

int main(void) {
	// MODEL 1: DUPLEX-GUEST pre-park never sleeps through a published S2C upcall.
	long g_lostc = enumerate(2 /*server steps*/, 3 /*guest steps*/, sizeof(struct GState),
		g_reset, g_server_step, g_guest_step, g_lost);
	fprintf(stderr, "      (duplex-guest lost-wake interleavings: %ld)\n", g_lostc);
	CHECK(g_lostc == 0, "DUPLEX-GUEST: no interleaving parks through a published S2C upcall");

	// MODEL 1b: the pump drains ALL upcalls in a burst (while, not if).
	CHECK(!pump_strands_an_upcall(),
	      "DUPLEX-PUMP: the guest pump drains ALL pending upcalls (a burst leaves none stranded)");

	// MODEL 2: DUPLEX-SERVER park never sleeps through a published upcall reply.
	long s_lostc = enumerate(3 /*server steps*/, 2 /*guest steps*/, sizeof(struct SState),
		s_reset, s_server_step, s_guest_step, s_lost);
	fprintf(stderr, "      (duplex-server lost-wake interleavings: %ld)\n", s_lostc);
	CHECK(s_lostc == 0, "DUPLEX-SERVER: no interleaving parks through a published upcall reply");

	// MODEL 3: correlation. The awaited upcall_id is A; a reply with a DIFFERENT id must be rejected,
	// a reply with the SAME id accepted.
	CHECK(correlation_accepts_mismatch(0xA11CE, 0xA11CE),
	      "correlation: matching upcall_id is accepted");
	CHECK(!correlation_accepts_mismatch(0xA11CE, 0xBEEF),
	      "correlation: mismatched upcall_id is REJECTED (no resume on the wrong reply)");
	// final-reply correlation by parent_id is the same predicate.
	CHECK(!correlation_accepts_mismatch(/*parent*/7u, /*arrived*/9u),
	      "correlation: mismatched parent_id final reply is REJECTED");

	if (failures) {
		fprintf(stderr, "\nring_duplex_wake_gate_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_duplex_wake_gate_test: duplex wake model is lost-wake-free + correlation-safe\n");
	return 0;
}
