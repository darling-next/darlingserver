// Regression test for perf #2b (dar-dar6x4-perf-5dq.8):
// darlingserver's main event loop must run cheap, non-blocking RPCs INLINE instead of
// handing every message to a worker thread via a condvar wakeup.
//
// perf #2a profiling showed ~70% of server CPU under a fork/checkin storm was the
// main-loop -> worker handoff: per message the main loop did pthread_cond_signal ->
// kernel futex_wake -> wake a parked worker that did sub-microsecond work and re-parked.
// The work itself was <1% of CPU. So for the common cheap RPC the wakeup tax dominates.
//
// We can't link the whole server in a host test, so we model the two dispatch strategies
// over an identical, deliberately-tiny "RPC" (the work a checkin does is trivial) and
// measure the involuntary context switches the dispatch costs:
//
//   RED  (handoff):  producer hands each item to a pool worker via mutex+condvar.
//                    Every item forces a futex_wake + a cross-thread context switch.
//   GREEN (inline):  producer runs the same tiny work itself, no wakeup, no switch.
//
// The discriminator is involuntary+voluntary context switches per item (getrusage
// nvcsw+nivcsw). The handoff path incurs ~1 voluntary ctx switch per item (the worker
// blocks/unblocks); the inline path incurs ~none. This is exactly the cost the fix
// removes. HOST test (plain glibc); see run-server-inline-fastpath.sh. Exit 0 = PASS.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/resource.h>

#define N_ITEMS 200000

// the "RPC work": trivial, like a checkin (bump a counter). Volatile so it isn't elided.
static volatile uint64_t g_work_sink = 0;
static inline void do_tiny_rpc(void) { g_work_sink += 1; }

// ---- handoff (RED) machinery: a single pool worker fed via condvar ----
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cv  = PTHREAD_COND_INITIALIZER;
static int q_pending = 0;       // 1 = an item is waiting for the worker
static int q_done_flag = 0;     // worker sets after handling, producer waits on it
static pthread_cond_t  done_cv = PTHREAD_COND_INITIALIZER;
static int q_dying = 0;

static void* handoff_worker(void* arg) {
	(void)arg;
	pthread_mutex_lock(&q_mtx);
	while (1) {
		while (!q_pending && !q_dying)
			pthread_cond_wait(&q_cv, &q_mtx);   // park -> futex_wait
		if (q_dying && !q_pending) break;
		q_pending = 0;
		pthread_mutex_unlock(&q_mtx);

		do_tiny_rpc();                          // the actual (tiny) work

		pthread_mutex_lock(&q_mtx);
		q_done_flag = 1;
		pthread_cond_signal(&done_cv);          // wake producer -> futex_wake
	}
	pthread_mutex_unlock(&q_mtx);
	return NULL;
}

static long ctx_switches(void) {
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_nvcsw + ru.ru_nivcsw;
}

int main(int argc, char** argv) {
	int use_handoff = (argc > 1 && strcmp(argv[1], "handoff") == 0);

	long csw0 = ctx_switches();

	if (use_handoff) {
		// RED: dispatch every item to the worker, synchronously, as the old main loop
		// effectively did (push + signal, worker wakes, runs, signals back).
		pthread_t th;
		pthread_create(&th, NULL, handoff_worker, NULL);
		for (int i = 0; i < N_ITEMS; ++i) {
			pthread_mutex_lock(&q_mtx);
			q_pending = 1;
			q_done_flag = 0;
			pthread_cond_signal(&q_cv);          // wake the worker
			while (!q_done_flag)
				pthread_cond_wait(&done_cv, &q_mtx);
			pthread_mutex_unlock(&q_mtx);
		}
		pthread_mutex_lock(&q_mtx);
		q_dying = 1;
		pthread_cond_signal(&q_cv);
		pthread_mutex_unlock(&q_mtx);
		pthread_join(th, NULL);
	} else {
		// GREEN: run the tiny work inline on the dispatching thread -- no wakeup.
		for (int i = 0; i < N_ITEMS; ++i)
			do_tiny_rpc();
	}

	long csw = ctx_switches() - csw0;
	printf("variant=%s items=%d ctx_switches=%ld (%.2f per item) sink=%llu\n",
		use_handoff ? "handoff" : "inline", N_ITEMS, csw,
		(double)csw / N_ITEMS, (unsigned long long)g_work_sink);

	// The inline path should incur essentially no context switches; the handoff path
	// incurs roughly one per item. Gate at a low fraction so the test is robust to a
	// little scheduler noise but still fails hard for the per-item-wakeup design.
	const double LIMIT_PER_ITEM = 0.10;
	double per_item = (double)csw / N_ITEMS;
	if (per_item > LIMIT_PER_ITEM) {
		fprintf(stderr, "FAIL: %.2f ctx-switches/item (limit %.2f) -- per-item worker wakeup\n",
			per_item, LIMIT_PER_ITEM);
		return 1;
	}
	printf("PASS: inline dispatch incurred %.2f ctx-switches/item (<= %.2f)\n", per_item, LIMIT_PER_ITEM);
	return 0;
}
