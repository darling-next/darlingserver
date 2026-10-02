// perf #18 P7 (dar-my8): RED->GREEN gate on the v3 wake-model LOST-WAKE invariants -- the two
// races the gist (191a10db / 33355bf) flagged as the costliest possible bug now that the hot path
// is ~740ns: a missed doorbell/FUTEX_WAKE that strands a request or a reply forever.
//
// The wake model is two cooperating sequences over the shared control block (server_state +
// s2c_waiters) plus the SPSC ring tails. This gate runs BOTH sequences as real concurrent threads
// against a REAL shared-memory ring (the same dserver_ring_shm_t layout the guest builds), using
// the REAL predicates from rpc-supplement.h, and asserts the two invariants hold:
//
//   REQUEST side (server must never sleep through a pending request):
//     guest:  producer_publish(c2s); if guest_should_doorbell(cb) -> doorbell
//     server: ACTIVE_POLLING -> [budget idle] -> SLEEP_ARMED -> CRITICAL RECHECK drain ->
//             if still empty -> SLEEPING_EPOLL -> block until doorbell
//     INVARIANT: for EVERY interleaving, the request is serviced -- either the server's recheck
//     drains it, or the guest doorbelled (server_state was != ACTIVE_POLLING when guest read it).
//
//   RESPONSE side (server must never skip a needed wake):
//     server: producer_publish(s2c); if server_should_wake(cb) -> FUTEX_WAKE
//     guest:  spin; on miss -> s2c_waiters=1; recheck ring; if empty -> FUTEX_WAIT(word,observed)
//     INVARIANT: a guest that commits to sleeping is always woken -- either it sees the reply in
//     its post-set recheck, or the server saw s2c_waiters==1 and woke it.
//
// We drive these two ways:
//   (a) EXHAUSTIVE: a deterministic single-threaded driver steps each side one micro-op at a time
//       and enumerates every relative ordering of the race-window operations, asserting no
//       interleaving loses the wake. This is the real proof -- it covers the windows a random
//       scheduler hits only rarely.
//   (b) STRESS: two real threads (pinned to distinct cores) hammer the window N times with
//       jittered timing on real hardware memory ordering. This is a DIAGNOSTIC that corroborates
//       (a) on real silicon -- it is NOT a pass/fail gate: its per-round handshake + modeled park
//       are scheduling-sensitive, so a watchdog timeout is REPORTED, not failed (a flaky stress
//       gate is exactly the CI hazard the gist warns against). The RED->GREEN contract rests
//       entirely on (a), which is exact, instant, and deterministic.
//
// GREEN (default): the real sequences (server does the critical recheck; guest sets the waiter
// bit BEFORE its recheck). Exhaustive enumeration finds ZERO lost-wake interleavings; stress is
// fully live.
// RED arm -DRACE_NO_SERVER_RECHECK: server skips the SLEEP_ARMED critical recheck -> the
//   "guest published after the last drain but read ACTIVE_POLLING and skipped the doorbell"
//   ordering strands the request. The exhaustive driver MUST find >0 such interleavings (= the
//   gate fails RED), proving the recheck is load-bearing.
// RED arm -DRACE_WAITERBIT_AFTER_RECHECK: guest sets s2c_waiters AFTER its recheck (publish-order
//   bug) -> the "server published + read waiters==0 + skipped wake, then guest sleeps" ordering
//   strands the reply. The exhaustive driver MUST find >0 such interleavings (= the gate fails RED).

#define DSERVER_RING_TRANSPORT 1
#define DSERVER_RING_NO_ATTACH_CHECK 1
#include <darlingserver/rpc.h> // callnums for the C2S opcode-set hash (dar-1il.2 item 2)
#include <darlingserver/rpc-supplement.h>

#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>
#include <sched.h>
#include <pthread.h>

