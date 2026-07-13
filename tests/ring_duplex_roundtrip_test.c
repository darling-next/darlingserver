// perf#18 P8 D1/D2 (dar-1il.3.1): LIVE synthetic duplex roundtrip over the REAL mailbox helpers.
//
// The wake MODEL is proven deterministically by ring_duplex_wake_gate_test.c. THIS test proves the
// actual TRANSPORT helpers (dserver_ring_duplex_* in rpc-supplement.h) carry one synthetic S2C upcall
// from a server thread to a "guest" thread running the wait-pump, and one upcall reply back, on a REAL
// shared mailbox (a heap dserver_ring_shm_t) with two real threads + real memory ordering. It is the
// load-bearing artifact for D1/D2: the model on paper vs. the model in running code.
//
// Roundtrip (the exact sequence the bead specifies):
//   guest:  send parent request (parent_id); enter wait-pump.
//   server: run parent; publish S2C_UPCALL(parent_id, upcall_id, ECHO, arg); scope-wait for reply.
//   guest:  pump sees the upcall, handles it ON THIS (caller) thread (echo transform + records its
//           tid to prove caller-thread context), publishes reply with matching parent_id/upcall_id.
//   server: accepts the correlated reply, resumes, publishes the final reply.
//   guest:  observes the final reply; result == echo(arg).
//
// Acceptance checked here (subset that is hermetic; boot-level items are in the live system smoke):
//   1. synthetic duplex upcall completes (final result == echo(arg));
//   2. wrong parent_id/upcall_id reply is REJECTED (server flags mismatch, does not resume on it);
//   3. a SEQUENCE of upcalls (the minimal protocol is ONE-outstanding-at-a-time) all complete and
//      fold correctly -- echo applied per upcall;
//   4. caller-thread context is proven (the pump records the guest thread's tid; it is the guest
//      thread, never the server thread).
//
// NOTE ON BURST-DRAIN: the minimal D1/D2 protocol allows ONE outstanding upcall at a time (the server
// sends upcall N, waits for its reply, only then sends N+1). So two upcalls are never simultaneously
// pending here, and the guest pump's `while` vs one-shot `if` is not observably different in THIS
// transport (the server serializes them). The abstract "pump must drain ALL of a simultaneous burst"
// property is proven separately + deterministically by ring_duplex_wake_gate_test.c (MODEL 1b); it is
// a property of a FUTURE multi-inflight lane, explicitly a non-goal of D1/D2. We do NOT fake a burst
// here -- that would be a dishonest gate. This test pins the one-outstanding sequence instead.
//
// GREEN: the real helpers complete the roundtrip + reject mismatches + fold a sequence of upcalls.
// RED -DDUPLEX_RT_IGNORE_CORRELATION: the server accepts the reply WITHOUT checking parent/upcall id
//   -> the mismatch case is wrongly accepted -> the test fails.

#define _POSIX_C_SOURCE 200112L
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define DSERVER_RING_TRANSPORT 1
#define DSERVER_RING_NO_ATTACH_CHECK 1
#include <darlingserver/rpc.h>
#include <darlingserver/rpc-supplement.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdatomic.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
	else         { fprintf(stderr, "ok:   %s\n", (msg)); } \
} while (0)

static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }

// shared state for one roundtrip
typedef struct {
	dserver_ring_shm_t cb;            // the real mailbox (only the duplex words are used here)
	uint32_t parent_id;
	uint32_t arg;                     // echo input
	int      upcalls_to_send;         // how many S2C upcalls the server sends for this parent (burst=2)
	_Atomic int final_ready;          // server -> guest: parent final reply published
	uint32_t final_result;            // the parent op's result (== echo(arg))
	int32_t  final_status;
	// observability
	_Atomic pid_t pump_tid;           // the tid that ran the upcall pump (must be the guest thread)
	_Atomic int   protocol_error;     // server saw a mis-correlated reply
	_Atomic int   handled_count;      // how many upcalls the guest pump handled
	pid_t guest_tid;
	pid_t server_tid;
	_Atomic int stop;
} RT;

