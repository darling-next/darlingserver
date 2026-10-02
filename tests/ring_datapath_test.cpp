// perf #18 (dar-dar6x4-perf-5dq.30) P3 RED->GREEN gate for the shared-memory ring DATAPATH:
// the SPSC enqueue/dequeue helpers + the server-side C2S service loop + the reply convention,
// all exercised in one process against a REAL memfd-backed ring (the same layout the guest
// builds and the server validates). This pins the exact contract the guest producer and the
// server consumer must satisfy, and -- critically -- proves the consumer is bounds-safe under
// adversarial head/tail/length corruption WITHOUT needing a deployed guest.
//
//   GREEN arm: the real dserver_ring_service_c2s() (+ the real SPSC helpers). A well-formed
//              request round-trips; every adversarial corruption is contained (no OOB, no
//              crash, no bogus reply); backpressure (full s2c) is handled as retry-not-loss.
//   RED   arm (-DRING_DATAPATH_STUB): an accept-all consumer that trusts the slot's claimed
//              length and indexes head/tail blindly. The adversarial cases then read/write out
//              of the slot array -> caught by ASan (the gate compiles the RED arm with ASan)
//              or by the wrong-reply assertions. The RED arm MUST fail.
//
// Build: g++ -std=c++17 -D_GNU_SOURCE -DDSERVER_RING_TRANSPORT=1 [-fsanitize=address]
//            [-DRING_DATAPATH_STUB] -I../include ring_datapath_test.cpp

#define DSERVER_RING_TRANSPORT 1

#include <darlingserver/rpc.h> // callnums for the C2S opcode-set hash (dar-1il.2 item 2)
#include <darlingserver/rpc-supplement.h>

// This gate is hermetic: it does NOT pull in the generated rpc.h (build-dir only). It only
// needs SOME callnum the servicer treats as ring-eligible vs not, so we use local constants.
// The real server keys eligibility off dserver_callnum_task_self_trap; the datapath logic
// under test (SPSC + service loop + reply convention) is callnum-agnostic.
#define TEST_CALLNUM_ELIGIBLE   42u  /* stands in for dserver_callnum_task_self_trap */
#define TEST_CALLNUM_INELIGIBLE 99u  /* stands in for any non-allowlisted call */

#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>

// ---- ring builder (mirrors what the guest will construct in its memfd) -------------------

struct RingMap {
	void* base = nullptr;
	uint64_t size = 0;
	dserver_ring_shm_t* cb = nullptr;
	dserver_ring_t* c2s = nullptr;
	dserver_ring_t* s2c = nullptr;
};

static RingMap buildRing(uint32_t slot_size, uint32_t slot_count) {
	RingMap m;
	uint64_t hdr = sizeof(dserver_ring_shm_t);
	// align ring regions to 64 bytes
	auto alignup = [](uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); };
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
	cb->arena_off = 0;
	cb->arena_size = 0;
	cb->c2s_ring_off = (uint32_t)c2s_off;
	cb->s2c_ring_off = (uint32_t)s2c_off;
	cb->total_size = (uint32_t)total;
	cb->guest_tid = 4242;

	m.base = base; m.size = total; m.cb = cb;
	m.c2s = (dserver_ring_t*)((char*)base + c2s_off);
	m.s2c = (dserver_ring_t*)((char*)base + s2c_off);
	return m;
}

// ---- the servicer: in the real server this runs task_self_trap on the microthread; here a
// fake servicer returns a fixed port for task_self_trap and refuses anything else. ----------
static int fakeServicer(void* ctx, uint32_t callnum,
                        const void* req_body, uint32_t req_len,
                        void* reply_body, uint32_t reply_cap, uint32_t* out_reply_len,
                        int32_t* out_code) {
	(void)ctx; (void)req_body; (void)req_len;
	if (callnum != TEST_CALLNUM_ELIGIBLE) {
		return 1; // refuse non-eligible callnums on the ring
	}
	uint32_t port = 0x103; // pretend task self port
	if (reply_cap < sizeof(port)) return 1;
	memcpy(reply_body, &port, sizeof(port));
	*out_reply_len = sizeof(port);
	*out_code = 0;
	return 0;
}