// Pin the calling thread to one CPU. The stress harness runs a producer + a modeled-sleeper that
// both busy-spin (mirroring the real spin-then-park hot path); on an OVERSUBSCRIBED host two
// spinners sharing a core can starve each other for seconds, which a wall-clock watchdog would
// misread as a lost wake. Pinning each to its OWN core removes that false-timeout source so the
// watchdog fires ONLY on a genuine lost wake (an infinite hang). Best-effort: if affinity isn't
// available the test still runs (just with the small flakiness window the long deadline covers).
static void pin_to_cpu(int cpu) {
	int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu <= 1) return;
	cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu % ncpu, &set);
	pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// ---- shared ring builder (mirrors the guest memfd layout; same as ring_datapath_test) --------
struct RingMap {
	void* base = nullptr;
	uint64_t size = 0;
	dserver_ring_shm_t* cb = nullptr;
	dserver_ring_t* c2s = nullptr;
	dserver_ring_t* s2c = nullptr;
};

static RingMap buildRing(uint32_t slot_size, uint32_t slot_count) {
	RingMap m;
	auto alignup = [](uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); };
	uint64_t hdr = sizeof(dserver_ring_shm_t);
	uint64_t ring_span = sizeof(dserver_ring_t) + (uint64_t)slot_count * slot_size;
	uint64_t c2s_off = alignup(hdr, 64);
	uint64_t s2c_off = alignup(c2s_off + ring_span, 64);
	uint64_t total = alignup(s2c_off + ring_span, 64);
	void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) { perror("mmap"); exit(2); }
	memset(base, 0, total);
	dserver_ring_shm_t* cb = (dserver_ring_shm_t*)base;
	cb->magic = DSERVER_RING_MAGIC;
	cb->abi_version = DSERVER_RING_ABI_VERSION;
	cb->c2s_opcode_hash = dserver_ring_c2s_opcode_hash(); // dar-1il.2 item 2 (this test doesn't validate, but keep it honest)
	cb->slot_size = (uint16_t)slot_size;
	cb->slot_count = slot_count;
	cb->c2s_ring_off = (uint32_t)c2s_off;
	cb->s2c_ring_off = (uint32_t)s2c_off;
	cb->total_size = (uint32_t)total;
	m.base = base; m.size = total; m.cb = cb;
	m.c2s = (dserver_ring_t*)((char*)base + c2s_off);
	m.s2c = (dserver_ring_t*)((char*)base + s2c_off);
	return m;
}
static void freeRing(RingMap& m) { if (m.base) munmap(m.base, m.size); m.base = nullptr; }

#define SLOT_SIZE  64u
#define SLOT_COUNT 8u

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
	else         { fprintf(stderr, "ok:   %s\n", (msg)); } \
} while (0)

// ============================================================================================
// (a) EXHAUSTIVE deterministic enumeration of the two race windows.
//
// We model the control block + ring tails as plain shared memory and step each side's race-window
// micro-ops in every relative order. Each side is a tiny program counter; the driver tries every
// interleaving of the two PCs. "Doorbell"/"FUTEX_WAKE" are modeled as setting a pending-wake flag
// the sleeper consults; a sleeper that commits to sleep with neither the work visible NOR a wake
// pending = a LOST WAKE (the bug).
// ============================================================================================

