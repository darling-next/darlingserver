#include <darlingserver/interrupt-resume.hpp>

namespace DarlingServer::InterruptResume {

Action classify(const State& state)
{
	// A re-entry through the syscall-return path means the interrupted call already finished its
	// syscall return during this interrupt: there is nothing left to resume.
	if (state.didSyscallReturnDuringInterrupt) {
		return Action::CompleteInterrupt;
	}

	// The interrupted call left a continuation instead of a suspended fiber; the continuation is
	// the only representation of its continuation, so it is what runs.
	if (state.haveInterruptedContinuation) {
		return Action::RunContinuation;
	}

	// Only a call whose suspension context was actually captured may be restored through it.
	// A present-but-never-suspended call (measured: a plane-serviced call that never reached a
	// suspend()) keeps its ownership and is completed by its own path.
	if (state.haveInterruptedCall && state.resumeContextCaptured) {
		return Action::ResumeCapturedContext;
	}

	return Action::CompleteInterrupt;
}

bool restoresCapturedContext(Action action)
{
	return action == Action::ResumeCapturedContext;
}

}
