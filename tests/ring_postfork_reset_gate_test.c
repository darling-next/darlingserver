// perf #18 D16 follow-up (dar-j7e7): RED->GREEN gate on __dserver_ring_postfork_reset()'s
// RESOURCE-OWNERSHIP predicate -- "which lanes actually own a mapping/wake-fd that the fork child must
// release." This closes the coverage gap the D16 gate (ring_multilane_gate_test.c) left: that gate pinned
// the active-bit/generation bookkeeping under concurrent reader/reuse, but NOT what postfork_reset does
// per lane. The shipped D16 impl gated the per-lane teardown on `wake_fd >= 0`, which is WRONG because the
// lane table is `static` (zero-initialized): every UN-attached lane has wake_fd == 0, and 0 >= 0, so the
// child ran close(0) once per untouched lane -> stdin destroyed -> "cannot duplicate fd 0" in the next
// exec'd shell (deterministically reproduced; the brew failure of the integration/arch guest dylib).
//
// THE INVARIANT (architectural, not a value-shape workaround): a lane OWNS its resources (map + wake_fd)
// IFF acquire published it -- i.e. active == 1. postfork_reset must free resources for exactly the lanes
// that own them, keyed off the SAME publish marker acquire sets LAST (dserver-ring.c: active=1 published
// last on acquire; cleared first on release). Freeing off a resource VALUE (wake_fd >= 0) instead of the
// ownership MARKER is the bug: it conflates the zero-init "no fd" default (0) with the valid fd 0.
//
// THE MODEL. A tiny lane table, some lanes live (active==1, own a fake mapping + a wake_fd), the rest
// left EXACTLY as `static` zero-init leaves them (active==0, wake_fd==0 -- NOT -1). We run the real
// postfork_reset teardown decision over the table with close()/munmap() replaced by RECORDING stubs, then
// assert: (1) NO teardown ever targets a NON-owning lane; specifically NEVER close(fd 0/1/2) coming from a
// zero-init lane; (2) EVERY owning lane's fd + map is released exactly once; (3) after reset every lane's
// wake_fd is the honest -1 sentinel (so a later read can't mistake 0 for a live fd).
//
// GREEN (default): teardown gated on the ownership marker (active==1). Zero violations.
// RED arm -DRESET_GATE_ON_WAKEFD_GE0: teardown gated on `wake_fd >= 0` (the shipped D16 bug). The
//   enumeration MUST find violations (close(0) on the zero-init lanes) -> nonzero exit.
// RED arm -DRESET_LEAVES_WAKEFD_ZERO: teardown does not restore the -1 sentinel (leaves wake_fd at 0).
//   The post-condition check MUST fail -> nonzero exit.

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// ---- the modeled lane (mirror of gr_lane_t's teardown-relevant fields) ----------------------------
// Only the fields postfork_reset touches: the publish marker (active), the two owned resources
// (map + wake_fd) and their size. Matches dserver-ring.c gr_lane_t field-for-field in intent.
typedef struct {
	uint32_t active;   // 0 = free (zero-init default!), 1 = live/published
	void*    map;      // owned mapping, or NULL
	uint64_t size;     // mapping size
	int      wake_fd;  // owned wake eventfd, or the -1 "no fd" sentinel
} model_lane_t;

#define MODEL_LANES 8

// ---- recording teardown stubs (stand in for LINUX_SYSCALL munmap/close) ----------------------------
// The whole point of the gate is to observe WHICH fds/maps the reset decides to release, without actually
// touching real fds. Every close target is recorded so we can assert none of them is a stdio fd coming
// from a non-owning (zero-init) lane.
static int  g_close_fds[64];
static int  g_close_n = 0;
static void*g_munmap_addrs[64];
static int  g_munmap_n = 0;

static void rec_close(int fd)        { if (g_close_n  < 64) g_close_fds[g_close_n++]   = fd;  }
static void rec_munmap(void* a)      { if (g_munmap_n < 64) g_munmap_addrs[g_munmap_n++] = a; }

// ---- the code under test: the postfork_reset per-lane teardown decision ----------------------------
// This mirrors __dserver_ring_postfork_reset() (dserver-ring.c). The ONE line under test is the teardown
// GATE. GREEN gates on the ownership marker (active==1); the RED arm restores the shipped bug (wake_fd>=0).
static void model_postfork_reset(model_lane_t* lanes, int n) {
	for (int i = 0; i < n; ++i) {
		model_lane_t* L = &lanes[i];
#ifdef RESET_GATE_ON_WAKEFD_GE0
		// RED (the shipped D16 bug): free by the resource VALUE. A zero-init lane has wake_fd==0, 0>=0 is
		// true, so this releases the mapping (garbage) and closes fd 0 for every untouched lane.
		if (L->map && L->size) rec_munmap(L->map);
		if (L->wake_fd >= 0)   rec_close(L->wake_fd);
#else
		// GREEN (the fix): free by the OWNERSHIP MARKER. Only a lane acquire actually published owns
		// resources; a zero-init lane (active==0) owns nothing and is skipped entirely.
		if (L->active == 1) {
			if (L->map && L->size)   rec_munmap(L->map);
			if (L->wake_fd >= 0)     rec_close(L->wake_fd);
		}
#endif
		// zero the lane + restore the HONEST sentinel (wake_fd == -1). Both arms clear the table; the
		// RED sentinel arm below deliberately skips restoring -1 to prove the post-condition is checked.
		L->active = 0;
		L->map = 0;
		L->size = 0;
#ifndef RESET_LEAVES_WAKEFD_ZERO
		L->wake_fd = -1;   // honest "no fd" sentinel: a later read can never mistake this for live fd 0
#endif
	}
}

