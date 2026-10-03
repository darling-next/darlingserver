// Deterministic InterruptEnter/resume contract.
//
// A. interrupted call suspended with a captured context + InterruptEnter -> restore the context
// B. interrupted call represented by a continuation       + InterruptEnter -> run the continuation
// C. active call NOT suspended (no captured context)      + InterruptEnter -> must NOT setcontext
// D. InterruptEnter arrives before the active call begins processing (same as C: no context yet)
// E. syscall-return-during-interrupt re-entry             -> bookkeeping only
// F. nested/repeated interrupt states the implementation must still classify safely
//
// The invariant: no restore of an uncaptured context. The product calls the same classify(), so
// this contract covers the decision the server actually makes.
#include <darlingserver/interrupt-resume.hpp>

#include <cstdlib>
#include <iostream>

using DarlingServer::InterruptResume::Action;
using DarlingServer::InterruptResume::State;

static int failures = 0;

static void require(bool condition, const char* message)
{
	if (!condition) {
		std::cerr << "FAIL " << message << '\n';
		++failures;
	}
}

static void expect(Action expected, const State& state, const char* message)
{
	require(DarlingServer::InterruptResume::classify(state) == expected, message);
}

static void neverResumesUncaptured(const State& state, const char* message)
{
	auto action = DarlingServer::InterruptResume::classify(state);
	require(!DarlingServer::InterruptResume::restoresCapturedContext(action) || state.resumeContextCaptured, message);
}

// The pre-fix inline rule, modelled explicitly: restore the interrupted call's context whenever an
// interrupted call exists (ignoring whether one was ever captured). This is the behavior MEASURED
// to fault inside glibc's setcontext(). Keeping it here makes the contract a two-arm proof: the
// model must violate the invariant, and the shipped classify() must not.
static Action preFixRule(const State& s)
{
	if (s.didSyscallReturnDuringInterrupt) {
		return Action::CompleteInterrupt;
	}
	if (s.haveInterruptedContinuation) {
		return Action::RunContinuation;
	}
	if (s.haveInterruptedCall) {
		return Action::ResumeCapturedContext;
	}
	return Action::CompleteInterrupt;
}

int main()
{
	// RED ARM: the old rule restores an uncaptured context in exactly the measured case.
	{
		State s;
		s.haveInterruptedCall = true;
		s.resumeContextCaptured = false;
		Action old = preFixRule(s);
		require(DarlingServer::InterruptResume::restoresCapturedContext(old),
			"RED: the pre-fix rule must be shown to restore the uncaptured context (measured fault)");
		require(DarlingServer::InterruptResume::classify(s) != old,
			"GREEN: the shipped decision must differ from the pre-fix rule for the measured case");
		std::cout << "RED/GREEN arm: pre-fix restores an uncaptured context, shipped decision does not\n";
	}
	// A: suspended call with a captured context resumes through it.
	{
		State s;
		s.haveInterruptedCall = true;
		s.resumeContextCaptured = true;
		expect(Action::ResumeCapturedContext, s, "A: captured context must be restored");
		require(DarlingServer::InterruptResume::restoresCapturedContext(DarlingServer::InterruptResume::classify(s)),
			"A: restoring a captured context is the expected action");
	}

	// B: a continuation is the interrupted call's representation; it runs instead of a context.
	{
		State s;
		s.haveInterruptedContinuation = true;
		s.haveInterruptedCall = true;
		s.resumeContextCaptured = true;
		expect(Action::RunContinuation, s, "B: continuation path must be taken");
	}

	// C: the measured case. An interrupted call that never suspended has no context to restore.
	{
		State s;
		s.haveInterruptedCall = true;
		s.resumeContextCaptured = false;
		expect(Action::CompleteInterrupt, s, "C: a never-suspended call must not be resumed");
		neverResumesUncaptured(s, "C: must never restore an uncaptured context");
	}

	// D: InterruptEnter arrives before the call begins processing (interrupt queued in
	// _pendingInterrupts, activated later): still no captured context.
	{
		State s;
		s.haveInterruptedCall = true;
		s.haveInterruptedContinuation = false;
		s.resumeContextCaptured = false;
		s.didSyscallReturnDuringInterrupt = false;
		expect(Action::CompleteInterrupt, s, "D: pre-processing interrupt must not resume a context");
		neverResumesUncaptured(s, "D: must never restore an uncaptured context");
	}

	// E: the syscall-return re-entry of a call that did finish during the interrupt.
	{
		State s;
		s.didSyscallReturnDuringInterrupt = true;
		s.haveInterruptedCall = true;
		s.resumeContextCaptured = true;
		expect(Action::CompleteInterrupt, s, "E: syscall-return re-entry must not resume again");
	}

	// F: stacked/repeated interrupt states must classify safely in every combination.
	{
		for (int mask = 0; mask < 16; ++mask) {
			State s;
			s.didSyscallReturnDuringInterrupt = (mask & 1) != 0;
			s.haveInterruptedContinuation = (mask & 2) != 0;
			s.haveInterruptedCall = (mask & 4) != 0;
			s.resumeContextCaptured = (mask & 8) != 0;
			neverResumesUncaptured(s, "F: no state combination may restore an uncaptured context");
		}
	}

	// The normal suspended-call behavior is unchanged by the additional case.
	{
		State before;
		before.haveInterruptedCall = true;
		before.resumeContextCaptured = true;
		require(DarlingServer::InterruptResume::classify(before) == Action::ResumeCapturedContext,
			"regression: a captured suspension must still be restored");
	}

	if (failures != 0) {
		std::cerr << failures << " interrupt/resume contract failure(s)\n";
		return 1;
	}

	std::cout << "interrupt/resume contract: OK\n";
	return 0;
}
