// perf #18 D16 (dar-1il.11): RED->GREEN gate on the PER-THREAD lane table's active-bit/generation
// bookkeeping -- the dar-my8 "exhaustive interleaving over shared-state predicates is the gate"
// lesson applied to the multi-lane case.
//
// CONTEXT. D15a proved the reclaimable UDS pool is ~2053 POST-attach eligible calls scattered to UDS
// because the guest ring is SINGLE-OWNER (one TID owns the one process ring; every other thread sees a
// foreign owner and falls back to UDS). D16 gives each guest thread its OWN SPSC ring lane via a
// fixed-max per-process lane table (indexed by hashed gettid -- NOT __thread, which aborts this early
// in libsystem_kernel; see rpc-sleep-account.h). The hazard a multi-lane table introduces is the
// active-bit / generation bookkeeping: a lane freed by a dying/forking thread and reused by a new
// thread must never let a reader act on a half-initialized lane or match a stale epoch.
//
// THE MODEL. A lane has {active, generation, owner_tid} plus a payload (the ring mapping fields, modeled
// here as a single `payload` word that the "reader" reads and asserts is the value the owner published).
// Three actors share the table:
//   ACQUIRE (a guest thread taking a free lane): claim a free slot, INITIALIZE the payload + owner +
//     bump generation, and ONLY THEN publish active=1 (release). The publish-active-LAST ordering is the
//     load-bearing invariant: a reader that observes active=1 must see a fully-initialized lane.
//   READER (a peer guest thread looking its lane up by tid, OR -- the sharper case -- the same slot's
//     NEXT owner verifying it cached the right epoch): on observing active=1 for its tid, it reads the
//     payload+generation; it must read the CURRENT owner's payload and the CURRENT generation, never a
//     torn/stale one.
//   REUSE (a dying thread releases its lane, a new thread reclaims the same slot): release clears active
//     BEFORE the slot can be reclaimed; reclaim bumps the generation so a stale cached epoch never
//     matches. A reuse WITHOUT a generation bump lets a reader that cached the old epoch match the new
//     owner's lane (the dar-my8 stale-epoch class).
//
// We drive it EXHAUSTIVELY: a deterministic single-stepped driver enumerates EVERY relative ordering of
// the race-window micro-ops of {acquire of lane i by thread A, a reader R observing the slot, release by
// A, reclaim of slot i by thread B} and asserts the two invariants for every interleaving. This is the
// pass/fail contract (exact, instant, deterministic). Concurrent stress is corroboration, NOT the gate.
//
// GREEN (default): the real predicates (publish-active-after-init; clear-active-before-reuse; gen bump
// on reclaim). Exhaustive enumeration finds ZERO violations.
// RED arm -DRACE_PUBLISH_ACTIVE_BEFORE_INIT: acquire sets active=1 BEFORE writing the payload/gen -> a
//   reader can observe active=1 + read a stale/garbage payload. The enumeration MUST find >0 violations.
// RED arm -DRACE_REUSE_WITHOUT_GEN_BUMP: reclaim reuses the slot WITHOUT bumping the generation -> a
//   reader holding the previous owner's cached epoch matches the new owner's lane. MUST find >0.

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// ---- the modeled lane table -----------------------------------------------------------------------
// Kept deliberately tiny (2 slots is enough to exercise free-slot search + reuse) so the exhaustive
// enumeration is cheap. The real guest table is GR_MAX_LANES wide but the bookkeeping predicate is
// per-slot, so 2 slots fully covers the active-bit/gen logic.
#define MODEL_LANES 2

typedef struct {
	uint32_t active;      // 0 = free, 1 = live  (published LAST on acquire, cleared FIRST on release)
	uint32_t generation;  // bumped on every (re)claim so a stale cached epoch never matches
	int32_t  owner_tid;
	uint32_t payload;     // stands in for the lane's ring mapping fields; reader checks it matches owner
} lane_t;

