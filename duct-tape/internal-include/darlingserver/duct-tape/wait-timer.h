#ifndef _DARLINGSERVER_DUCT_TAPE_WAIT_TIMER_H_
#define _DARLINGSERVER_DUCT_TAPE_WAIT_TIMER_H_

#include <stdbool.h>

static inline bool dtape_thread_cancel_wait_timer(thread_t thread) {
	if (!thread->wait_timer_is_set) {
		return false;
	}
	if (timer_call_cancel(&thread->wait_timer)) {
		thread->wait_timer_active--;
	}
	thread->wait_timer_is_set = false;
	return true;
}

static inline bool dtape_thread_prepare_for_wait(thread_t thread) {
	return dtape_thread_cancel_wait_timer(thread);
}

#endif