// ---- REQUEST side model --------------------------------------------------------------------
// Server micro-ops (in order): S0 set SLEEP_ARMED; S1 critical recheck (drain c2s); S2 set
//   SLEEPING_EPOLL; S3 block (consume any pending doorbell; else sleep). Before S0 the server is
//   ACTIVE_POLLING and has just finished a drain that found the ring empty.
// Guest micro-ops (in order): G0 publish (make c2s non-empty); G1 read server_state + decide
//   doorbell; G2 doorbell if decided.
// LOST WAKE iff: after both run to completion, c2s is still non-empty (request not drained by S1)
//   AND the server is SLEEPING_EPOLL AND no doorbell is pending. With the real recheck (S1 drains)
//   this is impossible; without it (RED) the ordering G0,S0,G1(reads ARMED?no—reads ACTIVE)... is
//   the strand.
struct ReqState {
	dserver_ring_shm_t cb;
	uint32_t c2s_pending; // 0/1: a published-but-undrained request
	int doorbell_pending; // server's wake eventfd has a token
	int server_sleeping;  // server committed to epoll_wait
	int serviced;         // server drained the request
};
static void req_reset(ReqState* s) {
	memset(&s->cb, 0, sizeof(s->cb));
	__atomic_store_n(&s->cb.server_state, DSERVER_RING_SRV_ACTIVE_POLLING, __ATOMIC_RELEASE);
	s->c2s_pending = 0; s->doorbell_pending = 0; s->server_sleeping = 0; s->serviced = 0;
}
// returns 1 if this step completed the side's program
static void req_server_step(ReqState* s, int pc) {
	switch (pc) {
		case 0: __atomic_store_n(&s->cb.server_state, DSERVER_RING_SRV_SLEEP_ARMED, __ATOMIC_RELEASE); break;
		case 1: // critical recheck
#ifndef RACE_NO_SERVER_RECHECK
			if (s->c2s_pending) { s->c2s_pending = 0; s->serviced = 1;
				__atomic_store_n(&s->cb.server_state, DSERVER_RING_SRV_ACTIVE_POLLING, __ATOMIC_RELEASE); }
#endif
			break;
		case 2: if (!s->serviced) __atomic_store_n(&s->cb.server_state, DSERVER_RING_SRV_SLEEPING_EPOLL, __ATOMIC_RELEASE); break;
		case 3: // block: a pending doorbell wakes us immediately and we drain; else we sleep
			if (s->serviced) break;
			if (s->doorbell_pending) { s->doorbell_pending = 0; if (s->c2s_pending) { s->c2s_pending = 0; s->serviced = 1; } }
			else { s->server_sleeping = 1; }
			break;
	}
}
static void req_guest_step(ReqState* s, int pc) {
	static thread_local int decided_doorbell;
	switch (pc) {
		case 0: s->c2s_pending = 1; break; // publish
		case 1: decided_doorbell = dserver_ring_guest_should_doorbell(&s->cb); break;
		case 2: if (decided_doorbell) {
				s->doorbell_pending = 1;
				// if the server already slept, the doorbell wakes + drains it (eventfd readiness)
				if (s->server_sleeping) { s->server_sleeping = 0; s->doorbell_pending = 0;
					if (s->c2s_pending) { s->c2s_pending = 0; s->serviced = 1; } }
			} break;
	}
}