// ---- GUEST side: send parent + run the wait-pump (the bead's loop) --------------------------------
static void* guest_fn(void* p) {
	RT* rt = (RT*)p;
	rt->guest_tid = gettid_();
	// (parent request is "sent" by the harness setting up rt; in the real lane this is a c2s publish)
	for (;;) {
		if (atomic_load_explicit(&rt->final_ready, memory_order_acquire)) {
			return NULL; // final reply observed -> parent op done
		}
		// pump: drain available S2C upcalls, handle each ON THIS THREAD, reply with matching ids.
		// (while-pump per the bead's loop; the one-outstanding protocol means at most one is pending
		// per iteration, but the while is the correct shape for the future multi-inflight lane.)
		while (dserver_ring_duplex_upcall_available(&rt->cb)) {
			uint32_t op     = rt->cb.duplex_upcall_op;
			uint32_t parent = rt->cb.duplex_upcall_parent;
			uint32_t uid    = rt->cb.duplex_upcall_id;
			uint32_t a      = rt->cb.duplex_upcall_arg;
			atomic_store_explicit(&rt->pump_tid, gettid_(), memory_order_release); // caller-thread proof
			int32_t status = 0;
			uint32_t result = 0;
			if (op == DSERVER_RING_DUPLEX_UPCALL_ECHO) {
				result = dserver_ring_duplex_echo_transform(a);
			} else {
				status = -1; // unsupported shape
			}
			dserver_ring_duplex_publish_reply(&rt->cb, parent, uid, status, result);
			atomic_fetch_add_explicit(&rt->handled_count, 1, memory_order_acq_rel);
		}
		// adaptive spin (the real lane spins-then-FUTEX_WAITs; the host model just yields)
		sched_yield();
		if (atomic_load_explicit(&rt->stop, memory_order_acquire)) return NULL;
	}
}

// ---- SERVER side: run parent, publish upcall(s), scope-wait the reply, publish final reply --------
// The scope-wait is BOUNDED and busy-polls the mailbox -- it does NOT block any global structure
// (mirrors the requirement that waiting for the upcall reply is scoped to the operation, not the
// whole dserver). We model "other clients live" by asserting this wait returns control via a bounded
// loop rather than an unbounded block.
static int server_run_parent(RT* rt) {
	rt->server_tid = gettid_();
	uint32_t parent = rt->parent_id;
	uint32_t result_accum = rt->arg;

	for (int n = 0; n < rt->upcalls_to_send; ++n) {
		uint32_t uid = 0x100u + (uint32_t)n; // unique per upcall
		dserver_ring_duplex_publish_upcall(&rt->cb, DSERVER_RING_DUPLEX_UPCALL_ECHO, parent, uid, result_accum);

		// scope-wait for THIS upcall's reply (bounded; yields so the guest thread runs).
		int got = 0;
		for (long spins = 0; spins < 50000000L && !got; ++spins) {
			int mismatch = 0;
#ifdef DUPLEX_RT_IGNORE_CORRELATION
			// RED: accept any ready reply without checking correlation.
			if (atomic_load_explicit((_Atomic uint32_t*)&rt->cb.duplex_reply_ready, memory_order_acquire)) {
				(void)mismatch; got = 1;
			}
#else
			if (dserver_ring_duplex_reply_ready(&rt->cb, parent, uid, &mismatch)) {
				got = 1;
			} else if (mismatch) {
				atomic_store_explicit(&rt->protocol_error, 1, memory_order_release);
				dserver_ring_duplex_consume_reply(&rt->cb); // drop the bad reply; do NOT resume on it
				return -1; // protocol error -> parent op fails (would UDS-fall-back in the real lane)
			}
#endif
			if (!got) sched_yield();
		}
		if (!got) return -2; // timed out waiting for the upcall reply (lost wake / strand)

		// resume: fold the echo result; consume the reply slot.
		result_accum = rt->cb.duplex_reply_arg;
		dserver_ring_duplex_consume_reply(&rt->cb);
	}

	// publish the final reply for the parent op.
	rt->final_result = result_accum;
	rt->final_status = 0;
	atomic_store_explicit(&rt->final_ready, 1, memory_order_release);
	return 0;
}