typedef struct {
	lane_t lanes[MODEL_LANES];
} table_t;

// A reader's cached view of "its" lane, captured when it last used the lane. The reuse invariant says:
// if the reader re-validates (active==1 && owner_tid==tid && generation==cached_gen) it must be looking
// at the SAME epoch it cached -- a reclaim must have bumped the generation so this can't false-match.
typedef struct {
	int      valid;
	int      slot;
	int32_t  tid;
	uint32_t cached_gen;       // the GENERATION the reader observed (what the guest code compares)
	uint32_t cached_payload;
	uint64_t cached_epoch_id;  // the model's ground-truth epoch identity (a separate monotonic counter
	                           // the generation is SUPPOSED to track; lets the model detect a real reuse
	                           // even when a buggy reclaim collides the generation number)
} reader_view_t;

// ---- acquire: claim slot `slot` for `tid` with `payload`, as a sequence of ordered micro-steps ----
// Returns the published generation. The ORDER of the writes is the thing under test, so acquire is
// expressed as discrete steps the driver can interleave a reader between.
//
// Real order (GREEN): write owner -> write payload -> bump generation -> publish active=1.
// RED RACE_PUBLISH_ACTIVE_BEFORE_INIT: publish active=1 FIRST, then owner/payload/gen.

// Step kinds for the acquire state machine.
enum { ACQ_STEP_0, ACQ_STEP_1, ACQ_STEP_2, ACQ_STEP_3, ACQ_DONE };

static void acquire_step(table_t* t, int slot, int32_t tid, uint32_t payload, uint32_t new_gen, int step) {
	lane_t* L = &t->lanes[slot];
#ifdef RACE_PUBLISH_ACTIVE_BEFORE_INIT
	// BUGGY order: active published before the lane is initialized.
	switch (step) {
		case ACQ_STEP_0: L->active = 1; break;            // <-- premature publish
		case ACQ_STEP_1: L->owner_tid = tid; break;
		case ACQ_STEP_2: L->payload = payload; break;
		case ACQ_STEP_3: L->generation = new_gen; break;
	}
#else
	// CORRECT order: initialize fully, publish active LAST.
	switch (step) {
		case ACQ_STEP_0: L->owner_tid = tid; break;
		case ACQ_STEP_1: L->payload = payload; break;
		case ACQ_STEP_2: L->generation = new_gen; break;
		case ACQ_STEP_3: L->active = 1; break;            // <-- publish last (release semantics)
	}
#endif
}

// ---- reader: observe a slot for `tid`; if it looks live for us, validate the payload + epoch --------
// A reader caches {slot, tid, cached_gen, cached_payload} when it last used a lane and later re-validates
// it via the trio (active==1 && owner_tid==tid && generation==cached_gen). The model's truth oracle is
// `truth_*`: who/what the slot REALLY holds right now. A violation is when the reader's accept/skip
// decision DISAGREES with the truth -- i.e. it accepts a lane that is not the exact epoch it cached, or
// reads a payload that isn't the live owner's. `truth_initialized` is 0 while an acquire is mid-flight
// (payload/gen/owner not all written yet) so we can catch a premature-publish read.
static int reader_observe(const table_t* t, int slot, int32_t reader_tid, reader_view_t cached,
                          int32_t truth_owner, uint32_t truth_gen, uint32_t truth_payload,
                          int truth_initialized, uint64_t truth_epoch_id) {
	const lane_t* L = &t->lanes[slot];
	// acquire-load the active flag; if not live, the reader skips (nothing to validate).
	if (L->active != 1) return 0;
	if (L->owner_tid != reader_tid) return 0; // not our lane -> skip (correct)

	// The reader has decided this is its live lane and will USE it. Two things must hold for that to be
	// safe:
	// INVARIANT 1 (init-before-publish): if active is visible, the lane must be fully initialized and the
	// payload the reader reads must be the live owner's payload. A premature active publish lets the
	// reader observe active==1 over an un/partially-written lane.
	if (!truth_initialized) {
		return 1; // VIOLATION: accepted a lane whose initialization is not yet complete
	}
	if (L->payload != truth_payload || truth_owner != reader_tid) {
		return 1; // VIOLATION: payload/owner the reader will use is not the live owner's
	}
	// INVARIANT 2 (gen-disambiguates-reuse): the reader re-validates by matching its CACHED epoch. If the
	// match (active && owner==tid && gen==cached_gen) succeeds, the lane MUST actually be the epoch the
	// reader cached. If the slot was reused (truth_gen advanced past cached.cached_gen) but the live gen
	// still equals the cached gen, the reader false-accepts a foreign epoch.
	(void)truth_gen;
	if (cached.valid && cached.slot == slot && cached.tid == reader_tid) {
		// What the guest ACTUALLY does: accept iff the live generation equals the cached generation.
		int reader_accepts = (L->generation == cached.cached_gen);
		// Ground truth: the live lane is the reader's cached epoch iff their epoch IDENTITIES match.
		int really_same_epoch = (truth_epoch_id == cached.cached_epoch_id);
		if (reader_accepts && !really_same_epoch) {
			return 2; // VIOLATION: the gen comparison accepted a DIFFERENT epoch (reuse without gen bump)
		}
	}
	return 0;
}