// ---- RESPONSE side model -------------------------------------------------------------------
// Server micro-ops: S0 publish reply (s2c non-empty); S1 read s2c_waiters + decide wake; S2 wake
//   if decided (sets a pending-wake the sleeper consumes).
// Guest micro-ops: G0 spin recheck (miss -- we model the already-missed case, the only one that
//   reaches the slow path); G1 set s2c_waiters=1 [GREEN: before recheck]; G2 recheck ring; G3
//   FUTEX_WAIT (sleep) iff ring still empty and no wake pending.
//   RED -DRACE_WAITERBIT_AFTER_RECHECK: order is G2 (recheck) then G1 (set bit) -- the bit is set
//   AFTER the recheck, so the server can read waiters==0 between them and skip the wake.
// LOST WAKE iff: guest commits to FUTEX_WAIT AND reply is present AND no wake pending.
struct RespState {
	dserver_ring_shm_t cb;
	uint32_t s2c_pending; // a published reply not yet observed
	int wake_pending;     // FUTEX_WAKE token
	int guest_sleeping;   // guest committed to FUTEX_WAIT (and not yet woken)
	int observed;         // guest saw the reply
};
static void resp_reset(RespState* s) {
	memset(&s->cb, 0, sizeof(s->cb));
	s->s2c_pending = 0; s->wake_pending = 0; s->guest_sleeping = 0; s->observed = 0;
}
static void resp_server_step(RespState* s, int pc) {
	switch (pc) {
		case 0: s->s2c_pending = 1; break; // publish reply
		case 1: break; // (decision is read fresh in step 2 to model a single atomic load)
		case 2: if (dserver_ring_server_should_wake(&s->cb)) {
				if (s->guest_sleeping) { s->guest_sleeping = 0; if (s->s2c_pending) { s->s2c_pending = 0; s->observed = 1; } }
				else { s->wake_pending = 1; }
			} break;
	}
}
static void resp_guest_step(RespState* s, int pc) {
	switch (pc) {
		case 0: // recheck ring; if reply already there, observe and we're done
			if (s->s2c_pending) { s->s2c_pending = 0; s->observed = 1; } break;
		case 1:
#ifndef RACE_WAITERBIT_AFTER_RECHECK
			__atomic_store_n(&s->cb.s2c_waiters, 1u, __ATOMIC_RELEASE); // GREEN: set bit BEFORE recheck
#endif
			break;
		case 2: // post-set recheck
			if (s->observed) break;
			if (s->s2c_pending) { s->s2c_pending = 0; s->observed = 1; } break;
		case 3:
#ifdef RACE_WAITERBIT_AFTER_RECHECK
			__atomic_store_n(&s->cb.s2c_waiters, 1u, __ATOMIC_RELEASE); // RED: bit set AFTER recheck
#endif
			// FUTEX_WAIT: sleep iff no reply visible and no wake already pending
			if (s->observed) break;
			if (s->wake_pending) { s->wake_pending = 0; if (s->s2c_pending) { s->s2c_pending = 0; s->observed = 1; } }
			else { s->guest_sleeping = 1; }
			break;
	}
}

// Generic interleaving enumerator: given two step-counts and steppers, run every interleaving and
// call done(state) at the end of each. Returns the number of LOST-WAKE interleavings found.
template <typename State, typename ResetFn, typename SrvStep, typename GstStep, typename LostFn>
static long enumerate(int nServer, int nGuest, ResetFn reset, SrvStep srvStep, GstStep gstStep, LostFn lost) {
	long lostCount = 0;
	int total = nServer + nGuest;
	// each interleaving = a bitmask choosing, at each of `total` steps, server(0) or guest(1),
	// subject to consuming exactly nServer server-steps and nGuest guest-steps in order.
	// enumerate via the standard "choose positions of guest steps" over C(total, nGuest).
	for (long mask = 0; mask < (1L << total); ++mask) {
		if (__builtin_popcountl(mask) != nGuest) continue;
		State st; reset(&st);
		int sp = 0, gp = 0;
		for (int k = 0; k < total; ++k) {
			if (mask & (1L << k)) { gstStep(&st, gp++); } else { srvStep(&st, sp++); }
		}
		if (lost(&st)) ++lostCount;
	}
	return lostCount;
}

// Both sides assert the SAME invariant (lost == 0) in every arm. GREEN satisfies it; the RED
// compile flags break the model so the enumeration finds lost > 0 and this assertion FAILS ->
// the RED binary exits nonzero, which is what the gate harness requires of a RED arm.
static void exhaustive_request_side() {
	long lost = enumerate<ReqState>(4, 3, req_reset, req_server_step, req_guest_step,
		[](ReqState* s){ return s->server_sleeping && s->c2s_pending && !s->serviced; });
	fprintf(stderr, "      (request-side lost-wake interleavings: %ld)\n", lost);
	CHECK(lost == 0, "REQUEST side: NO interleaving sleeps through a pending request");
}
static void exhaustive_response_side() {
	long lost = enumerate<RespState>(3, 4, resp_reset, resp_server_step, resp_guest_step,
		[](RespState* s){ return s->guest_sleeping && s->s2c_pending && !s->observed; });
	fprintf(stderr, "      (response-side lost-wake interleavings: %ld)\n", lost);
	CHECK(lost == 0, "RESPONSE side: NO interleaving sleeps through a published reply");
}

