/**
 * perf #18 (dar-dar6x4-perf-5dq.30) adversarial regression test: the shared-memory ring
 * control-block trust-boundary validator.
 *
 * Why this exists: the ring transport puts a guest-writable shared mapping in the RPC path.
 * The guest is UNTRUSTED -- it controls every field of the control block and can mutate the
 * page at any time. dserver_ring_shm_validate() is the gate the server runs (on a COPY of
 * the control block) before it maps/uses the ring; if it is wrong, a malicious guest makes
 * the server index out of bounds, deref a bogus offset, or map an absurd region. So the
 * validator MUST reject every malformed control block and accept only a well-formed one.
 *
 * RED->GREEN intent (standing TDD rule): with the real validator this is GREEN (exit 0).
 * It was proven RED first by compiling against an accept-all stub validator
 * (-DRING_VALIDATE_STUB) -- every adversarial case below then FAILs because the stub
 * returns dserver_ring_ok. The committed build uses the real header validator.
 *
 * Hermetic: header-only, no link deps, no boot, no sockets. Exit 0 = GREEN.
 */

#define DSERVER_RING_TRANSPORT 1
#include <darlingserver/rpc.h> // callnums for the C2S opcode-set hash (dar-1il.2 item 2)
#include <darlingserver/rpc-supplement.h>

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

#ifdef RING_VALIDATE_STUB
// RED arm: an accept-all validator. Proves the test is real -- every reject case below
// FAILs against this, so a GREEN run genuinely exercises the validator's logic.
static inline dserver_ring_reject_t stub_validate(const dserver_ring_shm_t*, uint64_t, int32_t) {
	return dserver_ring_ok;
}
#define VALIDATE stub_validate
#else
#define VALIDATE dserver_ring_shm_validate
#endif

static const int32_t NSID = 4242;

// Build a control block that is well-formed for a `count`x`size` pair, laid out as:
//   [ header | c2s ring | s2c ring | arena ] all packed, total = mapping size.
static dserver_ring_shm_t makeValid(uint32_t slot_size, uint32_t slot_count, uint32_t arena_size) {
	dserver_ring_shm_t cb;
	std::memset(&cb, 0, sizeof(cb));
	cb.magic = DSERVER_RING_MAGIC;
	cb.abi_version = DSERVER_RING_ABI_VERSION;
	cb.slot_size = (uint16_t)slot_size;
	cb.slot_count = slot_count;
	cb.guest_tid = NSID;
	cb.c2s_opcode_hash = dserver_ring_c2s_opcode_hash(); // dar-1il.2 item 2: matches the server's by default

	uint32_t hdr = (uint32_t)sizeof(dserver_ring_shm_t);
	uint32_t ring_span = (uint32_t)(sizeof(dserver_ring_t) + (uint64_t)slot_count * slot_size);
	cb.c2s_ring_off = hdr;
	cb.s2c_ring_off = hdr + ring_span;
	if (arena_size) {
		cb.arena_off = hdr + 2 * ring_span;
		cb.arena_size = arena_size;
		cb.total_size = hdr + 2 * ring_span + arena_size;
	} else {
		cb.arena_off = 0;
		cb.arena_size = 0;
		cb.total_size = hdr + 2 * ring_span;
	}
	return cb;
}

int main() {
	// --- accept: a well-formed control block (with and without an arena) ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_ok, "well-formed (no arena) accepted");
	}
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 4096);
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_ok, "well-formed (with arena) accepted");
	}

	// --- reject: bad magic ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.magic = 0xdeadbeef;
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_magic, "bad magic rejected");
	}
	// --- reject: wrong ABI version ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.abi_version = DSERVER_RING_ABI_VERSION + 1;
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_abi, "bad abi rejected");
	}
	// --- reject: C2S opcode-set hash mismatch (dar-1il.2 item 2) ---
	// A guest built from a DIFFERENT DSERVER_RING_C2S_OPCODES publishes a hash that doesn't match the
	// server's -> the ring is rejected (-> the thread uses UDS for everything, never a silent per-op
	// drop). Flip one bit of the matching hash to model the skew.
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.c2s_opcode_hash ^= 0x1ull;
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_opcode_set, "C2S opcode-set hash mismatch rejected");
	}
	// --- accept: the matching hash is accepted (guards the check isn't vacuously always-reject) ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.c2s_opcode_hash = dserver_ring_c2s_opcode_hash();
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_ok, "matching C2S opcode-set hash accepted");
	}
	// --- reject: slot_size out of range / below the slot header ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.slot_size = DSERVER_RING_MAX_SLOT_SIZE + 1; // also clamps via uint16, but >MAX still
		// recompute a consistent layout would still be out of range; just check the field
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_slot_size, "huge slot_size rejected");
	}
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.slot_size = 8; // < sizeof(dserver_ring_slot_t) and < MIN
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_slot_size, "tiny slot_size rejected");
	}
	// --- reject: slot_count not a power of two ---
	{
		dserver_ring_shm_t cb = makeValid(256, 48 /* not pow2 */, 0);
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_slot_count, "non-pow2 slot_count rejected");
	}
	// --- reject: slot_count over the cap ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.slot_count = DSERVER_RING_MAX_SLOT_COUNT * 2;
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_slot_count, "oversized slot_count rejected");
	}
	// --- reject: total_size != real mapping size (the fd is a different size) ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		CHECK(VALIDATE(&cb, cb.total_size - 1, NSID) == dserver_ring_reject_total_size, "size mismatch (fd smaller) rejected");
		CHECK(VALIDATE(&cb, cb.total_size + 4096, NSID) == dserver_ring_reject_total_size, "size mismatch (fd bigger) rejected");
	}
	// --- reject: total_size over the hard cap ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.total_size = DSERVER_RING_MAX_TOTAL_SIZE + 1;
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_total_size, "over-cap total_size rejected");
	}
	// --- reject: a ring whose span escapes the mapping ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.s2c_ring_off = cb.total_size - 16; // ring header alone already runs past the end
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_ring_bounds, "ring out of bounds rejected");
	}
	// --- reject: a ring offset inside the control-block header ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.c2s_ring_off = 0; // overlaps the header
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_ring_bounds, "ring inside header rejected");
	}
	// --- reject: the two rings overlap each other ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		cb.s2c_ring_off = cb.c2s_ring_off; // both rings at the same place
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_ring_overlap, "overlapping rings rejected");
	}
	// --- reject: arena escapes the mapping ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 4096);
		cb.arena_size = cb.total_size; // arena claims more than the whole mapping
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_arena_bounds, "arena out of bounds rejected");
	}
	// --- reject: arena overlaps a ring ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 4096);
		cb.arena_off = cb.c2s_ring_off; // arena sits on top of the c2s ring
		CHECK(VALIDATE(&cb, cb.total_size, NSID) == dserver_ring_reject_arena_overlap, "arena over ring rejected");
	}
	// --- reject: guest-claimed tid != the SCM-credentialed nsid (spoofed identity) ---
	{
		dserver_ring_shm_t cb = makeValid(256, 64, 0);
		CHECK(VALIDATE(&cb, cb.total_size, NSID + 1) == dserver_ring_reject_tid, "tid spoof rejected");
	}

	if (failures == 0) {
		std::printf("ring_shm_validate_test: all checks passed\n");
		return 0;
	}
	std::fprintf(stderr, "ring_shm_validate_test: %d failure(s)\n", failures);
	return 1;
}