// ---- release: clear active FIRST so the slot cannot be reclaimed while still advertised --------------
static void release_lane(table_t* t, int slot) {
	t->lanes[slot].active = 0; // clear active (release): the slot is now free for reclaim
}

// ---- reclaim: a new owner takes a freed slot. GREEN bumps the generation; RED reuses it verbatim. ----
static uint32_t reclaim_gen(const table_t* t, int slot) {
#ifdef RACE_REUSE_WITHOUT_GEN_BUMP
	(void)t; (void)slot;
	// BUGGY: reuse the slot's generation unchanged -> a reader holding the prior epoch false-matches.
	// We model "unchanged" by returning the slot's current generation.
	return t->lanes[slot].generation;
#else
	return t->lanes[slot].generation + 1; // bump so the new epoch is distinct from every prior one
#endif
}

static int g_violations = 0;
#define NOTE_VIOLATION(kind) do { ++g_violations; } while (0)

// ---- Scenario 1: ACQUIRE/READER interleaving (init-before-publish) ----------------------------------
// Thread A acquires slot 0 (4 ordered steps). A reader R for A's tid observes the slot at EVERY point
// between A's steps. INVARIANT: R either sees active!=1 (skips, correct) or sees active==1 AND the full
// payload (never active==1 with a stale payload). Enumerate the reader's observation point across all 5
// positions (before step0 .. after step3).
static void enumerate_acquire_reader(void) {
	const int32_t TID_A = 0x1111;
	const uint32_t PAYLOAD_A = 0xA0A0A0A0u;
	reader_view_t no_cache; memset(&no_cache, 0, sizeof(no_cache)); // reader has no prior epoch here
	for (int obs_at = 0; obs_at <= 4; ++obs_at) {
		table_t t;
		memset(&t, 0, sizeof(t)); // fresh table: all free, gen 0, payload 0 (the "garbage" pre-init value)
		uint32_t new_gen = t.lanes[0].generation + 1;
		// truth: the lane is only fully INITIALIZED after all 4 acquire steps have run.
		for (int step = 0; step < 4; ++step) {
			if (obs_at == step) {
				int v = reader_observe(&t, 0, TID_A, no_cache,
				                       /*truth_owner*/TID_A, /*truth_gen*/new_gen, /*truth_payload*/PAYLOAD_A,
				                       /*truth_initialized*/0, /*truth_epoch_id*/1); // mid-acquire: NOT done
				if (v) NOTE_VIOLATION(v);
			}
			acquire_step(&t, 0, TID_A, PAYLOAD_A, new_gen, step);
		}
		if (obs_at == 4) {
			int v = reader_observe(&t, 0, TID_A, no_cache, TID_A, new_gen, PAYLOAD_A, /*truth_initialized*/1, 1);
			if (v) NOTE_VIOLATION(v); // after full acquire the reader MUST see the lane cleanly
		}
	}
}

