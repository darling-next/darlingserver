// Regression test for dar-l8k (under epic dar-dar6x4-perf-5dq):
// darlingserver SIGSEGVs RARELY under fork+exec storms (~1 in 3-6 heavy fork-heavy
// builds), and DETERMINISTICALLY at boot when built with AddressSanitizer. Pinned to a
// heap-use-after-free with an ASan-instrumented server build:
//
//   freed by:  free() <- dtape_thread_release(oldThread)   src/process.cpp:364 (pre-fix)
//   used by:   ipc_port_destroy reads current_thread()->ith_assertions
//                                                          ipc/ipc_port.c:936
//              via dtape_task_release(oldThread's task)     src/process.cpp:365 (pre-fix)
//   alloc:     dtape_thread_create (the dtape_thread struct, ~3200 bytes)
//
// Mechanism (Process::notifyCheckin, the exec/"replace task" branch):
//   1. a new task + new dtape thread are created to replace the old ones,
//   2. (BUG) the OLD dtape thread is released -> on its last ref it is free()d,
//   3. the OLD task is released -> its IPC space is torn down -> ipc_space_terminate ->
//      ipc_right_terminate -> ipc_port_destroy, which (IMPORTANCE_INHERITANCE=1) reads
//      `current_thread()->ith_assertions`.
//   `current_thread()` resolves through dtape_hook_current_thread to
//   `Thread::currentThread()->_dtapeThread` == mainThread->_dtapeThread, which is STILL
//   the freed OLD thread at this point (the new thread is not published until AFTER the
//   releases). So the task teardown dereferences the just-freed dtape_thread -> UAF.
//
// The fix publishes mainThread->_dtapeThread = newThread (so current_thread() resolves to
// the LIVE new thread) BEFORE releasing the old thread/task. This test models that exact
// invariant with the same primitives: a "current thread" pointer consulted during a task
// teardown, a heap-allocated thread object freed on its last release, and the two possible
// orderings. Built with -fsanitize=address: the buggy order trips a deterministic
// heap-use-after-free; the fixed order is clean.
//
//   RED   (unsafe): release+free the old thread, THEN run the task teardown while the
//                   "current thread" pointer still points at the freed object -> the
//                   teardown reads it -> ASan heap-use-after-free -> FAIL.
//   GREEN (fixed):  publish the new thread as "current" first, THEN release the old one;
//                   the teardown reads the live new thread -> clean -> PASS.
//
// Arm by argv[1]: "unsafe" = RED, "safe" = GREEN (default).
// HOST test (plain glibc + ASan). Exit 0 = PASS. See run-checkin-retask-current-thread-uaf.sh.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// Models a dtape_thread: the field the teardown reads is `ith_assertions` (as in
// ipc_port_destroy: `current_thread()->ith_assertions`). Heap-allocated so ASan tracks it.
typedef struct dtape_thread {
	atomic_int ref_count;     // os_ref; release drops it, frees on last ref
	int        ith_assertions; // the field ipc_port_destroy reads via current_thread()
	int        thread_id;
} dtape_thread_t;

// The microthread's "current thread" pointer, resolved by current_thread() during the
// task teardown -- this is the moral equivalent of mainThread->_dtapeThread.
static dtape_thread_t* g_current_thread = NULL;

static dtape_thread_t* thread_create(int id) {
	dtape_thread_t* t = (dtape_thread_t*)malloc(sizeof(dtape_thread_t));
	atomic_store(&t->ref_count, 1);
	t->ith_assertions = 0;
	t->thread_id = id;
	return t;
}

// thread_deallocate / dtape_thread_release: drop a ref; free on the last one.
static void thread_release(dtape_thread_t* t) {
	if (atomic_fetch_sub(&t->ref_count, 1) == 1) {
		free(t);  // == dtape_thread_destroy -> free(thread)
	}
}

// Models the body of the old task's IPC-space teardown that reaches ipc_port_destroy,
// which reads current_thread()->ith_assertions. Whatever g_current_thread points at when
// this runs gets dereferenced.
static void task_release_teardown(void) {
	dtape_thread_t* self = g_current_thread;   // == current_thread()
	// ipc_port_destroy:936 -> `boolean_t top = (self->ith_assertions == 0);`
	volatile int top = (self->ith_assertions == 0);  // UAF READ in the buggy ordering
	(void)top;
}

int main(int argc, char** argv) {
	const char* arm = (argc > 1) ? argv[1] : "safe";
	int safe = (strcmp(arm, "unsafe") == 0) ? 0 : 1;

	// run many rounds; the UAF is deterministic under ASan on the very first one, but we
	// loop to mirror a checkin storm and keep the harness shape consistent.
	const int ROUNDS = 1000;
	for (int r = 0; r < ROUNDS; ++r) {
		// state at the top of the exec/"replace task" branch: the OLD thread is current.
		dtape_thread_t* oldThread = thread_create(r * 2);
		g_current_thread = oldThread;

		// create the replacement thread (models dtape_thread_create(newTask, ...)).
		dtape_thread_t* newThread = thread_create(r * 2 + 1);

		if (safe) {
			// GREEN/fix: publish the new thread as current FIRST, so current_thread()
			// resolves to the live newThread during the old task's teardown...
			g_current_thread = newThread;
			// ...then release the old thread (frees it) and run the old task teardown.
			thread_release(oldThread);
			task_release_teardown();    // reads g_current_thread == live newThread -> OK
		} else {
			// RED/buggy (the pre-fix order): release+free the old thread while it is STILL
			// current, then run the old task's teardown -> it reads the freed old thread.
			thread_release(oldThread);  // last ref -> free(oldThread)
			task_release_teardown();    // reads g_current_thread == freed oldThread -> UAF
			g_current_thread = newThread; // (too late: the teardown already UAF'd)
		}

		// drop the surviving thread to keep the test leak-clean.
		thread_release(g_current_thread);
		g_current_thread = NULL;
	}

	printf("PASS: no use-after-free (arm=%s)\n", arm);
	return 0;
}
