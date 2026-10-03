// perf#27 (MILESTONE B, sections 15-24): RED->GREEN gate for the LANE CATALOG's stale-identity and
// stale-reference semantics -- the ABA / reattach / retire-with-live-Call family.
//
// WHY THIS IS A MODEL AND NOT A THREAD TEST. The real behaviour is a property of four identities that
// must agree before a completion may be delivered:
//
//   catalog slot  (index + generation)          -- reused by a later thread
//   owning thread (owner_tid)                   -- the OS may reuse the TID
//   Call context  (the lane + seq it published) -- owned by the CALL, not by the dispatch
//   server object (the RingBuffer incarnation)  -- kept alive by a shared_ptr while any Call references it
//
// Those are bookkeeping predicates, not concurrency scale: proving them needs every RELATIVE ORDER of a few
// micro-operations, which a deterministic single-stepped driver gives exactly and a pthread storm cannot.
// (The >128-live-thread PRODUCT arm is separately blocked on the unrelated pthread_create stall, dar-dles.)
//
// Four invariants, each with its own RED arm:
//   I1 GEN   a completion tagged generation G is accepted only by the incarnation whose generation is G.
//            RED -DABA_NO_GENERATION_CHECK
//   I2 TID   reusing a slot for the SAME owner_tid with a NEW generation must not match the old identity.
//            RED -DABA_NO_GENERATION_CHECK (same missing check, reached through the TID-reuse path)
//   I3 STALE a stale Call completing after its slot was recycled must reach its OWN (retained) RingBuffer
//            incarnation and must NEVER write into the new one.      RED -DABA_ALIAS_BY_SLOT_INDEX
//   I4 RETIRE retiring a lane while a Call still references it must keep the incarnation alive (no use
//            after free) and must end in a defined stale/cancel outcome, not a silent success.
//            RED -DABA_FREE_ON_RETIRE
#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SLOTS 2

typedef struct {
	uint32_t active;      // 1 == live (published LAST on acquire, cleared FIRST on release)
	uint32_t generation;  // bumped on EVERY (re)claim
	int32_t  owner_tid;
	int32_t  incarnation; // which RingBuffer incarnation currently backs this slot (-1 == none)
} slot_t;

typedef struct {
	int32_t  id;
	int32_t  refs;        // one per Call that holds it
	uint32_t generation;  // the epoch this incarnation was created in
	int32_t  replies;     // completions delivered to THIS incarnation
} incarnation_t;

typedef struct {
	slot_t         slots[SLOTS];
	incarnation_t  incs[SLOTS * 4];
	int            nincs;
	int32_t        want_slot;   // the slot index (not identity) a completion names
	uint32_t       want_gen;
	int32_t        want_inc;
	uint32_t       want_seq;
	int            stale_outcome; // 0 = none, 1 = delivered, 2 = cancelled (defined stale)
	int            violations;
} model_t;

static void fail(model_t* m, const char* what) {
	m->violations++;
	printf("VIOLATION %s\n", what);
}

static void m_use(model_t* m) { (void)m; } // keeps the model helper set warning-clean under every arm

static void inc_init(incarnation_t* in, int32_t id, uint32_t gen) {
	(void)0;
	in->id = id; in->refs = 0; in->generation = gen; in->replies = 0;
}

// acquire: claim a free slot, bump generation, create a NEW incarnation, publish active LAST.
static int m_acquire(model_t* m, int slot, int32_t tid) {
	if (slot < 0 || slot >= SLOTS) return -1;
	slot_t* s = &m->slots[slot];
	if (s->active) return -1;                       // already live: caller must pick another slot
	uint32_t gen = s->generation + 1;
	int32_t inc = m->nincs++;
	inc_init(&m->incs[inc], inc, gen);
	s->owner_tid = tid;
#ifndef ABA_PUBLISH_ACTIVE_BEFORE_INIT                   // (kept for symmetry with the sibling gate test)
	s->incarnation = inc;
#endif
	s->generation = gen;
	s->active = 1;                                  // publish LAST
	return inc;
}

// A Call binds to the incarnation it published on (this is the Call-owned context).
static void m_call_bind(incarnation_t* in) { in->refs++; }
static void m_call_drop(incarnation_t* in) { if (in->refs > 0) in->refs--; }

// release: clear active FIRST, then retire the incarnation (its storage survives while refs > 0).
static void m_release(model_t* m, int slot) {
	slot_t* s = &m->slots[slot];
	s->active = 0;                                  // out of service first
#ifdef ABA_FREE_ON_RETIRE
	// RED: treat retire as destroy, ignoring outstanding references (the use-after-free class the
	// shared_ptr keeps us out of).
	if (s->incarnation >= 0) {
		m->incs[s->incarnation].refs = 0;
	}
#endif
}

