// perf #18 P4 (dar-dar6x4-perf-5dq.33): RED->GREEN gate on the wake-model decision predicates.
//
// The whole P4 integration hinges on TWO pure decisions made from the shared ring control block:
//   * the guest doorbells (writes the wake eventfd) only when the server is NOT actively polling;
//   * the server FUTEX_WAKEs the guest only when a guest is actually parked (waiter-bit set).
// If either decision degrades to "always" we are back to v0 (a syscall on every hot-path call),
// which the .31 spike measured as ~330x slower. This test pins the predicates so a regression
// that reintroduces an unconditional wake is caught at build/test time, not in a latency A/B.
//
// GREEN (default): include the real dserver_ring_guest_should_doorbell / _server_should_wake from
// rpc-supplement.h and assert they are conditional.
// RED arm (-DWAKE_MODEL_OLD): replace them with the pre-P4 always-true behavior; the conditional
// assertions below MUST then fail, proving the test actually exercises the conditionality.

#define DSERVER_RING_TRANSPORT 1
#define DSERVER_RING_NO_ATTACH_CHECK 1
#include <darlingserver/rpc-supplement.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef WAKE_MODEL_OLD
// The v0 model: unconditional. These shadow the real predicates to prove RED.
static int old_guest_should_doorbell(const dserver_ring_shm_t* cb) { (void)cb; return 1; }
static int old_server_should_wake(const dserver_ring_shm_t* cb)    { (void)cb; return 1; }
#define GUEST_DOORBELL old_guest_should_doorbell
#define SERVER_WAKE    old_server_should_wake
#else
#define GUEST_DOORBELL dserver_ring_guest_should_doorbell
#define SERVER_WAKE    dserver_ring_server_should_wake
#endif

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
	else         { fprintf(stderr, "ok:   %s\n", (msg)); } \
} while (0)

int main(void) {
	dserver_ring_shm_t cb;
	memset(&cb, 0, sizeof(cb));

	// ABI sanity: the wake words must exist and sit on their own cache lines, distinct from the
	// futex words (false-sharing them would silently reintroduce contention).
	CHECK((char*)&cb.server_state - (char*)&cb.s2c_futex >= 64,  "server_state on its own cache line vs s2c_futex");
	CHECK((char*)&cb.s2c_waiters - (char*)&cb.server_state >= 64, "s2c_waiters on its own cache line vs server_state");
	CHECK(DSERVER_RING_ABI_VERSION == 5u, "ABI version is 5 (v4 duplex mailbox + v5 typed munmap payload, P8 D4)");

	// --- guest conditional doorbell ----------------------------------------------------------
	// Server actively polling -> the hot path -> guest must NOT doorbell.
	__atomic_store_n(&cb.server_state, DSERVER_RING_SRV_ACTIVE_POLLING, __ATOMIC_RELEASE);
	CHECK(GUEST_DOORBELL(&cb) == 0, "no doorbell while server is ACTIVE_POLLING");

	// Server armed-to-sleep or already in epoll -> guest MUST doorbell (else lost wakeup).
	__atomic_store_n(&cb.server_state, DSERVER_RING_SRV_SLEEP_ARMED, __ATOMIC_RELEASE);
	CHECK(GUEST_DOORBELL(&cb) != 0, "doorbell while server is SLEEP_ARMED (race window)");
	__atomic_store_n(&cb.server_state, DSERVER_RING_SRV_SLEEPING_EPOLL, __ATOMIC_RELEASE);
	CHECK(GUEST_DOORBELL(&cb) != 0, "doorbell while server is SLEEPING_EPOLL");

	// --- server conditional wake --------------------------------------------------------------
	// No guest parked -> the hot path -> server must NOT FUTEX_WAKE.
	__atomic_store_n(&cb.s2c_waiters, 0u, __ATOMIC_RELEASE);
	CHECK(SERVER_WAKE(&cb) == 0, "no FUTEX_WAKE while no guest is parked (s2c_waiters == 0)");

	// A guest parked in FUTEX_WAIT -> server MUST wake it.
	__atomic_store_n(&cb.s2c_waiters, 1u, __ATOMIC_RELEASE);
	CHECK(SERVER_WAKE(&cb) != 0, "FUTEX_WAKE when a guest is parked (s2c_waiters != 0)");

	if (failures) {
		fprintf(stderr, "\nring_wake_predicates_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_wake_predicates_test: all wake-model invariants hold\n");
	return 0;
}