// Run one roundtrip with `upcalls` upcalls; returns the server_run_parent rc, fills *out.
static int run_roundtrip(uint32_t parent_id, uint32_t arg, int upcalls, RT* out) {
	memset(out, 0, sizeof(*out));
	out->parent_id = parent_id;
	out->arg = arg;
	out->upcalls_to_send = upcalls;
	pthread_t gt;
	pthread_create(&gt, NULL, guest_fn, out);
	int rc = server_run_parent(out);
	atomic_store_explicit(&out->stop, 1, memory_order_release);
	pthread_join(gt, NULL);
	return rc;
}

// Mis-correlation roundtrip: the guest replies with a DELIBERATELY WRONG upcall_id. The server's
// correlation check must flag a protocol error and refuse to resume. (Separate guest fn.)
static RT* g_mis_rt;
static void* guest_fn_miscorrelate(void* p) {
	RT* rt = (RT*)p;
	rt->guest_tid = gettid_();
	for (;;) {
		if (atomic_load_explicit(&rt->final_ready, memory_order_acquire)) return NULL;
		if (dserver_ring_duplex_upcall_available(&rt->cb)) {
			uint32_t parent = rt->cb.duplex_upcall_parent;
			uint32_t uid    = rt->cb.duplex_upcall_id;
			uint32_t a      = rt->cb.duplex_upcall_arg;
			// reply with a WRONG upcall_id (uid ^ 0xFFFF) -- the correlation check must reject it.
			dserver_ring_duplex_publish_reply(&rt->cb, parent, uid ^ 0xFFFFu,
				0, dserver_ring_duplex_echo_transform(a));
			return NULL;
		}
		sched_yield();
		if (atomic_load_explicit(&rt->stop, memory_order_acquire)) return NULL;
	}
}

