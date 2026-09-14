#ifndef _DARLINGSERVER_DUCT_TAPE_THREAD_CANCEL_H_
#define _DARLINGSERVER_DUCT_TAPE_THREAD_CANCEL_H_

#include <stdbool.h>
#include <sys/errno.h>

typedef struct dtape_thread_cancel_state {
	// POSIX thread-cancellation state, mirroring XNU's per-uthread uu_flag bits
	// (UT_CANCELDISABLE / UT_CANCEL / UT_CANCELED). The guest libpthread drives
	// these through the __pthread_canceled / __pthread_markcancel syscalls.
	bool disable;
	bool pending;
	bool canceled;
} dtape_thread_cancel_state_t;

static inline void
dtape_thread_cancel_state_init(dtape_thread_cancel_state_t* state)
{
	state->disable = false;
	state->pending = false;
	state->canceled = false;
}

static inline int
dtape_thread_cancel_state_canceled(dtape_thread_cancel_state_t* state, int action)
{
	switch (action) {
		case 1:
			state->disable = false;
			return 0;
		case 2:
			state->disable = true;
			return 0;
		case 0:
		default:
			// Mirror XNU: act only when UT_CANCEL is set and neither
			// UT_CANCELDISABLE nor UT_CANCELED is set, i.e.
			// (uu_flag & (CANCELDISABLE|CANCEL|CANCELED)) == UT_CANCEL.
			if (state->pending && !state->disable && !state->canceled) {
				state->pending = false;
				state->canceled = true;
				return 0;
			}
			return EINVAL;
	}
}

static inline int
dtape_thread_cancel_state_markcancel(dtape_thread_cancel_state_t* state)
{
	// Mirror XNU's guard: only arm a cancel if one is not already in flight
	// or acted upon ((uu_flag & (CANCEL|CANCELED)) == 0; we have no vfork bit).
	if (!state->pending && !state->canceled) {
		state->pending = true;
	}
	return 0;
}

#endif // _DARLINGSERVER_DUCT_TAPE_THREAD_CANCEL_H_