// ---- RED-arm accept-all stub: trusts the claimed length, indexes blindly, no bounds -------
#ifdef RING_DATAPATH_STUB
static uint32_t stub_service_c2s(dserver_ring_t* c2s, dserver_ring_t* s2c,
                                 uint32_t slot_size, uint32_t slot_count,
                                 dserver_ring_servicer_t servicer, void* ctx, int* out_woke) {
	if (out_woke) *out_woke = 0;
	uint32_t serviced = 0;
	// trust tail blindly; consume exactly one if tail != head
	uint32_t head = c2s->head, tail = c2s->tail;
	while (head != tail) {
		char* slots = (char*)c2s + sizeof(dserver_ring_t);
		dserver_ring_slot_t* req = (dserver_ring_slot_t*)(slots + (uint64_t)(head & (slot_count - 1)) * slot_size);
		// BUG: copy the claimed length with no clamp -> OOB read when length is huge
		char scratch[4096];
		uint32_t L = req->length; if (L > sizeof(scratch)) L = L; // intentionally do NOT clamp to slot
		memcpy(scratch, (char*)req + sizeof(dserver_ring_slot_t), L > sizeof(scratch) ? sizeof(scratch) : L);
		// produce a reply trusting everything
		char* sslots = (char*)s2c + sizeof(dserver_ring_t);
		dserver_ring_slot_t* rep = (dserver_ring_slot_t*)(sslots + (uint64_t)(s2c->tail & (slot_count - 1)) * slot_size);
		char* payload = (char*)rep + sizeof(dserver_ring_slot_t);
		dserver_ring_reply_hdr_t* rh = (dserver_ring_reply_hdr_t*)payload;
		uint32_t rlen = 0; int32_t code = 0;
		int h = servicer(ctx, req->callnum, (char*)req + sizeof(dserver_ring_slot_t), req->length,
		                 payload + sizeof(dserver_ring_reply_hdr_t), 4096, &rlen, &code);
		head++; c2s->head = head;
		if (h != 0) continue;
		rh->code = code; rep->callnum = req->callnum; rep->seq = req->seq;
		rep->length = sizeof(dserver_ring_reply_hdr_t) + rlen; rep->flags = 0;
		s2c->tail++;
		if (out_woke) *out_woke = 1;
		serviced++;
	}
	return serviced;
}
#define SERVICE stub_service_c2s
#else
#define SERVICE dserver_ring_service_c2s
#endif

// ---- guest-style producer: publish a task_self_trap request -------------------------------
static void guestPublishTaskSelfTrap(dserver_ring_t* c2s, uint32_t slot_size, uint32_t slot_count, uint32_t seq) {
	dserver_ring_slot_t* slot = dserver_ring_producer_begin(c2s, slot_size, slot_count);
	if (!slot) { fprintf(stderr, "producer_begin: ring full unexpectedly\n"); exit(2); }
	slot->callnum = TEST_CALLNUM_ELIGIBLE;
	slot->seq = seq;
	slot->length = 0; // no request body
	slot->arena_off = 0; slot->arena_len = 0; slot->flags = 0;
	dserver_ring_producer_publish(c2s);
}

// guest-style reply consume: read the matching reply, return port via out, code via *outCode
static bool guestConsumeReply(dserver_ring_t* s2c, uint32_t slot_size, uint32_t slot_count,
                              uint32_t expectSeq, uint32_t* outPort, int32_t* outCode) {
	dserver_ring_slot_t* slot = dserver_ring_consumer_begin(s2c, slot_size, slot_count);
	if (!slot) return false;
	bool ok = true;
	if (slot->seq != expectSeq) ok = false;
	char* payload = (char*)slot + sizeof(dserver_ring_slot_t);
	dserver_ring_reply_hdr_t* rh = (dserver_ring_reply_hdr_t*)payload;
	*outCode = rh->code;
	if (slot->length >= sizeof(dserver_ring_reply_hdr_t) + sizeof(uint32_t)) {
		memcpy(outPort, payload + sizeof(dserver_ring_reply_hdr_t), sizeof(uint32_t));
	} else {
		ok = false;
	}
	dserver_ring_consumer_advance(s2c);
	return ok;
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } } while (0)