int main(void) {
	// --- 1. happy path: one upcall, completes, result == echo(arg) ---------------------------
	{
		RT rt;
		uint32_t arg = 0x12345678u;
		int rc = run_roundtrip(/*parent*/7u, arg, /*upcalls*/1, &rt);
		CHECK(rc == 0, "synthetic duplex roundtrip completes (server resumed + published final reply)");
		CHECK(rt.final_result == dserver_ring_duplex_echo_transform(arg),
		      "final result == echo(arg) (the upcall ran and its result folded into the parent)");
		// caller-thread context: the pump ran on the GUEST thread, never the server thread.
		CHECK(atomic_load_explicit(&rt.pump_tid, memory_order_acquire) == rt.guest_tid,
		      "caller-thread context: upcall handled on the guest (caller) thread");
		CHECK(rt.pump_tid != rt.server_tid,
		      "caller-thread context: upcall NOT handled on the server thread");
		fprintf(stderr, "      (guest_tid=%d server_tid=%d pump_tid=%d)\n",
		        rt.guest_tid, rt.server_tid, (int)rt.pump_tid);
	}

	// --- 3. sequence: TWO one-outstanding upcalls both complete + fold correctly --------------
	// (NOT a simultaneous burst -- the minimal protocol is one-outstanding; see the header note.
	// Simultaneous-burst drain is proven abstractly by ring_duplex_wake_gate_test.c MODEL 1b.)
	{
		RT rt;
		int rc = run_roundtrip(/*parent*/9u, 0xABCD0000u, /*upcalls*/2, &rt);
		CHECK(rc == 0, "sequenced duplex roundtrip completes (both one-outstanding upcalls handled)");
		CHECK(atomic_load_explicit(&rt.handled_count, memory_order_acquire) == 2,
		      "sequence: the guest pump handled both upcalls across the two scope-waits");
		// echo applied twice: echo(echo(arg))
		uint32_t expect = dserver_ring_duplex_echo_transform(dserver_ring_duplex_echo_transform(0xABCD0000u));
		CHECK(rt.final_result == expect, "sequence: final result == echo(echo(arg))");
	}

	// --- 2. wrong correlation: the server must reject a mis-correlated reply ------------------
	{
		RT rt;
		memset(&rt, 0, sizeof(rt));
		rt.parent_id = 11u; rt.arg = 0x55u; rt.upcalls_to_send = 1;
		g_mis_rt = &rt;
		pthread_t gt;
		pthread_create(&gt, NULL, guest_fn_miscorrelate, &rt);
		int rc = server_run_parent(&rt);
		atomic_store_explicit(&rt.stop, 1, memory_order_release);
		pthread_join(gt, NULL);
		CHECK(rc == -1, "wrong upcall_id reply is REJECTED (server returns protocol error, does not resume)");
		CHECK(atomic_load_explicit(&rt.protocol_error, memory_order_acquire) == 1,
		      "wrong upcall_id reply FLAGGED as a protocol error");
		CHECK(atomic_load_explicit(&rt.final_ready, memory_order_acquire) == 0,
		      "wrong upcall_id: server did NOT publish a final reply (no resume on bad correlation)");
	}

	// --- 4. perf #18 P8 D4: the REAL munmap-shape mailbox payload carries losslessly --------------
	// The deallocate-via-duplex S2C is a munmap upcall: the server publishes (op=MUNMAP, addr, len) and
	// the guest replies (return_value, errno). Prove the v5 typed payload words roundtrip exactly through
	// the publish/observe/consume helpers (pure, no thread needed -- the threaded path is identical to
	// the echo case proven above; this pins the TYPED words the echo path doesn't touch).
	{
		dserver_ring_shm_t cb;
		memset(&cb, 0, sizeof(cb));
		uint64_t addr = 0x00007f1122330000ull;
		uint64_t len  = 0x0000000000004000ull; // 16 KiB
		dserver_ring_duplex_publish_munmap_upcall(&cb, /*parent*/42u, /*upcall*/43u, addr, len);
		CHECK(dserver_ring_duplex_upcall_available(&cb), "munmap upcall published + visible");
		CHECK(cb.duplex_upcall_op == DSERVER_RING_DUPLEX_UPCALL_MUNMAP, "munmap upcall op is MUNMAP");
		CHECK(cb.duplex_upcall_addr == addr && cb.duplex_upcall_len == len,
		      "munmap upcall addr+len carry losslessly");
		// guest replies with the munmap result (success: return_value 0, errno 0).
		dserver_ring_duplex_publish_munmap_reply(&cb, 42u, 43u, /*return_value*/0, /*errno*/0);
		int mismatch = 0;
		CHECK(dserver_ring_duplex_reply_ready(&cb, 42u, 43u, &mismatch) && !mismatch,
		      "munmap reply correlated + accepted");
		CHECK(cb.duplex_reply_status == 0 && cb.duplex_reply_errno == 0,
		      "munmap reply return_value+errno carry (success)");
		dserver_ring_duplex_consume_reply(&cb);
		// error case: return_value -1 + errno carry too.
		dserver_ring_duplex_publish_munmap_upcall(&cb, 44u, 45u, addr, len);
		dserver_ring_duplex_publish_munmap_reply(&cb, 44u, 45u, /*return_value*/-1, /*errno*/22 /*EINVAL*/);
		CHECK(dserver_ring_duplex_reply_ready(&cb, 44u, 45u, &mismatch) && !mismatch,
		      "munmap error reply correlated + accepted");
		CHECK(cb.duplex_reply_status == -1 && cb.duplex_reply_errno == 22,
		      "munmap reply return_value+errno carry (error: -1/EINVAL)");
		// mis-correlated munmap reply is still rejected (correlation is shape-agnostic).
		dserver_ring_duplex_consume_reply(&cb);
		dserver_ring_duplex_publish_munmap_upcall(&cb, 46u, 47u, addr, len);
		dserver_ring_duplex_publish_munmap_reply(&cb, 46u, 0xBADu, 0, 0);
		CHECK(!dserver_ring_duplex_reply_ready(&cb, 46u, 47u, &mismatch) && mismatch,
		      "munmap reply with wrong upcall_id is REJECTED");
	}

	if (failures) {
		fprintf(stderr, "\nring_duplex_roundtrip_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_duplex_roundtrip_test: live synthetic duplex roundtrip OK\n");
	return 0;
}
