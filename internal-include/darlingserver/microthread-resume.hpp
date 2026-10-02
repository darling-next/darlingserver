#ifndef _DARLINGSERVER_MICROTHREAD_RESUME_HPP_
#define _DARLINGSERVER_MICROTHREAD_RESUME_HPP_

#include <cassert>

namespace DarlingServer {
	// Owns physical execution only; XNU wait state has its own lock and lifetime.
	// Caller serializes transitions with the thread's C++ lock.
	class MicrothreadExecution {
		enum class State {
			Idle,
			Running,
			RunningWithPendingDispatch,
		};
		State _state = State::Idle;

	public:
		bool isRunning() const {
			return _state != State::Idle;
		}

		bool deferDispatch() {
			if (_state == State::Idle) {
				return false;
			}
			_state = State::RunningWithPendingDispatch;
			return true;
		}

		void begin() {
			assert(_state == State::Idle);
			_state = State::Running;
		}

		// Return the coalesced dispatch to the caller as ownership is released.
		bool finish() {
			assert(isRunning());
			bool dispatchPending = _state == State::RunningWithPendingDispatch;
			_state = State::Idle;
			return dispatchPending;
		}
	};

	inline bool microthreadRecordResume(bool running, bool suspended, bool& resumePermit) {
		if (!running && !suspended) {
			return false;
		}
		if (resumePermit) {
			return false;
		}
		resumePermit = true;
		return suspended && !running;
	}

	inline bool microthreadConsumePendingResume(bool& resumePermit) {
		if (!resumePermit) {
			return false;
		}
		resumePermit = false;
		return true;
	}

	inline bool microthreadConsumeResumeDuringSuspend(bool& suspended, bool& resumePermit) {
		if (!microthreadConsumePendingResume(resumePermit)) {
			return false;
		}
		suspended = false;
		return true;
	}

	inline bool microthreadShouldScheduleAfterStop(bool resumePermit, bool suspended, bool terminating, bool dead) {
		return resumePermit && suspended && !terminating && !dead;
	}
}

#endif