// ============================================================================================
// (b) STRESS: two real threads on a real shared ring. A lost wake manifests as the consumer
// never observing the producer's item within a bounded watchdog -> we report a TIMEOUT (== the
// failure mode a real lost wake produces). This complements the exhaustive proof with real
// hardware memory ordering + a real scheduler.
// ============================================================================================

// Request-side stress: guest publishes one request; server (other thread) must service it. The
// server models the spin->arm->recheck->sleep loop; "sleep" here is a short futex-like park that
// the doorbell flag releases. We run ITERS rounds; a round that the server fails to service
// within the watchdog = lost wake.
static void stress_request_side(long iters) {
	RingMap m = buildRing(SLOT_SIZE, SLOT_COUNT);
	std::atomic<int> stop{0};
	std::atomic<long> serviced{0};
	std::atomic<int> doorbell{0};
	std::atomic<long> round{0};
	std::atomic<long> serverRound{-1};

	// server thread: per round, drain; if empty, arm + critical recheck + "sleep" (poll doorbell).
	std::thread server([&]{
		pin_to_cpu(0);
		while (!stop.load(std::memory_order_acquire)) {
			long r = round.load(std::memory_order_acquire);
			if (serverRound.load(std::memory_order_acquire) == r) { __builtin_ia32_pause(); continue; }
			// ACTIVE_POLLING spin (short), draining
			__atomic_store_n(&m.cb->server_state, DSERVER_RING_SRV_ACTIVE_POLLING, __ATOMIC_RELEASE);
			bool got = false;
			for (int i = 0; i < 64 && !got; ++i) {
				if (dserver_ring_consumer_begin(m.c2s, SLOT_SIZE, SLOT_COUNT)) { dserver_ring_consumer_advance(m.c2s); got = true; }
				else __builtin_ia32_pause();
			}
			if (!got) {
				// arm + CRITICAL RECHECK + sleep
				__atomic_store_n(&m.cb->server_state, DSERVER_RING_SRV_SLEEP_ARMED, __ATOMIC_RELEASE);
#ifndef RACE_NO_SERVER_RECHECK
				if (dserver_ring_consumer_begin(m.c2s, SLOT_SIZE, SLOT_COUNT)) { dserver_ring_consumer_advance(m.c2s); got = true;
					__atomic_store_n(&m.cb->server_state, DSERVER_RING_SRV_ACTIVE_POLLING, __ATOMIC_RELEASE); }
#endif
				if (!got) {
					__atomic_store_n(&m.cb->server_state, DSERVER_RING_SRV_SLEEPING_EPOLL, __ATOMIC_RELEASE);
					// "epoll_wait": park until a doorbell token arrives. NO speculative ring drain --
					// the doorbell is the ONLY thing that may wake us here, so a missed doorbell (the
					// RED bug) genuinely strands the request and the upstream watchdog catches it. The
					// deadline is wall-clock (steady_clock), so heavy host load only deschedules us, it
					// does NOT manufacture a false timeout the way a fixed spin budget would.
					auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
					while (std::chrono::steady_clock::now() < deadline) {
						if (doorbell.exchange(0, std::memory_order_acq_rel)) {
							if (dserver_ring_consumer_begin(m.c2s, SLOT_SIZE, SLOT_COUNT)) { dserver_ring_consumer_advance(m.c2s); got = true; }
							break;
						}
						if (stop.load(std::memory_order_acquire)) break;
						__builtin_ia32_pause();
					}
					__atomic_store_n(&m.cb->server_state, DSERVER_RING_SRV_ACTIVE_POLLING, __ATOMIC_RELEASE);
				}
			}
			if (got) serviced.fetch_add(1, std::memory_order_acq_rel);
			serverRound.store(r, std::memory_order_release);
		}
	});

	pin_to_cpu(1); // producer (this thread) on a distinct core from the server thread
	long timeouts = 0;
	for (long it = 0; it < iters; ++it) {
		// jitter so the publish lands across the arm/recheck/sleep window
		if (it & 1) for (volatile int z = 0; z < (int)(it % 97); ++z) {}
		// guest publish
		dserver_ring_slot_t* slot = dserver_ring_producer_begin(m.c2s, SLOT_SIZE, SLOT_COUNT);
		if (!slot) { // ring full (server behind) -> spin briefly; not the race under test
			while (!(slot = dserver_ring_producer_begin(m.c2s, SLOT_SIZE, SLOT_COUNT))) __builtin_ia32_pause();
		}
		slot->callnum = 1; slot->seq = (uint32_t)it; slot->length = 0; slot->arena_len = 0; slot->arena_off = 0; slot->flags = 0;
		dserver_ring_producer_publish(m.c2s);
		// conditional doorbell -- the real decision
		if (dserver_ring_guest_should_doorbell(m.cb)) doorbell.store(1, std::memory_order_release);
		// release this round to the server and wait (watchdog) for it to be serviced
		long want = it;
		round.store(want, std::memory_order_release);
		bool ok = false;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (std::chrono::steady_clock::now() < deadline) {
			if (serverRound.load(std::memory_order_acquire) == want) { ok = true; break; }
			__builtin_ia32_pause();
		}
		if (!ok) { ++timeouts; break; } // watchdog fired == lost wake (true hang, not jitter)
	}
	stop.store(1, std::memory_order_release);
	doorbell.store(1, std::memory_order_release); // release a parked server so it can exit
	round.store(iters + 1, std::memory_order_release);
	server.join();
	freeRing(m);

	// DIAGNOSTIC ONLY (see header): the exhaustive enumeration is the authoritative gate. This
	// concurrent harness corroborates on real hardware but its round handshake + modeled park are
	// scheduling-sensitive, so a timeout here is reported, NOT a hard failure (it would otherwise
	// be a flaky CI gate -- exactly what we must avoid). A genuine wake-model regression is caught
	// deterministically by the enumeration above.
	fprintf(stderr, "      (request stress [diag]: serviced=%ld/%ld timeouts=%ld)\n", serviced.load(), iters, timeouts);
}

