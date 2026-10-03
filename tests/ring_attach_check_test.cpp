/**
 * perf #18 (dar-dar6x4-perf-5dq.30) adversarial regression test: the server-side ring_attach
 * decision, dserver_ring_attach_check(), driven with REAL memfds.
 *
 * Where ring_shm_validate_test exercises the pure arithmetic validator, this test exercises
 * the bridge to real kernel state: the server fstats the guest-passed memfd for its TRUE
 * size, maps it, copies the control block out, and validates -- so the attack surface here is
 * a memfd whose real size disagrees with the guest's claim, or a memfd that is genuinely too
 * small to hold a control block, or a perfectly-sized memfd carrying a malformed block.
 *
 * RED->GREEN intent (standing TDD rule): GREEN against the real attach-check; proven RED
 * against an accept-all stub (-DRING_ATTACH_STUB) where every adversarial case FAILs.
 *
 * Hermetic: header-only + memfd_create, no boot/sockets/server. Exit 0 = GREEN.
 */

#define DSERVER_RING_TRANSPORT 1
#include <darlingserver/rpc.h> // callnums for the C2S opcode-set hash (dar-1il.2 item 2)
#include <darlingserver/rpc-supplement.h>

#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
		++failures; \
	} \
} while (0)

static const int32_t NSID = 7777;

#ifdef RING_ATTACH_STUB
static inline dserver_ring_reject_t stub_attach(int, uint64_t, int32_t, uint64_t*, dserver_ring_shm_t*) {
	return dserver_ring_ok; // accept-all -> every reject case below must FAIL
}
#define ATTACH stub_attach
#else
#define ATTACH dserver_ring_attach_check
#endif

// Build a well-formed control block for slot_size x slot_count (+ optional arena),
// laid out packed: [header | c2s | s2c | arena]. Returns the total size.
static uint32_t fillValid(dserver_ring_shm_t* cb, uint32_t slot_size, uint32_t slot_count, uint32_t arena_size) {
	std::memset(cb, 0, sizeof(*cb));
	cb->magic = DSERVER_RING_MAGIC;
	cb->abi_version = DSERVER_RING_ABI_VERSION;
	cb->slot_size = (uint16_t)slot_size;
	cb->slot_count = slot_count;
	cb->guest_tid = NSID;
	cb->c2s_opcode_hash = dserver_ring_c2s_opcode_hash(); // dar-1il.2 item 2: matches the server's by default
	uint32_t hdr = (uint32_t)sizeof(dserver_ring_shm_t);
	uint32_t ring_span = (uint32_t)(sizeof(dserver_ring_t) + (uint64_t)slot_count * slot_size);
	cb->c2s_ring_off = hdr;
	cb->s2c_ring_off = hdr + ring_span;
	if (arena_size) {
		cb->arena_off = hdr + 2 * ring_span;
		cb->arena_size = arena_size;
		cb->total_size = hdr + 2 * ring_span + arena_size;
	} else {
		cb->total_size = hdr + 2 * ring_span;
	}
	return cb->total_size;
}