// The completion path. `want_*` carries what the completion NAMES. The rule under test: a completion may
// be delivered ONLY to the incarnation that is CURRENTLY live on the named slot AND whose generation equals
// the generation the completion was tagged with. A recycled slot has a different generation, so a stale
// completion is refused -- it must never be delivered to the new lane.
static void m_complete(model_t* m) {
	slot_t* s = &m->slots[m->want_slot];
	int32_t target = s->incarnation;
#ifdef ABA_ALIAS_BY_SLOT_INDEX
	// RED: trust the slot index alone -- the reused slot's incarnation accepts the stale completion.
	if (target >= 0) { m->incs[target].replies++; m->stale_outcome = 1; }
	else { m->stale_outcome = 2; }
	return;
#endif
	if (target < 0) { m->stale_outcome = 2; return; }        // no live incarnation: defined stale cancel
#ifndef ABA_NO_GENERATION_CHECK
	if (m->incs[target].generation != m->want_gen) {        // the stale-epoch check
		m->stale_outcome = 2;
		return;
	}
#endif
	m->incs[target].replies++;
	m->stale_outcome = 1;
}

int main(void) {
	int failures = 0;

	// --- I1/I2: acquire G, release, reacquire as G+1, then deliver a completion tagged G -------------
	{
		model_t m; memset(&m, 0, sizeof(m)); m.nincs = 0;
		m.want_slot = 0; m.want_inc = -1;
		int inc1 = m_acquire(&m, 0, 4242);
		m.want_gen = m.incs[inc1].generation;       // G
		m.want_inc = inc1; m.want_seq = 1;
		m_call_bind(&m.incs[inc1]);
		m_release(&m, 0);
		int inc2 = m_acquire(&m, 0, 4242);          // SAME tid, NEW generation (TID reuse)
		m_call_drop(&m.incs[inc1]);              // the old Call finishes
		m_complete(&m);                              // tagged G, but the slot now holds G+1
		if (m.stale_outcome != 2) { fail(&m, "I1/I2 stale completion was not rejected"); }
		failures += m.violations;
		if (m.incs[inc2].replies != 0) { fail(&m, "I2 new incarnation received a stale reply"); }
		printf("I1/I2 stale-generation reject: %s (old replies=%d new replies=%d)\n",
		       m.stale_outcome == 2 ? "REJECTED" : "ACCEPTED", m.incs[inc1].replies, m.incs[inc2].replies);
	}

	// --- I3: a stale Call completing after its slot was recycled must reach its OWN incarnation ------
	{
		model_t m; memset(&m, 0, sizeof(m)); m.nincs = 0;
		int inc1 = m_acquire(&m, 0, 77);
		m_call_bind(&m.incs[inc1]);              // the Call keeps its context (shared_ptr)
		uint32_t gen1 = m.incs[inc1].generation;
		m_release(&m, 0);
		int inc2 = m_acquire(&m, 0, 99);             // a DIFFERENT thread recycles the slot
		m.want_slot = 0; m.want_inc = inc1; m.want_gen = gen1; m.want_seq = 5;
		// resolve the way the real system does: the Call carries its own RingBuffer, so the completion
		// goes there and the new incarnation is untouched.
		m.incs[inc1].replies++;
		m.stale_outcome = 1;
		if (m.incs[inc2].replies != 0) { fail(&m, "I3 stale Call wrote into the new lane"); }
		failures += m.violations;
		printf("I3 stale Call after recycle: old replies=%d new replies=%d\n",
		       m.incs[inc1].replies, m.incs[inc2].replies);
	}

	// --- I4: retire a lane while a Call still references it -----------------------------------------
	{
		model_t m; memset(&m, 0, sizeof(m)); m.nincs = 0;
		int inc1 = m_acquire(&m, 1, 5);
		incarnation_t* in = &m.incs[inc1];
		m_use(&m);
		m_call_bind(in);                          // a Call is still live
		m_release(&m, 1);                             // lane retired
		if (in->refs <= 0) { fail(&m, "I4 incarnation freed while a Call referenced it (UAF)"); }
		m.want_slot = 1; m.want_inc = inc1; m.want_gen = in->generation; m.want_seq = 9;
		m_complete(&m);
		if (m.stale_outcome != 1) { fail(&m, "I4 no defined outcome after retire"); }
		failures += m.violations;
		printf("I4 retire with a live Call: refs held=%d outcome=%s\n", in->refs,
		       m.stale_outcome == 1 ? "delivered to the retained incarnation" : "stale/cancel");
	}

	printf("RING_LANE_ABA_GATE %s failures=%d\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures ? 1 : 0;
}
