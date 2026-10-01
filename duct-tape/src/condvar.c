#include <darlingserver/duct-tape/condvar.h>
#include <darlingserver/duct-tape/thread.h>
#include <darlingserver/duct-tape/hooks.internal.h>
#include <darlingserver/duct-tape/log.h>

// an extremely unoptimized (and honestly, half-assed) implementation of condition variables for duct-taped code

void dtape_condvar_init(dtape_condvar_t* condvar) {
	libsimple_lock_init(&condvar->queue_lock);
	TAILQ_INIT(&condvar->queue_head);
};

void dtape_condvar_signal(dtape_condvar_t* condvar, size_t count) {
	libsimple_lock_lock(&condvar->queue_lock);
	while (count > 0) {
		dtape_mutex_link_t* link = TAILQ_FIRST(&condvar->queue_head);
		if (!link) {
			break;
		}

		TAILQ_REMOVE(&condvar->queue_head, link, link);
		link->_dbg_queued = 0;
		dtape_thread_t* thread = __container_of(link, dtape_thread_t, mutex_link);
		dtape_hooks->thread_resume(thread->context);

		--count;
	}
	libsimple_lock_unlock(&condvar->queue_lock);
};

void dtape_condvar_wait(dtape_condvar_t* condvar, dtape_mutex_t* mutex) {
	dtape_thread_t* thread = dtape_thread_for_xnu_thread(current_thread());

	libsimple_lock_lock(&condvar->queue_lock);

	// unlocking the mutex here is safe;
	// we can't be signaled until we drop the queue lock,
	// which we only do once we actually suspend ourselves,
	// so there's no chance for us to miss a wakeup here.
	dtape_mutex_unlock(mutex);

	// add ourselves to the wait queue
	thread->mutex_link._dbg_queued = 1;
	TAILQ_INSERT_TAIL(&condvar->queue_head, &thread->mutex_link, link);

	// now let's suspend ourselves to wait;
	// this also drops the queue lock.
	dtape_hooks->thread_suspend(thread->context, NULL, NULL, &condvar->queue_lock);

	// perf#25a A0: we've been awoken -- but by WHAT? A normal dtape_condvar_signal
	// dequeued us (TAILQ_REMOVE + _dbg_queued=0) before resuming us. A SIGNAL-ABORT
	// (dtape_thread_sigexc_enter -> clear_wait_internal -> thread_resume) resumed us
	// WITHOUT touching this queue, so our mutex_link is still linked here. If we then
	// fall through to dtape_mutex_lock() below it would TAILQ_INSERT_TAIL the SAME
	// link into the mutex queue -> the one link is in two queues -> tailq corruption
	// (locks.c:151) -> a later unlock wakes a garbage link -> lost wakeup -> the brew
	// condvar livelock. Defensively unlink ourselves under the condvar lock before
	// reacquiring the mutex. (proven double-insert; diagnostic in git history d5d01dd)
	if (thread->mutex_link._dbg_queued) {
		libsimple_lock_lock(&condvar->queue_lock);
		if (thread->mutex_link._dbg_queued) {
			TAILQ_REMOVE(&condvar->queue_head, &thread->mutex_link, link);
			thread->mutex_link._dbg_queued = 0;
		}
		libsimple_lock_unlock(&condvar->queue_lock);
	}

	// we've been awoken; reacquire the mutex
	dtape_mutex_lock(mutex);
};