// Create a memfd of exactly `size` bytes, with `cb` written at offset 0.
static int makeMemfd(const dserver_ring_shm_t* cb, uint64_t size) {
	int fd = memfd_create("ringtest", 0);
	if (fd < 0) { perror("memfd_create"); std::exit(2); }
	if (ftruncate(fd, (off_t)size) != 0) { perror("ftruncate"); std::exit(2); }
	void* m = mmap(NULL, sizeof(*cb), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { perror("mmap"); std::exit(2); }
	std::memcpy(m, cb, sizeof(*cb));
	munmap(m, sizeof(*cb));
	return fd;
}

int main() {
	uint64_t outSize = 0;
	dserver_ring_shm_t outCb;

	// --- accept: a well-formed, correctly-sized memfd (no arena, and with arena) ---
	{
		dserver_ring_shm_t cb; uint32_t sz = fillValid(&cb, 256, 64, 0);
		int fd = makeMemfd(&cb, sz);
		CHECK(ATTACH(fd, sz, NSID, &outSize, &outCb) == dserver_ring_ok, "well-formed memfd accepted");
		CHECK(outSize == sz, "accepted: real size reported");
		close(fd);
	}
	{
		dserver_ring_shm_t cb; uint32_t sz = fillValid(&cb, 256, 64, 8192);
		int fd = makeMemfd(&cb, sz);
		CHECK(ATTACH(fd, sz, NSID, &outSize, &outCb) == dserver_ring_ok, "well-formed memfd (arena) accepted");
		close(fd);
	}

	// --- reject: guest claims a size different from the fd's real size ---
	{
		dserver_ring_shm_t cb; uint32_t sz = fillValid(&cb, 256, 64, 0);
		int fd = makeMemfd(&cb, sz);
		CHECK(ATTACH(fd, sz - 64, NSID, &outSize, &outCb) == dserver_ring_reject_total_size, "claimed size < real rejected");
		CHECK(ATTACH(fd, sz + 64, NSID, &outSize, &outCb) == dserver_ring_reject_total_size, "claimed size > real rejected");
		close(fd);
	}

	// --- reject: control block says total_size != the fd's real size (lying header) ---
	{
		dserver_ring_shm_t cb; uint32_t sz = fillValid(&cb, 256, 64, 0);
		// make the fd one page BIGGER than the header claims; guest claims the big size honestly,
		// but the validator sees cb.total_size (sz) != real (sz + page).
		uint64_t real = sz + 4096;
		int fd = makeMemfd(&cb, real);
		CHECK(ATTACH(fd, real, NSID, &outSize, &outCb) == dserver_ring_reject_total_size, "header total_size != real fd size rejected");
		close(fd);
	}

	// --- reject: a memfd too small to even hold a control block ---
	{
		dserver_ring_shm_t cb; std::memset(&cb, 0, sizeof(cb));
		cb.magic = DSERVER_RING_MAGIC; cb.abi_version = DSERVER_RING_ABI_VERSION;
		int fd = memfd_create("tiny", 0);
		ftruncate(fd, 16); // smaller than sizeof(dserver_ring_shm_t)
		CHECK(ATTACH(fd, 16, NSID, &outSize, &outCb) == dserver_ring_reject_total_size, "undersized memfd rejected");
		close(fd);
	}

	// --- reject: correctly-sized memfd carrying a malformed control block (bad magic) ---
	{
		dserver_ring_shm_t cb; uint32_t sz = fillValid(&cb, 256, 64, 0);
		cb.magic = 0xbadc0de;
		int fd = makeMemfd(&cb, sz);
		CHECK(ATTACH(fd, sz, NSID, &outSize, &outCb) == dserver_ring_reject_magic, "malformed block in valid-size memfd rejected");
		close(fd);
	}

	// --- reject: tid spoof (block well-formed but guest_tid != the SCM nsid) ---
	{
		dserver_ring_shm_t cb; uint32_t sz = fillValid(&cb, 256, 64, 0);
		int fd = makeMemfd(&cb, sz);
		CHECK(ATTACH(fd, sz, NSID + 1, &outSize, &outCb) == dserver_ring_reject_tid, "tid spoof rejected at attach");
		close(fd);
	}

	// --- reject: C2S opcode-set hash mismatch (dar-1il.2 item 2) ---
	// A guest built from a different DSERVER_RING_C2S_OPCODES publishes a non-matching hash even
	// though everything else is well-formed -> the ring is rejected at attach (the thread then uses
	// UDS for everything; no silent per-op drop). Real-memfd path mirrors the validator-level test.
	{
		dserver_ring_shm_t cb; uint32_t sz = fillValid(&cb, 256, 64, 0);
		cb.c2s_opcode_hash ^= 0x1ull; // model a skewed opcode set
		int fd = makeMemfd(&cb, sz);
		CHECK(ATTACH(fd, sz, NSID, &outSize, &outCb) == dserver_ring_reject_opcode_set, "C2S opcode-set hash mismatch rejected at attach");
		close(fd);
	}

	// --- reject: a bad fd number (negative / closed) does not crash, returns reject ---
	{
		CHECK(ATTACH(-1, 4096, NSID, &outSize, &outCb) != dserver_ring_ok, "bad fd rejected without crash");
	}

	if (failures == 0) {
		std::printf("ring_attach_check_test: all checks passed\n");
		return 0;
	}
	std::fprintf(stderr, "ring_attach_check_test: %d failure(s)\n", failures);
	return 1;
}