// ---- Scenario 2: RELEASE/REUSE interleaving (gen-disambiguates-reuse) -------------------------------
// Slot 0 starts owned by thread A (epoch gen_A). A releases it; thread B reclaims the SAME slot for a
// different tid + payload (epoch gen_B). A STALE reader still holding A's cached epoch (gen_A) probes the
// slot for A's tid -- it must NOT match B's lane (different owner_tid skips), and a reader probing for
// B's tid with A's stale cached_gen must not false-match. Enumerate the stale reader's probe at every
// point across {release, B-acquire steps}.
static void enumerate_release_reuse(void) {
	const int32_t TID_A = 0x1111, TID_B = 0x2222;
	const uint32_t PAYLOAD_A = 0xA0A0A0A0u;

	// Build a table where slot 0 is fully owned by A at some epoch gen_A.
	table_t base;
	memset(&base, 0, sizeof(base));
	{
		uint32_t gen_A = base.lanes[0].generation + 1;
		for (int s = 0; s < 4; ++s) acquire_step(&base, 0, TID_A, PAYLOAD_A, gen_A, s);
	}
	uint32_t gen_A = base.lanes[0].generation;

	// The KEY reuse hazard is TID RECYCLING: a slot freed by tid X is reclaimed later by the SAME tid X
	// (the OS recycles tids; the hashed-tid lane index can land on the same slot). A reader caching X's
	// FIRST epoch must not false-accept X's SECOND epoch in that slot. So thread B here probes under
	// TID_A too (the recycled tid), carrying A's first-epoch cache; only the generation distinguishes the
	// two epochs. (We also keep a distinct-tid B probe to confirm the owner_tid check skips cleanly.)
	const uint64_t EPOCH_A = 1, EPOCH_R = 2; // the two distinct ground-truth epochs of slot 0
	reader_view_t cacheA; // a reader that cached A's FIRST epoch in slot 0
	cacheA.valid = 1; cacheA.slot = 0; cacheA.tid = TID_A; cacheA.cached_gen = gen_A;
	cacheA.cached_payload = PAYLOAD_A; cacheA.cached_epoch_id = EPOCH_A;

	// Sequence after A is established: [release][reclaim step0..3]. The stale reader probes between each.
	for (int obs_at = 0; obs_at <= 5; ++obs_at) {
		table_t t = base;
		uint32_t gen_R = 0;     // the reclaim epoch's generation, fixed once release has happened
		int reclaimed = 0;
		// truth tracker: who/what slot 0 really holds, its epoch identity, and whether it's mid-(re)acquire.
		int32_t  truth_owner = TID_A; uint32_t truth_gen = gen_A, truth_payload = PAYLOAD_A; int truth_init = 1;
		uint64_t truth_epoch = EPOCH_A;
		for (int pos = 0; pos <= 5; ++pos) {
			if (obs_at == pos) {
				// Stale reader holding A's first-epoch cache, probing for the RECYCLED tid TID_A. It must
				// accept ONLY when slot 0 truly still holds A's first epoch; once reclaimed (EPOCH_R) it
				// must NOT match. With the gen-bump bug the reclaim generation collides gen_A and the
				// reader false-matches a DIFFERENT epoch -> INVARIANT 2 violation.
				int v = reader_observe(&t, 0, TID_A, cacheA,
				                       truth_owner, truth_gen, truth_payload, truth_init, truth_epoch);
				if (v) NOTE_VIOLATION(v);
				// A reader for a DIFFERENT tid must always skip on owner mismatch (no false-match).
				reader_view_t cacheB; memset(&cacheB, 0, sizeof(cacheB));
				int v2 = reader_observe(&t, 0, TID_B, cacheB, truth_owner, truth_gen, truth_payload, truth_init, truth_epoch);
				if (v2) NOTE_VIOLATION(v2);
			}
			if (pos == 0) {
				release_lane(&t, 0);
				truth_init = 0; truth_owner = -1; truth_epoch = 0; // slot is free; no live owner/epoch
			} else if (pos >= 1 && pos <= 4) {
				int step = pos - 1;
				if (!reclaimed) { gen_R = reclaim_gen(&t, 0); reclaimed = 1; }
				acquire_step(&t, 0, TID_A, PAYLOAD_A, gen_R, step); // recycled tid TID_A reclaims the slot
				if (step == 3) { truth_init = 1; truth_owner = TID_A; truth_gen = gen_R; truth_payload = PAYLOAD_A; truth_epoch = EPOCH_R; }
			}
		}
	}
}

