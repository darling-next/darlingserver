// Regression test for dar-6x4 (under epic dar-dar6x4-perf-5dq):
// darlingserver deadlocks under a fork+exec storm (e.g. `make -jN` of many tiny
// compiles). Root cause, pinned live (gdb: __cur_writer == MAIN thread tid, the
// single worker blocked in pthread_rwlock_rdlock with __readers==WRLOCKED|WRPHASE):
//
//   perf #2b (dar-dar6x4-perf-5dq.8) runs cheap RPCs INLINE on the MAIN event-loop
//   thread instead of always handing them to the single worker. A microthread that
//   suspends keeps running later on the WORKER. But darlingserver's per-process lock
//   (Process::_rwlock, a std::shared_mutex) is a real OS rwlock whose ownership is
//   per-OS-THREAD. notifyCheckin()'s exec branch holds that write lock across the
//   suspending dtape_task_create / dtape_thread_create calls. When that checkin runs
//   inline on MAIN and suspends, the write lock is left owned by MAIN while the
//   microthread parks. A sibling's fork-checkin then runs on the WORKER and blocks in
//   _notifyListeningKqchannels() taking the SAME process lock for read -- forever,
//   because the only thread that could resume the lock-holding microthread is that
//   now-stuck worker. The whole server wedges (rpcs_serviced flat, every guest's
//   recvmsg hangs). Pre-#2b this could not happen: ALL microthreads ran on the single
//   worker, so a process lock was always acquired and released on the same OS thread.
//
// We can't link the whole server in a host test, so we model the exact invariant with
// the same primitives: a std::shared_mutex-style rwlock (pthread_rwlock) that a
// "microthread" takes for WRITE on OS-thread A (= main) and then "suspends" (parks)
// WITHOUT releasing, while OS-thread B (= worker) tries to take it for READ. With a
// writer-preferring/ writer-present glibc rwlock this read blocks, and since the
// write-holder never resumes (its resume vehicle is exactly the blocked reader thread),
// the pair deadlocks -- which is the bug.
//
//   RED   (held-across-suspend): writer parks holding the write lock -> reader on the
//                                other OS thread blocks forever -> DEADLOCK -> FAIL.
//   GREEN (fixed):               the write lock is RELEASED before the suspend point
//                                (the fix: notifyCheckin drops _rwlock around the
//                                suspending dtape calls) -> the reader proceeds ->
//                                both finish -> PASS.
//
// The arm is chosen by argv[1]: "heldacross" = RED, "released" = GREEN (default).
// A watchdog thread bounds the run: if the reader hasn't completed within the deadline
// the process reports DEADLOCK and exits non-zero. HOST test (plain glibc + pthreads).
// Exit 0 = PASS (no deadlock). See run-inline-suspend-rwlock-deadlock.sh.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

// The per-process lock, modeled as a glibc rwlock (std::shared_mutex maps onto this).
static pthread_rwlock_t proc_lock = PTHREAD_RWLOCK_INITIALIZER;

// Coordination so the reader (worker) only races the lock AFTER the writer (main) has
// taken it -- this makes the deadlock deterministic rather than timing-dependent.
static atomic_int writer_locked = 0;     // writer has acquired the write lock
static atomic_int reader_done = 0;       // reader successfully took+released read lock
static atomic_int writer_parked = 0;     // writer has reached its "suspend" point

// Whether the writer releases the lock before parking (GREEN) or holds it (RED).
static int g_release_before_suspend = 1;

// "worker" OS thread: models a sibling fork-checkin running notifyCheckin ->
// _notifyListeningKqchannels -> rdlock(parent->_rwlock).
static void* worker_reader(void* arg) {
	(void)arg;
	// wait until the writer (main) holds the lock, to force the contended ordering
	while (!atomic_load(&writer_locked))
		usleep(100);
	pthread_rwlock_rdlock(&proc_lock);     // <-- blocks forever in the RED arm
	// got the read lock: do the trivial notify work, then release
	pthread_rwlock_unlock(&proc_lock);
	atomic_store(&reader_done, 1);
	return NULL;
}

// "main" OS thread inline path: models notifyCheckin's exec branch taking the process
// write lock and then suspending (dtape_task_create/thread_create park the microthread).
static void main_inline_checkin(void) {
	pthread_rwlock_wrlock(&proc_lock);     // take process _rwlock for WRITE
	atomic_store(&writer_locked, 1);

	// ... mutate process state (architecture, dtape task/thread) ...

	// THE SUSPEND POINT: dtape_task_create / dtape_thread_create park the microthread.
	// In the real server the microthread would resume later on the WORKER. Here we just
	// model "we are about to give up this OS thread for a long time while a sibling runs
	// on the worker." The fix releases the lock before this point.
	if (g_release_before_suspend) {
		pthread_rwlock_unlock(&proc_lock); // GREEN: fix -- don't hold _rwlock across suspend
	}
	atomic_store(&writer_parked, 1);

	// "suspend": wait for the worker's reader to make progress. In the RED arm, if we
	// still hold the write lock, the reader can never make progress, so this never
	// returns within the deadline -> the watchdog fires -> deadlock proven.
	while (!atomic_load(&reader_done))
		usleep(100);

	if (!g_release_before_suspend) {
		// (never reached in RED before the watchdog, but keep it correct)
		pthread_rwlock_unlock(&proc_lock);
	}
}

static void* watchdog(void* arg) {
	double deadline_s = *(double*)arg;
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		usleep(20 * 1000);
		if (atomic_load(&reader_done))
			return NULL;                   // success: reader finished
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
		if (elapsed > deadline_s) {
			fprintf(stderr,
				"DEADLOCK: reader did not complete within %.1fs "
				"(writer_locked=%d writer_parked=%d reader_done=%d)\n",
				deadline_s, atomic_load(&writer_locked),
				atomic_load(&writer_parked), atomic_load(&reader_done));
			// hard-exit non-zero: the deadlock is real and the process can't unwind it
			_exit(1);
		}
	}
}

int main(int argc, char** argv) {
	const char* arm = (argc > 1) ? argv[1] : "released";
	if (strcmp(arm, "heldacross") == 0)
		g_release_before_suspend = 0;      // RED: hold the lock across the suspend
	else
		g_release_before_suspend = 1;      // GREEN: release before suspend (the fix)

	static double deadline_s = 4.0;
	pthread_t wd, wk;
	pthread_create(&wd, NULL, watchdog, &deadline_s);
	pthread_create(&wk, NULL, worker_reader, NULL);

	main_inline_checkin();

	pthread_join(wk, NULL);
	// cancel the watchdog (reader_done is set, so it returns on its own next tick)
	pthread_join(wd, NULL);

	printf("PASS: no deadlock (arm=%s) -- write lock was %s across the suspend\n",
		arm, g_release_before_suspend ? "RELEASED before" : "HELD across");
	return 0;
}
