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

Retirement retireInterruptedCall(const OwnershipState& state)
{
	if (!state.interruptedCall) {
		return Retirement::Nothing;
	}

	// The interrupt resumed the call (through its captured context, or by running its
	// continuation): its own path ends here, so the record is the completion.
	if (state.resumed) {
		return Retirement::Retire;
	}

	// Not resumed. A plane-serviced call already answered its caller from the management page, so no
	// waiter exists and the record may be retired. A call that still has a reply to deliver may not
	// be retired by this path, because nothing here can deliver it.
	return state.replySuppressed ? Retirement::Retire : Retirement::Keep;
}

bool retirementIsAmbiguous(const OwnershipState& state)
{
	// Retiring and keeping are mutually exclusive outcomes; the rule above must never need both.
	return (retireInterruptedCall(state) == Retirement::Keep) && state.resumed;
}

PendingSignalResult setPendingSignal(const PendingSignalState& state, int signal)
{
	PendingSignalResult result;

	// No marker to update is a legal state (processSignal and pendingSignal already accept it); it is
	// reported, never raised, and nothing is mutated.
	if (!state.interruptMarker) {
		result.raised = false;
		result.applied = false;
		result.previous = 0;
		return result;
	}

	result.raised = false;
	result.applied = true;
	result.previous = state.currentPending;
	(void)signal; // the marker's new value is the caller's; this models the bookkeeping only
	return result;
}

}