int main(void) {
	const uint32_t SS = 128, SC = 8;

	// 1. happy path: one request round-trips with the right port.
	{
		RingMap m = buildRing(SS, SC);
		guestPublishTaskSelfTrap(m.c2s, SS, SC, 7);
		int woke = 0;
		uint32_t n = SERVICE(m.c2s, m.s2c, SS, SC, fakeServicer, nullptr, &woke);
		CHECK(n == 1, "happy: serviced exactly 1");
		CHECK(woke == 1, "happy: woke set");
		uint32_t port = 0; int32_t code = -1;
		bool got = guestConsumeReply(m.s2c, SS, SC, 7, &port, &code);
		CHECK(got, "happy: reply consumed + seq matched");
		CHECK(code == 0, "happy: reply code 0");
		CHECK(port == 0x103, "happy: reply carries the port");
		munmap(m.base, m.size);
	}

	// 2. several requests in a row (FIFO order, seq echoed).
	{
		RingMap m = buildRing(SS, SC);
		for (uint32_t i = 0; i < 4; ++i) guestPublishTaskSelfTrap(m.c2s, SS, SC, 100 + i);
		int woke = 0;
		uint32_t n = SERVICE(m.c2s, m.s2c, SS, SC, fakeServicer, nullptr, &woke);
		CHECK(n == 4, "batch: serviced all 4");
		for (uint32_t i = 0; i < 4; ++i) {
			uint32_t port = 0; int32_t code = -1;
			bool got = guestConsumeReply(m.s2c, SS, SC, 100 + i, &port, &code);
			CHECK(got, "batch: each reply in FIFO order with matching seq");
			CHECK(port == 0x103, "batch: each reply carries the port");
		}
		munmap(m.base, m.size);
	}

	// 3. ADVERSARIAL: a request slot claims length > slot capacity. The real consumer must
	//    DROP it (no OOB read, no reply). The stub copies the claimed length -> OOB.
	{
		RingMap m = buildRing(SS, SC);
		// hand-craft a malformed published slot
		dserver_ring_slot_t* slot = dserver_ring_producer_begin(m.c2s, SS, SC);
		slot->callnum = TEST_CALLNUM_ELIGIBLE;
		slot->seq = 9;
		slot->length = 0xFFFFFF00u; // absurd: far bigger than slot_size
		slot->arena_off = 0; slot->arena_len = 0; slot->flags = 0;
		dserver_ring_producer_publish(m.c2s);
		int woke = 0;
		uint32_t n = SERVICE(m.c2s, m.s2c, SS, SC, fakeServicer, nullptr, &woke);
		CHECK(n == 0, "adversarial-length: serviced 0 (dropped)");
		CHECK(woke == 0, "adversarial-length: no wake");
		// no reply must have been published
		dserver_ring_slot_t* rep = dserver_ring_consumer_begin(m.s2c, SS, SC);
		CHECK(rep == nullptr, "adversarial-length: no reply published");
		munmap(m.base, m.size);
	}

	// 4. ADVERSARIAL: corrupt c2s tail to a wild value (peer owns tail). Consumer must treat
	//    an impossible (tail-head) distance as empty and service nothing -- never index OOB.
	{
		RingMap m = buildRing(SS, SC);
		guestPublishTaskSelfTrap(m.c2s, SS, SC, 11); // one legit, tail now 1
		// corrupt: jump tail far ahead so (tail-head) > slot_count
		std::atomic_thread_fence(std::memory_order_seq_cst);
		((dserver_ring_t*)m.c2s)->tail = 0x40000000u;
		int woke = 0;
		uint32_t n = SERVICE(m.c2s, m.s2c, SS, SC, fakeServicer, nullptr, &woke);
		CHECK(n == 0, "adversarial-tail: corrupt tail -> serviced 0");
		dserver_ring_slot_t* rep = dserver_ring_consumer_begin(m.s2c, SS, SC);
		CHECK(rep == nullptr, "adversarial-tail: no reply published");
		munmap(m.base, m.size);
	}

	// 5. backpressure: fill s2c so a reply can't be published; the request must remain
	//    UNCONSUMED (retry, not loss). We simulate a full s2c by pre-advancing its tail to
	//    capacity with the consumer not draining.
	{
		RingMap m = buildRing(SS, SC);
		// fill s2c to capacity (slot_count) by bumping tail without anyone consuming head
		((dserver_ring_t*)m.s2c)->tail = SC; // head==0 -> used==SC == full
		guestPublishTaskSelfTrap(m.c2s, SS, SC, 21);
		int woke = 0;
		uint32_t n = SERVICE(m.c2s, m.s2c, SS, SC, fakeServicer, nullptr, &woke);
		CHECK(n == 0, "backpressure: serviced 0 when s2c full");
		// the request must still be pending (head unchanged) so a later wake retries it
		CHECK(((dserver_ring_t*)m.c2s)->head == 0, "backpressure: request left unconsumed (retry-not-loss)");
		munmap(m.base, m.size);
	}

	// 6. a non-eligible callnum is refused (no reply), so the guest UDS-falls-back.
	{
		RingMap m = buildRing(SS, SC);
		dserver_ring_slot_t* slot = dserver_ring_producer_begin(m.c2s, SS, SC);
		slot->callnum = TEST_CALLNUM_INELIGIBLE; // not on the P3 allowlist (fakeServicer refuses)
		slot->seq = 31; slot->length = 0; slot->arena_off = 0; slot->arena_len = 0; slot->flags = 0;
		dserver_ring_producer_publish(m.c2s);
		int woke = 0;
		uint32_t n = SERVICE(m.c2s, m.s2c, SS, SC, fakeServicer, nullptr, &woke);
		CHECK(n == 0, "refused-callnum: serviced 0 (servicer refused)");
		dserver_ring_slot_t* rep = dserver_ring_consumer_begin(m.s2c, SS, SC);
		CHECK(rep == nullptr, "refused-callnum: no reply published");
		// the request WAS consumed (we don't spin on it) so head advanced
		CHECK(((dserver_ring_t*)m.c2s)->head == 1, "refused-callnum: request consumed (no spin)");
		munmap(m.base, m.size);
	}

	if (failures == 0) {
		printf("ring datapath gate: all %s checks passed\n",
#ifdef RING_DATAPATH_STUB
		       "(STUB)"
#else
		       "(REAL)"
#endif
		);
		return 0;
	}
	fprintf(stderr, "ring datapath gate: %d FAILURES\n", failures);
	return 1;
}
