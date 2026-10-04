#pragma once

// Deterministic decision for the interrupt/resume boundary of a Thread.
//
// The server handles an interrupt in `Thread::_handleInterruptEnterForCurrentThread()` after an
// interrupt context has been pushed onto that thread's interrupt stack. The interrupted call (the
// call that was active when the interrupt was pushed) can be in more than one state, and the state
// MEASURED on the recovered Ring candidate is the one the old inline branch could not express:
//
//     interrupted call present
//     + no interrupted continuation captured
//     + no suspension context captured for that call
//
// Restoring a never-captured `ucontext_t` is undefined: glibc's setcontext() dereferences
// `uc_mcontext.fpregs`, which is NULL for a context that `getcontext()` never filled, so it faults.
// MEASURED: pc inside libc `setcontext+0x34` (`fldenv (%rcx)` after `mov 0xe0(%rdx),%rcx`),
// si_addr = 0x0, with `ret` inside `Thread::_handleInterruptEnterForCurrentThread` and
// `rdi = &Thread::_resumeContext`; the server then dies in its own crash probe.
//
// This header keeps the decision pure so a host test can drive every case without a running guest
// and without a scheduler: the product calls `classify()`, so the contract exercises the decision
// the server actually makes.
namespace DarlingServer::InterruptResume {

enum class Action {
	// Nothing to restore here: the interrupted call either finished (syscall-return re-entry) or
	// never suspended, so its own path owns it. Only interrupt bookkeeping remains.
	CompleteInterrupt,
	// The interrupted call left a continuation: run it.
	RunContinuation,
	// The interrupted call suspended and its context was captured: restore that context.
	ResumeCapturedContext,
};

struct State {
	// Set when control returned through `_syscallReturnHereDuringInterrupt`, i.e. the interrupted
	// call already ran to its syscall return during this interrupt.
	bool didSyscallReturnDuringInterrupt = false;
	// `_interruptedContinuation != nullptr`
	bool haveInterruptedContinuation = false;
	// `_interrupts.top().interruptedCall != nullptr`
	bool haveInterruptedCall = false;
	// `_resumeContext` was captured by a suspension of the interrupted call and has not been
	// consumed by a previous setcontext().
	bool resumeContextCaptured = false;
};

// What the interrupt path must do. `ResumeCapturedContext` is NEVER selected unless the suspension
// context was actually captured: a call that is present but never suspended is completed, not
// resumed.
Action classify(const State& state);

// True when the decision restores a captured suspension context (the contract's core invariant:
// this may only be true when `resumeContextCaptured` is true).
bool restoresCapturedContext(Action action);

// ---------------------------------------------------------------------------
// Ownership of the interrupted call, for the cleanup at the end of the interrupt path.
//
// The interrupt pushes the thread's active call into `_interrupts.top().interruptedCall` and takes
// the thread over; the interrupt call itself becomes `_activeCall` for the duration. When the
// interrupt ends, the record is retired. Three outcomes exist and each must neither lose, resume
// twice, nor complete twice the interrupted call:
//
//   * the interrupt RESUMED the call through its captured context -> the call runs again and its own
//     syscall-return re-entry ends here; retiring the record is the completion.
//   * the interrupt ran the call's continuation -> same: the record is retired here.
//   * the interrupt did NOT resume it (case C: the call was never suspended, e.g. a plane-serviced
//     call that answered its caller from the management page). Nothing in this path can complete it,
//     so retiring the record is only safe when the call no longer has a reply to deliver.
struct OwnershipState {
	bool resumed = false;          // the interrupt resumed the interrupted call (context or continuation)
	bool interruptedCall = false;  // `_interrupts.top().interruptedCall != nullptr`
	bool replySuppressed = false;  // that call's reply goes to the management page, not to a waiter
};

enum class Retirement {
	// Retire the record: the call is complete (resumed) or has no waiter left.
	Retire,
	// Keep ownership: nothing here may complete a call that still has a reply to deliver.
	Keep,
	// Nothing to retire.
	Nothing,
};

Retirement retireInterruptedCall(const OwnershipState& state);

// True when the state is only reachable if the implementation both retired and kept the call --
// the contract asserts this is never selected by `retireInterruptedCall`.
bool retirementIsAmbiguous(const OwnershipState& state);

// ---------------------------------------------------------------------------
// Setting a pending signal (Thread::setPendingSignal).
//
// MEASURED: an urgent-plane sigprocess on a thread with no interrupt marker made the server throw
// ("Can't set pending signal with no active interrupts"); the worker guard maps a plain exception to
// -EINVAL, the guest's sigexc handler aborted and boot died. The same class already treats this state
// as legal elsewhere: `processSignal()` documents that an empty interrupt stack only means there is
// no marker of this thread's to clear, and `pendingSignal()` answers 0 for it.
//
// The rule therefore never raises, and it is explicit about whether it mutated anything: a caller
// must be able to tell "there was no marker to update" from "the marker was updated". Losing the
// signal is NOT an outcome of this function -- the request is recorded by the caller's own pending-
// signal path; this only maintains the interrupt marker.
struct PendingSignalState {
	bool interruptMarker = false;  // `_interrupts` non-empty
	int currentPending = 0;        // `_interrupts.top().signal`
};

struct PendingSignalResult {
	bool raised = false;     // must never be true: the marker-less state is not an error
	bool applied = false;    // the marker was updated exactly once
	int previous = 0;        // the marker's previous value, or 0 when there was none
};

PendingSignalResult setPendingSignal(const PendingSignalState& state, int signal);

}