// ---- free-slot search: acquire must never hand two live threads the SAME slot -----------------------
// Models the allocator: with MODEL_LANES slots, claim for distinct tids must pick distinct free slots,
// and on exhaustion must return "no lane" (the guest then UDS-falls-back). This pins the active-bit as
// the free/used marker the search reads.
static int claim_free_slot(const table_t* t) {
	for (int i = 0; i < MODEL_LANES; ++i) {
		if (t->lanes[i].active == 0) return i;
	}
	return -1; // exhausted -> caller UDS-falls-back
}

static void enumerate_allocator(void) {
	table_t t;
	memset(&t, 0, sizeof(t));
	int32_t tids[MODEL_LANES + 1] = { 0x10, 0x20, 0x30 };
	int claimed[MODEL_LANES + 1];
	for (int k = 0; k < MODEL_LANES + 1; ++k) {
		int slot = claim_free_slot(&t);
		claimed[k] = slot;
		if (k < MODEL_LANES) {
			if (slot < 0) { NOTE_VIOLATION(3); continue; } // must find a free slot while capacity remains
			// fully acquire it so the slot is marked used for the next claim
			uint32_t g = t.lanes[slot].generation + 1;
			for (int s = 0; s < 4; ++s) acquire_step(&t, slot, tids[k], 0xC0DE0000u + (uint32_t)k, g, s);
		} else {
			// table full -> the (MODEL_LANES+1)th claim MUST be refused (UDS fallback), never a reuse of
			// a live slot.
			if (slot >= 0) NOTE_VIOLATION(4);
		}
	}
	// distinctness of the first MODEL_LANES claims
	for (int a = 0; a < MODEL_LANES; ++a)
		for (int b = a + 1; b < MODEL_LANES; ++b)
			if (claimed[a] == claimed[b]) NOTE_VIOLATION(5);
}

int main(void) {
	enumerate_acquire_reader();
	enumerate_release_reuse();
	enumerate_allocator();

#if defined(RACE_PUBLISH_ACTIVE_BEFORE_INIT) || defined(RACE_REUSE_WITHOUT_GEN_BUMP)
	if (g_violations > 0) {
		fprintf(stderr, "RED arm correctly found %d active-bit/generation violation(s)\n", g_violations);
		// A RED arm MUST find violations -> exit nonzero so the harness sees the gate is real.
		return 1;
	}
	fprintf(stderr, "RED arm found ZERO violations but MUST find some -- gate not exercising the predicate\n");
	return 0; // harness treats a passing RED arm as failure
#else
	if (g_violations > 0) {
		fprintf(stderr, "GREEN: FAILED -- %d active-bit/generation violation(s) under the real predicates\n", g_violations);
		return 1;
	}
	fprintf(stderr, "GREEN: per-thread lane active-bit/generation bookkeeping is race-free across all enumerated interleavings\n");
	return 0;
#endif
}