static int g_violations = 0;

// Build a table exactly as the guest sees it after a fork: a `static` (zero-init) table with a few lanes
// that had been ACQUIRED (published, owning a fake mapping + a real-looking wake_fd) before the fork.
// The untouched lanes are left as calloc/zero gives them: active==0, map==NULL, size==0, wake_fd==0.
static void build_postfork_table(model_lane_t* lanes, int n) {
	memset(lanes, 0, (size_t)n * sizeof(*lanes)); // <- the crux: mirrors `static gr_lane_t g_lanes[...]`
	// Two lanes were live at fork time. Give them owned resources with wake_fds that are NOT 0/1/2 (a real
	// attached lane's wake_fd came back from the server well above the stdio range).
	lanes[2].active = 1; lanes[2].map = (void*)0x1000; lanes[2].size = 4096; lanes[2].wake_fd = 17;
	lanes[5].active = 1; lanes[5].map = (void*)0x2000; lanes[5].size = 8192; lanes[5].wake_fd = 23;
}

int main(void) {
	model_lane_t lanes[MODEL_LANES];
	build_postfork_table(lanes, MODEL_LANES);

	// Snapshot which lanes truly OWN resources (the ground truth: active==1 at fork time).
	int owner[MODEL_LANES];
	int owner_fd[MODEL_LANES];
	void* owner_map[MODEL_LANES];
	for (int i = 0; i < MODEL_LANES; ++i) {
		owner[i]     = (lanes[i].active == 1);
		owner_fd[i]  = lanes[i].wake_fd;
		owner_map[i] = lanes[i].map;
	}

	g_close_n = 0; g_munmap_n = 0;
	model_postfork_reset(lanes, MODEL_LANES);

	// INVARIANT 1: no teardown may target a stdio fd (0/1/2). A close of fd 0 here can ONLY have come from
	// a zero-init non-owning lane -> the exact bug. This is the load-bearing assertion.
	for (int k = 0; k < g_close_n; ++k) {
		if (g_close_fds[k] >= 0 && g_close_fds[k] <= 2) {
			fprintf(stderr, "VIOLATION: postfork_reset closed stdio fd %d (from a non-owning zero-init lane)\n",
			        g_close_fds[k]);
			++g_violations;
		}
	}

	// INVARIANT 2: the ONLY fds closed / maps unmapped are those the owning lanes held -- exactly once each,
	// and nothing from a non-owner.
	// 2a: every closed fd must be an owner's fd.
	for (int k = 0; k < g_close_n; ++k) {
		int matched = 0;
		for (int i = 0; i < MODEL_LANES; ++i)
			if (owner[i] && owner_fd[i] == g_close_fds[k]) { matched = 1; break; }
		if (!matched) {
			fprintf(stderr, "VIOLATION: closed fd %d belongs to no owning lane (spurious free)\n", g_close_fds[k]);
			++g_violations;
		}
	}
	// 2b: every owning lane's fd + map must have been released exactly once.
	for (int i = 0; i < MODEL_LANES; ++i) {
		if (!owner[i]) continue;
		int fd_closes = 0, map_unmaps = 0;
		for (int k = 0; k < g_close_n; ++k)  if (g_close_fds[k]    == owner_fd[i])  ++fd_closes;
		for (int k = 0; k < g_munmap_n; ++k) if (g_munmap_addrs[k] == owner_map[i]) ++map_unmaps;
		if (fd_closes != 1)  { fprintf(stderr, "VIOLATION: owning lane %d fd closed %d times (want 1)\n", i, fd_closes); ++g_violations; }
		if (map_unmaps != 1) { fprintf(stderr, "VIOLATION: owning lane %d map unmapped %d times (want 1)\n", i, map_unmaps); ++g_violations; }
	}

	// INVARIANT 3 (post-condition): after reset, EVERY lane carries the honest -1 sentinel, so a later read
	// of wake_fd can never mistake the zero default for a live fd 0.
	for (int i = 0; i < MODEL_LANES; ++i) {
		if (lanes[i].wake_fd != -1) {
			fprintf(stderr, "VIOLATION: lane %d wake_fd left at %d after reset (want -1 sentinel)\n", i, lanes[i].wake_fd);
			++g_violations;
		}
	}

#if defined(RESET_GATE_ON_WAKEFD_GE0) || defined(RESET_LEAVES_WAKEFD_ZERO)
	if (g_violations > 0) {
		fprintf(stderr, "RED arm correctly found %d postfork_reset violation(s)\n", g_violations);
		return 1; // a RED arm MUST find violations
	}
	fprintf(stderr, "RED arm found ZERO violations but MUST find some -- gate not exercising the reset predicate\n");
	return 0; // harness treats a passing RED arm as failure
#else
	if (g_violations > 0) {
		fprintf(stderr, "GREEN: FAILED -- %d postfork_reset violation(s) under the real ownership-gated teardown\n", g_violations);
		return 1;
	}
	fprintf(stderr, "GREEN: postfork_reset frees resources for exactly the owning lanes; no stdio fd closed; -1 sentinel restored\n");
	return 0;
#endif
}