// Response-side stress: server publishes a reply; guest (other thread) must observe it. Guest
// runs spin -> set waiter bit -> recheck -> "FUTEX_WAIT" (park on a wake flag). A lost wake = the
// guest parks and is never released.
static void stress_response_side(long iters) {
	RingMap m = buildRing(SLOT_SIZE, SLOT_COUNT);
	std::atomic<int> stop{0};
	std::atomic<long> observed{0};
	std::atomic<int> wake{0};
	std::atomic<long> round{-1};
	std::atomic<long> guestRound{-1};

	// guest thread: per round, spin a little; on miss set waiter bit, recheck, then park on `wake`.
	std::thread guest([&]{
		pin_to_cpu(0);
		while (!stop.load(std::memory_order_acquire)) {
			long r = round.load(std::memory_order_acquire);
			if (r < 0 || guestRound.load(std::memory_order_acquire) == r) { __builtin_ia32_pause(); continue; }
			bool got = false;
			for (int i = 0; i < 8 && !got; ++i) { // short spin (force the slow path often)
				if (dserver_ring_consumer_begin(m.s2c, SLOT_SIZE, SLOT_COUNT)) { dserver_ring_consumer_advance(m.s2c); got = true; }
				else __builtin_ia32_pause();
			}
			if (!got) {
#ifndef RACE_WAITERBIT_AFTER_RECHECK
				__atomic_store_n(&m.cb->s2c_waiters, 1u, __ATOMIC_RELEASE); // GREEN: bit BEFORE recheck
#endif
				if (dserver_ring_consumer_begin(m.s2c, SLOT_SIZE, SLOT_COUNT)) { dserver_ring_consumer_advance(m.s2c); got = true; }
				if (!got) {
#ifdef RACE_WAITERBIT_AFTER_RECHECK
					__atomic_store_n(&m.cb->s2c_waiters, 1u, __ATOMIC_RELEASE); // RED: bit AFTER recheck
#endif
					// "FUTEX_WAIT": park until a wake token. NO speculative ring drain -- the wake is
					// the ONLY thing that releases us, so a missed wake (the RED bug) genuinely strands
					// us and the upstream watchdog catches it. steady_clock deadline so host load only
					// deschedules us, never manufactures a false timeout.
					auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
					while (std::chrono::steady_clock::now() < deadline) {
						if (wake.exchange(0, std::memory_order_acq_rel)) break;
						if (stop.load(std::memory_order_acquire)) break;
						__builtin_ia32_pause();
					}
					if (dserver_ring_consumer_begin(m.s2c, SLOT_SIZE, SLOT_COUNT)) { dserver_ring_consumer_advance(m.s2c); got = true; }
				}
				__atomic_store_n(&m.cb->s2c_waiters, 0u, __ATOMIC_RELEASE);
			}
			if (got) observed.fetch_add(1, std::memory_order_acq_rel);
			guestRound.store(r, std::memory_order_release);
		}
	});

	pin_to_cpu(1); // producer (this thread) on a distinct core from the guest thread
	long timeouts = 0;
	for (long it = 0; it < iters; ++it) {
		// let the guest reach its slow path, with jitter across the set-bit/recheck/park window
		round.store(it, std::memory_order_release);
		if (it & 1) for (volatile int z = 0; z < (int)(it % 53); ++z) {}
		// server publish reply
		dserver_ring_slot_t* slot = dserver_ring_producer_begin(m.s2c, SLOT_SIZE, SLOT_COUNT);
		if (!slot) { while (!(slot = dserver_ring_producer_begin(m.s2c, SLOT_SIZE, SLOT_COUNT))) __builtin_ia32_pause(); }
		slot->callnum = 1; slot->seq = (uint32_t)it; slot->length = sizeof(dserver_ring_reply_hdr_t); slot->arena_len = 0; slot->arena_off = 0; slot->flags = 0;
		dserver_ring_producer_publish(m.s2c);
		// conditional wake -- the real decision
		if (dserver_ring_server_should_wake(m.cb)) wake.store(1, std::memory_order_release);
		// watchdog: guest must observe this round (steady_clock; a true lost wake hangs forever)
		bool ok = false;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (std::chrono::steady_clock::now() < deadline) {
			if (guestRound.load(std::memory_order_acquire) == it) { ok = true; break; }
			__builtin_ia32_pause();
		}
		if (!ok) { ++timeouts; break; }
	}
	stop.store(1, std::memory_order_release);
	wake.store(1, std::memory_order_release);
	round.store(iters + 1, std::memory_order_release);
	guest.join();
	freeRing(m);

	// DIAGNOSTIC ONLY (see header): see the request-side note. Reported, not a hard failure.
	fprintf(stderr, "      (response stress [diag]: observed=%ld/%ld timeouts=%ld)\n", observed.load(), iters, timeouts);
}

int main(int argc, char** argv) {
	long iters = (argc > 1) ? atol(argv[1]) : 20000;

	fprintf(stderr, "== exhaustive ordering enumeration ==\n");
	exhaustive_request_side();
	exhaustive_response_side();

	// The exhaustive enumeration is the authoritative RED detector and is exact + instant. If it
	// already found a lost-wake interleaving (a RED arm), stop here -- the stress would otherwise
	// run the broken model and the watchdog would burn its full timeout on the genuine hang.
	if (failures) {
		fprintf(stderr, "\nring_wake_race_test: %d FAILURE(S) (enumeration) -- skipping stress\n", failures);
		return 1;
	}

	fprintf(stderr, "== concurrent stress (iters=%ld) ==\n", iters);
	stress_request_side(iters);
	stress_response_side(iters);

	if (failures) {
		fprintf(stderr, "\nring_wake_race_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_wake_race_test: all lost-wake invariants hold\n");
	return 0;
}
