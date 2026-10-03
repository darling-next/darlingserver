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

}
