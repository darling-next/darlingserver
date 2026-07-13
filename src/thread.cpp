/**
 * This file is part of Darling.
 *
 * Copyright (C) 2021 Darling developers
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Darling is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Darling.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "darlingserver/registry.hpp"
#include <darlingserver/thread.hpp>
#include <darlingserver/microthread-resume.hpp>
#include <darlingserver/processcall-guard.hpp>
#include <darlingserver/process.hpp>
#include <darlingserver/call.hpp>
#include <darlingserver/server.hpp>
#include <darlingserver/logging.hpp>
#include <darlingserver/metrics.hpp>
#include <darlingserver/test-diagnostics.hpp>
#ifdef DSERVER_RING_TRANSPORT
	#include <darlingserver/ring.hpp>
	#include <darlingserver/monitor.hpp>
#endif
#include <filesystem>
#include <fstream>

#include <sys/mman.h>
#include <signal.h>

#include <darlingserver/duct-tape.h>
#include <atomic>

#include <sys/syscall.h>
#include <time.h>

#if DSERVER_ASAN
	#include <sanitizer/asan_interface.h>
#endif

#include <rtsig.h>

#include <assert.h>

#include <limits>
#include <system_error>
#include <cerrno>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <vector>

// 64KiB should be enough for us
#define THREAD_STACK_SIZE (64 * 1024ULL)
#define USE_THREAD_GUARD_PAGES 1
#define IDLE_THREAD_STACK_COUNT 8

static thread_local std::shared_ptr<DarlingServer::Thread> currentThreadVar = nullptr;
static thread_local bool returningToThreadTop = false;
static thread_local ucontext_t backToThreadTopContext;
static thread_local libsimple_lock_t* unlockMeWhenSuspending = nullptr;
static thread_local std::function<void()> currentContinuation = nullptr;

/**
 * Our microthreads use cooperative multitasking, so we don't really use interrupts per-se.
 * Rather, this is an indication to our cooperative scheduler that the microthread is doing something and
 * expects to continue to have control of the executing thread. If it calls a function/method that
 * would cause it to relinquish control of the thread, this should be considered an error.
 *
 * This is primarily of use for debugging duct-tape code and ensuring certain assumptions made in the duct-tape code hold true.
 */
static thread_local uint64_t interruptDisableCount = 0;

#if DSERVER_ASAN
	static thread_local void* asanOldFakeStack = nullptr;
	static thread_local const void* asanOldStackBottom = nullptr;
	static thread_local size_t asanOldStackSize = 0;
#endif

static DarlingServer::Log threadLog("thread");

DarlingServer::StackPool DarlingServer::Thread::stackPool(IDLE_THREAD_STACK_COUNT, THREAD_STACK_SIZE, USE_THREAD_GUARD_PAGES);

DarlingServer::Thread::Thread(std::shared_ptr<Process> process, NSID nsid, void* stackHint):
	_nstid(nsid),
	_process(process)
{
	_tid = -1;

	for (const auto& entry: std::filesystem::directory_iterator("/proc/" + std::to_string(process->id()) + "/task")) {
		std::ifstream statusFile(entry.path() / "status");
		std::string line;

		while (std::getline(statusFile, line)) {
			if (line.substr(0, sizeof("NSpid") - 1) == "NSpid") {
				auto pos = line.find_last_of('\t');
				std::string id;

				if (pos != line.npos) {
					id = line.substr(pos + 1);
				}

				if (id.empty()) {
					throw std::runtime_error("Failed to parse thread ID");
				}

				if (std::stoi(id) != _nstid) {
					continue;
				}

				_tid = std::stoi(entry.path().filename().string());

				break;
			}
		}
	}

	// if we can't determine the thread id from procfs, try some other more costly methods.
	if (_tid == -1) {
		std::vector<pid_t> ids;
		auto& registry = threadRegistry();
		for (const auto& entry: std::filesystem::directory_iterator("/proc/" + std::to_string(process->id()) + "/task")) {
			pid_t currentId = std::stoi(entry.path().filename().string());
			// Skip threads that are already registered, as we're sure they're not the ones we want.
			if (registry.lookupEntryByID(currentId).has_value()) {
				continue;
			}
			ids.push_back(currentId);
		}

		// we're sure this is the thread we want as this is the only unregistered thread.
		if (ids.size() == 1) {
			_tid = ids[0];
		} else if (stackHint != nullptr) {
			pid_t chosenId = -1;
			intptr_t nearest = std::numeric_limits<intptr_t>::max();

			for (auto id : ids) {
				if (ptrace(PTRACE_ATTACH, id, 0, 0) == -1) {
					continue;
				}

				int status;
				int waitStatus = waitpid(id, &status, 0);

				if (waitStatus < 0) {
					continue;
				}

				struct user_regs_struct regs;
				if (ptrace(PTRACE_GETREGS, id, 0, &regs) == -1) {
					continue;
				}

#ifdef __x86_64__
				intptr_t stackDiff = (intptr_t)stackHint - (intptr_t)regs.rsp;
				if (stackDiff >= 0 && stackDiff < nearest) {
#else
	#warning Unsupported architecture
				if (true) {
#endif
					chosenId = id;
					nearest = stackDiff;
				}

				// this is critical: we're tracing a process but cannot detach from it, and it'll not run normally.
				if (ptrace(PTRACE_DETACH, id, 0, 0) == -1) {
					throw std::system_error(errno, std::generic_category(), "Failed to detach from process.");
				}
			}

			_tid = chosenId;
		}
	}

	if (_tid == -1) {
		throw std::system_error(ESRCH, std::generic_category(), "Failed to find thread ID within darlingserver's namespace");
	}

	// NOTE: it's okay to use raw `this` without a shared pointer because the duct-taped thread will always live for less time than this Thread instance
	_dtapeThread = dtape_thread_create(process->_dtapeTask, _nstid, this);
	_s2cPerformSempahore = dtape_semaphore_create(process->_dtapeTask, 1);
	_s2cReplySempahore = dtape_semaphore_create(process->_dtapeTask, 0);
	_s2cInterruptEnterSemaphore = dtape_semaphore_create(process->_dtapeTask, 0);
	_s2cInterruptExitSemaphore = dtape_semaphore_create(process->_dtapeTask, 0);

	threadLog.info() << "New thread created with ID " << _tid << " and NSID " << _nstid << " for process with ID " << (process ? process->id() : -1) << " and NSID " << (process ? process->nsid() : -1);
};

DarlingServer::Thread::Thread(KernelThreadConstructorTag tag):
	_tid(-1),
	_process(Process::kernelProcess())
{
	static uint64_t kernelThreadIDCounter = DTAPE_KERNEL_THREAD_ID_THRESHOLD;
	static std::mutex kernelThreadIDCounterLock;

	std::unique_lock idLock(kernelThreadIDCounterLock);
	_nstid = kernelThreadIDCounter++;
	if (kernelThreadIDCounter == 0) {
		kernelThreadIDCounter = DTAPE_KERNEL_THREAD_ID_THRESHOLD;
	}
	idLock.unlock();

	_dtapeThread = dtape_thread_create(Process::kernelProcess()->_dtapeTask, _nstid, this);
};

void DarlingServer::Thread::registerWithProcess() {
	std::unique_lock lock(_process->_rwlock);
	_process->_threads[_nstid] = shared_from_this();
};

DarlingServer::Thread::~Thread() noexcept(false) {
	threadLog.info() << *this << ": thread being destroyed" << threadLog.endLog;

	if (_stack.isValid()) {
		stackPool.free(_stack);
	}

	if (!_process) {
		return;
	}

	std::unique_lock lock(_process->_rwlock);
	auto it = _process->_threads.begin();
	while (it != _process->_threads.end()) {
		if (it->first == _nstid) {
			break;
		}
		++it;
	}
	if (it == _process->_threads.end()) {
		throw std::runtime_error("Thread was not registered with Process");
	}
	_process->_threads.erase(it);

	if (_process->_threads.empty()) {
		// if this was the last thread in the process, it has died, so unregister it.
		// this should already be handled by the process' pidfd monitor, but just in case, we also handle it here.
		lock.unlock();
		_process->notifyDead();
	}
};

DarlingServer::Thread::ID DarlingServer::Thread::id() const {
	return _tid;
};

DarlingServer::Thread::NSID DarlingServer::Thread::nsid() const {
	return _nstid;
};

DarlingServer::EternalID DarlingServer::Thread::eternalID() const {
	return _eid;
};

void DarlingServer::Thread::_setEternalID(EternalID eid) {
	_eid = eid;
};

std::shared_ptr<DarlingServer::Process> DarlingServer::Thread::process() const {
	return _process;
};

std::shared_ptr<DarlingServer::Call> DarlingServer::Thread::pendingCall() const {
	std::shared_lock lock(_rwlock);
	return _pendingCall;
};

void DarlingServer::Thread::setPendingCall(std::shared_ptr<Call> newPendingCall) {
	std::unique_lock lock(_rwlock);
	if (newPendingCall && _pendingCall) {
		if (newPendingCall->number() == Call::Number::InterruptEnter) {
			// InterruptEnter calls can occur after we receive a call but before we start processing it,
			// so we need to handle this case gracefully. we do so by saving the interrupt and scheduling
			// it to be processed once the pending call becomes active and suspends or exits.
			_pendingInterrupts.push(newPendingCall);
			return;
		} else {
			throw std::runtime_error("Thread's pending call overwritten while active");
		}
	}
	_pendingCall = newPendingCall;
};

std::shared_ptr<DarlingServer::Call> DarlingServer::Thread::activeCall() const {
	std::shared_lock lock(_rwlock);
	return _activeCall;
};

void DarlingServer::Thread::makePendingCallActive() {
	std::unique_lock lock(_rwlock);
	_activeCall = _pendingCall;
	_pendingCall = nullptr;
};

void DarlingServer::Thread::_deactivateCallLocked(std::shared_ptr<Call> expectedCall) {
	if ((_interruptedForSignal ? _interrupts.top().interruptedCall : _activeCall).get() != expectedCall.get()) {
		throw std::runtime_error("Upon deactivating the active call found active/interrupted call != expected call");
	}
	(_interruptedForSignal ? _interrupts.top().interruptedCall : _activeCall) = nullptr;
};

void DarlingServer::Thread::deactivateCall(std::shared_ptr<Call> expectedCall) {
	std::unique_lock lock(_rwlock);
	_deactivateCallLocked(expectedCall);
};

DarlingServer::Address DarlingServer::Thread::address() const {
	std::shared_lock lock(_rwlock);
	return _address;
};

void DarlingServer::Thread::setAddress(Address address) {
	std::unique_lock lock(_rwlock);
	_address = address;
};

void DarlingServer::Thread::setThreadHandles(uintptr_t pthreadHandle, uintptr_t dispatchQueueAddress) {
	dtape_thread_set_handles(_dtapeThread, pthreadHandle, dispatchQueueAddress);
};

bool DarlingServer::Thread::waitingForReply() const {
	std::shared_lock lock(_rwlock);
	return !!_activeCall;
};

/*
 * IMPORTANT
 * ===
 *
 * The way that microthread handling/switching is done here is... not pretty, to say the least.
 * The problem is that, in order to use XNU's Mach IPC code, we need a way to interrupt execution of a "thread" and then resume from the same point.
 * Actual threads are too heavyweight, so instead we use our own form of microthreads with cooperative multitasking.
 * Using actual threads for each managed thread would be much simpler and far less hacky, but far more resource-intensive.
 */

static const auto microthreadLog = DarlingServer::Log("microthread");

// this runs in the context of the microthread (i.e. with the microthread's stack active)
void DarlingServer::Thread::microthreadWorker() {
#if DSERVER_ASAN
	__sanitizer_finish_switch_fiber(asanOldFakeStack, &asanOldStackBottom, &asanOldStackSize);
	asanOldFakeStack = nullptr;
#endif

	currentContinuation = nullptr;
	currentThreadVar->makePendingCallActive();

	// A processCall() implementation may throw (e.g. std::system_error from a memory
	// operation on a guest that died concurrently, or std::runtime_error from the
	// exec-replacement path). This worker runs on a makecontext()-established fiber
	// stack and never returns normally (it always setcontext()s away below), so an
	// uncaught exception unwinding out of here is undefined behavior -> std::terminate
	// -> the whole darlingserver process dies, stranding every other guest with an
	// in-flight RPC blocked forever in recvmsg. Contain it: turn the failure into an
	// error reply when the call supports one, otherwise just log and drop it, and
	// continue the normal post-call flow so the server stays alive.
	// perf #0 (dar-dar6x4-perf-5dq.6): time how long this RPC takes to service and count
	// it. We measure from worker entry (== dequeue, since the worker runs immediately on
	// the same thread that took the item) to completion below.
	auto& _metrics = DarlingServer::Metrics::shared();
	uint64_t _callStartUs = DarlingServer::Metrics::nowMonoUs();
	auto _callNumber = currentThreadVar->_activeCall->number();

	int guardedReplyCode = DarlingServer::processCallBasicReplyCode([&]() {
		currentThreadVar->_activeCall->processCall();
	});
	if (guardedReplyCode < 0) {
		microthreadLog.error()
			<< "Uncaught exception from processCall (call "
			<< DarlingServer::Call::callNumberToString(currentThreadVar->_activeCall->number())
			<< "); replying " << guardedReplyCode << microthreadLog.endLog;
		try {
			currentThreadVar->_activeCall->sendBasicReply(guardedReplyCode);
		} catch (...) {
			// call has no basic reply (returns data); nothing more we can do but survive
		}
	}

	// perf #0: record service latency (worker entry -> completion).
	{
		uint64_t now = DarlingServer::Metrics::nowMonoUs();
		uint64_t serviceUs = (now >= _callStartUs) ? (now - _callStartUs) : 0;
		_metrics.rpcsServiced.fetch_add(1, std::memory_order_relaxed);
		_metrics.rpcLatency.record(serviceUs);
		// perf #9 (dar-dar6x4-perf-5dq.16): per-call-number breakdown so perf #7 can
		// see which RPC numbers dominate the guest's recvmsg-wait.
		_metrics.recordCall(static_cast<uint32_t>(_callNumber), serviceUs);
		// perf #18 D9 (dar-1il.4): heatmap. This is the GENERIC fiber worker, so used_fiber=true (the
		// call ran on a suspendable microthread -- a blocking/fiber op, NOT Tier-2-no-fiber-eligible).
		// transport comes from the sticky latch set during dispatch/servicing. perf #18 D14 (dar-1il.9):
		// caller-S2C is no longer a sticky per-thread bool consumed here -- it is attributed per-call at
		// _s2cPerform via Metrics::recordCallerS2cFor(activeCall), so there is no cross-op leak.
		_metrics.recordCallHeatmap(
			static_cast<uint32_t>(_callNumber), serviceUs,
			currentThreadVar->_heatmapCallWasRing ? DarlingServer::Metrics::CallTransport::Ring
			                                      : DarlingServer::Metrics::CallTransport::Uds,
			/* usedFiber */ true,
			/* didCallerS2c */ false);
		currentThreadVar->_heatmapCallWasRing = false;
		if (_callNumber == DarlingServer::Call::Number::Checkin) {
			_metrics.checkinLatency.record(serviceUs);
		}
	}

	if (currentThreadVar->_handlingInterruptedCall) {
		currentThreadVar->_didSyscallReturnDuringInterrupt = true;
#if DSERVER_ASAN
		__sanitizer_start_switch_fiber(NULL, currentThreadVar->_stack.base, currentThreadVar->_stack.size);
#endif
		setcontext(&currentThreadVar->_syscallReturnHereDuringInterrupt);
	} else {
#if DSERVER_ASAN
		// we're exiting normally, so we might not re-enter this microthread; tell ASAN to drop the fake stack
		__sanitizer_start_switch_fiber(NULL, asanOldStackBottom, asanOldStackSize);
#endif

		setcontext(&backToThreadTopContext);
	}
	__builtin_unreachable();
};

void DarlingServer::Thread::microthreadContinuation() {
#if DSERVER_ASAN
	__sanitizer_finish_switch_fiber(asanOldFakeStack, &asanOldStackBottom, &asanOldStackSize);
	asanOldFakeStack = nullptr;
#endif

	currentContinuation = currentThreadVar->_continuationCallback;
	// FIXME: we probably should never see `currentContinuation == nullptr`
	if (currentContinuation) {
		currentThreadVar->_continuationCallback = nullptr;
		currentContinuation();
		currentContinuation = nullptr;
	}

	if (currentThreadVar->_handlingInterruptedCall) {
		currentThreadVar->_didSyscallReturnDuringInterrupt = true;
#if DSERVER_ASAN
		__sanitizer_start_switch_fiber(NULL, currentThreadVar->_stack.base, currentThreadVar->_stack.size);
#endif
		setcontext(&currentThreadVar->_syscallReturnHereDuringInterrupt);
	} else {
#if DSERVER_ASAN
		// see microthreadWorker()
		__sanitizer_start_switch_fiber(NULL, asanOldStackBottom, asanOldStackSize);
#endif
		setcontext(&backToThreadTopContext);
	}
	__builtin_unreachable();
};

void DarlingServer::Thread::doWork() {
	// NOTE: this method MUST NOT use any local variables that require destructors.
	//       this method is actually major UB because the compiler is free to do whatever it likes with the stack,
	//       but we know what reasonable compilers (i.e. GCC and Clang) do with it and we're specifically targeting Clang, so it's okay for us.

	// perf#25a A0 (Part 3c): whether THIS dispatch consumed a genuine resume() wake permit.
	// Only such a dispatch (or a pending-call override) may resume a suspended context: every
	// legitimate waker goes through resume() (thread_unblock finalizes wait_result first), so a
	// dispatch that finds the thread suspended WITHOUT a permit is a stale re-run (e.g. an owed
	// _rerunPending whose wake was already consumed) and must not touch the parked wait --
	// resuming it would deliver a spurious wakeup with wait_result == THREAD_WAITING.
	bool hadResumePermit = false;
	bool preserveWaitState = false;

	_rwlock.lock();

	if (_deferralState != DeferralState::NotDeferred) {
		microthreadLog.debug() << _tid << "(" << _nstid << "): execution was deferred" << microthreadLog.endLog;
		_deferralState = DeferralState::DeferredPending;
		_rwlock.unlock();
		return;
	}

	if (_running) {
		// perf#25a A0: this dispatch was popped while the microthread is still
		// _running on another worker (it is mid suspend()/doneWorking transition;
		// _running is only cleared at the doneWorking tail). We cannot run it now,
		// but we MUST NOT silently drop it -- otherwise a wake that a waker
		// delivered via scheduleThread() (rather than via _resumePermit) is lost
		// forever and the microthread deadlocks. Record the owed re-run; the
		// doneWorking tail will reschedule exactly once after _running clears.
		_rerunPending = true;
		microthreadLog.debug() << _tid << "(" << _nstid << "): dispatch arrived while still running; deferring re-run" << microthreadLog.endLog;
		_rwlock.unlock();
		return;
	}

	if (_terminating) {
		goto doneWorking;
	}

	if (_dead && !_activeCall) {
		// should be impossible, since this should be handled in `notifyDead`, but just in case
		_terminating = true;
		goto doneWorking;
	}

	if (_suspended && _resumePermit) {
		// This execution was scheduled by resume(); consume that wake permit.
		hadResumePermit = microthreadConsumePendingResume(_resumePermit);
	}
	_running = true;
	currentThreadVar = shared_from_this();
	// perf#25a A0 (Part 3): an interrupt_enter that stacks onto an in-flight call must NOT run
	// dtape_thread_entering(). The interrupted call may be suspended on a waitq with TH_WAIT set
	// (e.g. fork_wait_for_child parked in waitForChildAfterFork's semaphore, or a psynch cv/mutex
	// wait), and entering's unconditional "entering => cannot be waiting" clear half-tears that
	// wait: dtape_thread_sigexc_enter's clear_wait_internal(THREAD_INTERRUPTED) then sees no
	// TH_WAIT, returns KERN_NOT_WAITING, and SKIPS the real teardown -- no waitq unlink, no
	// wait_result write, no wait-timer cancel. The thread stays linked on the waitq while its
	// microthread unwinds, so the next semaphore_signal/wakeup on that waitq is consumed by a
	// thread that is no longer waiting and the GENUINE waiter never wakes: zombie child + parent
	// parked forever in recvmsg (the brew reinstall / nestwait.c hang). With the Part 2a fix (no
	// wait_result pre-write) the same clobber is loud instead: the resumed continuation reads
	// wait_result == THREAD_WAITING and panics in semaphore_convert_wait_result (captured live on
	// nestwait ring-OFF). Preserving the wait state here lets clear_wait_internal perform the
	// full, correct abort exactly like XNU. Fresh calls (no in-flight call to stack onto) keep
	// the normal entering transition.
	preserveWaitState =
		// an interrupt_enter stacking onto an in-flight (possibly waitq-parked) call...
		(_pendingCall && _pendingCall->number() == Call::Number::InterruptEnter
			&& !_pendingCallOverride && (_activeCall || _suspended || _continuationCallback))
		// ...or ANY dispatch that resumes a suspended context (matches the resume branch
		// below) rather than starting a fresh call. A genuine wake already had
		// thread_unblock clear TH_WAIT and write wait_result; a stray wake (raw
		// mutex/condvar handoff crosstalk, stale re-run) left TH_WAIT set and the Part 3e
		// re-park guard in thread_block/thread_continuation_callback needs to SEE it --
		// entering's unconditional TH_WAIT clear here would launder the stray wake into a
		// spurious wait_result == THREAD_WAITING return (stranded waitq link -> "thread
		// already waiting" panic / silent lost wakeup). dtape_thread_entering is only for
		// fresh call dispatches, where the guest thread is by definition not blocked here.
		|| (_suspended && (_pendingCallOverride || !_pendingCall));
	if (!preserveWaitState) {
		dtape_thread_entering(_dtapeThread);
	}

	returningToThreadTop = false;
	_rwlock.unlock();

	_runningCondvar.notify_all();

	getcontext(&backToThreadTopContext);

	if (returningToThreadTop) {
		// someone jumped back to the top of the microthread
		// (that means either the microthread has been suspended or it has finished)

#if DSERVER_ASAN
		__sanitizer_finish_switch_fiber(asanOldFakeStack, &asanOldStackBottom, &asanOldStackSize);
		asanOldFakeStack = nullptr;
#endif

		_rwlock.lock();

		if (!_suspended || _continuationCallback) {
			// we discard the old stack when either:
			//   * we exit normally (i.e. without suspending); this includes syscall returns.
			//   * or when we suspend with a continuation callback.
			stackPool.free(_stack);
		}

		//microthreadLog.debug() << _tid << "(" << _nstid << "): microthread returned to top" << microthreadLog.endLog;
		goto doneWorking;
	} else {
		returningToThreadTop = true;

		_rwlock.lock();

		if (!_pendingCallOverride && _pendingCall && _pendingCall->number() == Call::Number::InterruptEnter) {
			_interrupts.emplace();
			_interrupts.top().savedStack = _stack;
			_stack = StackPool::Stack();
			_interruptedContinuation = _continuationCallback;
			_continuationCallback = nullptr;
			_interrupts.top().interruptedCall = _activeCall;
			_activeCall = nullptr;
		}

		if (_continuationCallback && _pendingCall) {
			// we can only have one of the two
			throw std::runtime_error("Thread has both a pending call and a pending continuation");
		}

		// perf#25a A0 (Part 3c): resume the suspended context only for a dispatch that carried a
		// real wake (permit consumed above) or an explicit override; a permit-less dispatch of a
		// suspended thread is stale and falls through to the else-branch, which parks it again
		// (no pending call -> doneWorking) instead of spuriously resuming a live wait.
		if (_suspended && (_pendingCallOverride || (!_pendingCall && hadResumePermit))) {
			if (_pendingCallOverride) {
				microthreadLog.info() << _tid << "(" << _nstid << "): thread was suspended with a pending call override and is now resuming with a pending call" << microthreadLog.endLog;
			}
			// we were in the middle of processing a call and we need to resume now
			_suspended = false;
			_resumeContext.uc_link = &backToThreadTopContext;
			_rwlock.unlock();

			if (_continuationCallback) {
				// for continuations, we discard the old stack and start with a new one
				assert(!_stack.isValid());
				stackPool.allocate(_stack);

				// we also ahve to set up the resume context properly with the new stack
				_resumeContext.uc_stack.ss_sp = _stack.base;
				_resumeContext.uc_stack.ss_size = _stack.size;
				_resumeContext.uc_stack.ss_flags = 0;
				_resumeContext.uc_link = &backToThreadTopContext;
				makecontext(&_resumeContext, microthreadContinuation, 0);
			} else {
				// otherwise, we expect to have a valid stack to continue where we left off
				assert(_stack.isValid());
			}

#if DSERVER_ASAN
			__sanitizer_start_switch_fiber(&asanOldFakeStack, _stack.base, _stack.size);
#endif

			setcontext(&_resumeContext);
		} else {
			if (!_pendingCall) {
				// if we don't actually have a pending call, we have nothing to do
				goto doneWorking;
			}
			_suspended = false;
			_rwlock.unlock();

			// we might've had a valid stack if we're overwriting a previous suspension, so handle that.
			if (_stack.isValid()) {
				stackPool.free(_stack);
			}

			stackPool.allocate(_stack);

			ucontext_t newContext;
			getcontext(&newContext);
			newContext.uc_stack.ss_sp = _stack.base;
			newContext.uc_stack.ss_size = _stack.size;
			newContext.uc_stack.ss_flags = 0;
			newContext.uc_link = &backToThreadTopContext;
			makecontext(&newContext, microthreadWorker, 0);

#if DSERVER_ASAN
			__sanitizer_start_switch_fiber(&asanOldFakeStack, _stack.base, _stack.size);
#endif

			setcontext(&newContext);
		}

		// inform the compiler that it shouldn't do anything that would live past the setcontext calls
		__builtin_unreachable();
	}

doneWorking:
	// we must be holding `_rwlock` when we get here
	if (_running) {
		dtape_thread_exiting(_dtapeThread);
		currentThreadVar = nullptr;
		_running = false;
	}
	// A wake can arrive after suspend()'s final permit check but before it
	// physically switches back here. Now that _running is false, rescheduling
	// is safe and cannot race another worker running this microthread.
	//
	// perf#25a A0: also honor a dispatch that a worker popped and deferred while
	// we were still _running (doWork() set _rerunPending instead of dropping it).
	// Now that _running is false it is safe to reschedule; consume the flag
	// exactly once. _resumePermit still uses the shared helper contract from the
	// homebrew fix; _rerunPending is the separate dropped-dispatch channel and
	// is intentionally not gated on _suspended.
	bool rerunPending = _rerunPending;
	_rerunPending = false;
	bool resumeAfterWorking =
		microthreadShouldScheduleAfterStop(_resumePermit, _suspended, _terminating, _dead) ||
		(rerunPending && !_terminating && !_dead);
	bool canRelease = false;
	if (_dead) {
		threadLog.debug() << *this << ": dead thread returning. active call? " << (!!_activeCall ? "true" : "false") << " terminating? " << (_terminating ? "true" : "false") << threadLog.endLog;
	}
	if (_dead && !_activeCall && !_terminating) {
		// this is the case when `notifyDead` notified us we were dead
		// but we had an active call and had to finish it first
		_terminating = true;
		canRelease = true;
	}
	if (_terminating && !_dead) {
		// this will not destroy our thread immediately;
		// the worker thread invoker still holds a reference on us
		_rwlock.unlock();
		notifyDead();
	} else {
		// if we have any pending interrupts, schedule them to be processed now
		// (we've just finished or suspended a call, so now's the time to handle interrupts)
		if (!_terminating && !_dead && !_pendingInterrupts.empty()) {
			if (_pendingCall) {
				throw std::runtime_error("Need to schedule interrupt for processing, but thread has pending call");
			}

			_pendingCall = _pendingInterrupts.front();
			_pendingInterrupts.pop();

			Server::sharedInstance().scheduleThread(shared_from_this());
		}

		_rwlock.unlock();
	}
	if (unlockMeWhenSuspending) {
		libsimple_lock_unlock(unlockMeWhenSuspending);
		unlockMeWhenSuspending = nullptr;
	}
	_runningCondvar.notify_all();
	if (resumeAfterWorking) {
		Server::sharedInstance().scheduleThread(shared_from_this());
	}
	if (canRelease) {
		_scheduleRelease();
	}
	return;
};

#ifdef DSERVER_RING_TRANSPORT
bool DarlingServer::Thread::doWorkInline() {
	// perf #18 P6.1 (dar-ohp): run a proven-non-blocking Call without the microthread fiber.
	// This is doWork() with the fiber stripped: same guards, same context-enter (so current_task
	// resolves identically), same processCall + exception containment + metrics as
	// microthreadWorker(), then the same essential completion cleanup as doneWorking -- but the
	// call runs on the CURRENT (main-loop) stack instead of a makecontext fiber.
	//
	// SAFETY: the caller guarantees the call never suspends. We therefore never touch the fiber
	// state (backToThreadTopContext / _resumeContext / _stack) and assert it stays non-suspended.
	std::unique_lock<std::shared_mutex> lock(_rwlock);

	if (_deferralState != DeferralState::NotDeferred) {
		_deferralState = DeferralState::DeferredPending;
		return false;
	}
	if (_running) {
		microthreadLog.warning() << _tid << "(" << _nstid << "): doWorkInline on already-running microthread" << microthreadLog.endLog;
		return false;
	}
	if (_terminating || (_dead && !_activeCall) || _suspended || _continuationCallback || !_pendingCall) {
		// any of these means this is NOT the simple "fresh non-blocking call" case the inline
		// path is for; let the caller fall back to the full doWork() which handles them.
		return false;
	}

	_running = true;
	currentThreadVar = shared_from_this();
	dtape_thread_entering(_dtapeThread);
	_suspended = false;
	lock.unlock();
	_runningCondvar.notify_all();

	// --- body: mirror microthreadWorker() without the fiber-return setcontext ---------------
	makePendingCallActive(); // moves _pendingCall -> _activeCall (same as the fiber path)

	auto& _metrics = DarlingServer::Metrics::shared();
	uint64_t _callStartUs = DarlingServer::Metrics::nowMonoUs();
	auto _callNumber = _activeCall->number();

	try {
		_activeCall->processCall();
	} catch (const std::system_error& err) {
		microthreadLog.error() << "Uncaught std::system_error from inline processCall ("
			<< DarlingServer::Call::callNumberToString(_callNumber) << "): " << err.what()
			<< " (code " << err.code().value() << ")" << microthreadLog.endLog;
		try { _activeCall->sendBasicReply(-err.code().value()); } catch (...) {}
	} catch (const std::exception& ex) {
		microthreadLog.error() << "Uncaught exception from inline processCall ("
			<< DarlingServer::Call::callNumberToString(_callNumber) << "): " << ex.what() << microthreadLog.endLog;
		try { _activeCall->sendBasicReply(-EINVAL); } catch (...) {}
	} catch (...) {
		microthreadLog.error() << "Uncaught non-std exception from inline processCall ("
			<< DarlingServer::Call::callNumberToString(_callNumber) << ")" << microthreadLog.endLog;
		try { _activeCall->sendBasicReply(-EINVAL); } catch (...) {}
	}

	{
		uint64_t now = DarlingServer::Metrics::nowMonoUs();
		uint64_t serviceUs = (now >= _callStartUs) ? (now - _callStartUs) : 0;
		_metrics.rpcsServiced.fetch_add(1, std::memory_order_relaxed);
		_metrics.rpcLatency.record(serviceUs);
		_metrics.recordCall(static_cast<uint32_t>(_callNumber), serviceUs);
		// perf #18 D9 (dar-1il.4): heatmap. This is doWorkInline -- the Tier-2 NO-FIBER ring path. By
		// construction it ran a ring-originated allowlisted op without suspending (used_fiber=false); a
		// real suspend here is a contract violation handled below. transport=ring. perf #18 D14
		// (dar-1il.9): caller-S2C attributed per-call at _s2cPerform, not via a sticky latch here.
		_metrics.recordCallHeatmap(
			static_cast<uint32_t>(_callNumber), serviceUs,
			DarlingServer::Metrics::CallTransport::Ring,
			/* usedFiber */ false,
			/* didCallerS2c */ false);
		_heatmapCallWasRing = false;
	}

	// processCall() on a non-blocking op must have run to completion. If somehow it suspended
	// (a misclassified op), that is a contract violation -> we'd have corrupted the fiber model.
	// Detect it loudly rather than silently mishandle.
	bool canRelease = false;
	{
		std::unique_lock<std::shared_mutex> relock(_rwlock);
		if (_suspended) {
			// Should be impossible for an allowlisted op. The microthread "suspended" without a
			// fiber to resume onto -> we cannot honor it. Log; leave _running cleared so the
			// thread isn't wedged. (The guest will time out on this op and UDS-fall-back.)
			microthreadLog.error() << *this << ": doWorkInline call suspended -- not fast-path eligible!" << microthreadLog.endLog;
			DarlingServer::Metrics::shared().ringFastSuspend.fetch_add(1, std::memory_order_relaxed);
			_suspended = false;
		}
		_activeCall = nullptr;
		dtape_thread_exiting(_dtapeThread);
		currentThreadVar = nullptr;
		_running = false;

		if (_dead && !_activeCall && !_terminating) {
			_terminating = true;
			canRelease = true;
		}
		if (_terminating && !_dead) {
			relock.unlock();
			notifyDead();
		} else {
			if (!_terminating && !_dead && !_pendingInterrupts.empty()) {
				if (!_pendingCall) {
					_pendingCall = _pendingInterrupts.front();
					_pendingInterrupts.pop();
					Server::sharedInstance().scheduleThread(shared_from_this());
				}
			}
			relock.unlock();
		}
	}
	_runningCondvar.notify_all();
	if (canRelease) {
		_scheduleRelease();
	}
	return true;
};

bool DarlingServer::Thread::doMachReplyPortInline(uint32_t seq) {
	// perf #18 P6.1 step 2 (dar-ohp): the surgical one-op path. This is doWorkInline() with the
	// Call/Message framing ALSO removed: no _pendingCall, no processCall(), no callFromMessage --
	// we run the bare dtape primitive and publish the reply onto the ring ourselves. The duct-tape
	// context setup/teardown is IDENTICAL to doWorkInline (so current_task() resolves to this
	// thread's space exactly as MachReplyPort::processCall would see it).
	auto ring = _ring;
	if (!ring) {
		return false; // caller falls back to the generic path
	}

	std::unique_lock<std::shared_mutex> lock(_rwlock);

	if (_deferralState != DeferralState::NotDeferred) {
		_deferralState = DeferralState::DeferredPending;
		return false;
	}
	if (_running) {
		microthreadLog.warning() << _tid << "(" << _nstid << "): doMachReplyPortInline on already-running microthread" << microthreadLog.endLog;
		return false;
	}
	if (_terminating || _dead || _suspended || _continuationCallback || _pendingCall || _activeCall) {
		// Not the simple "fresh, idle thread servicing a no-arg trap" case. There is no Call to run
		// here (we bypass callFromMessage), so a _pendingCall/_activeCall would be left dangling --
		// decline and let the caller take the generic step-1 path which handles all of these.
		return false;
	}

	_running = true;
	currentThreadVar = shared_from_this();
	dtape_thread_entering(_dtapeThread);
	_suspended = false;
	lock.unlock();
	_runningCondvar.notify_all();

	// --- body: the bare Mach primitive, identical to what MachReplyPort::processCall calls ------
	auto& _metrics = DarlingServer::Metrics::shared();
	uint64_t _callStartUs = DarlingServer::Metrics::nowMonoUs();
	uint32_t port = dtape_mach_reply_port();

	// publish {replyhdr.code=0}{uint32 port_name} straight onto the s2c ring + wake the guest.
	// Byte-identical to the reply the generic path produces via pushCallReply for this op.
#ifdef DSERVER_RING_PHASE_PROF
	uint64_t _pubT0 = Metrics::rdtscCycles();
#endif
	bool published = ring->publishReply(seq, static_cast<uint32_t>(dserver_callnum_mach_reply_port), 0, &port, sizeof(port));
#ifdef DSERVER_RING_PHASE_PROF
	_ringPublishCycles = Metrics::rdtscCycles() - _pubT0;
#endif
	if (published) {
		ring->wakeGuest();
	} else {
		// s2c ring full: the reply will be sent via UDS below (no double-mint). Account it so a
		// nonzero ring_s2c_full under load flags a guest that isn't draining its s2c ring.
		Metrics::shared().ringS2cFull.fetch_add(1, std::memory_order_relaxed);
	}

	{
		uint64_t now = DarlingServer::Metrics::nowMonoUs();
		uint64_t serviceUs = (now >= _callStartUs) ? (now - _callStartUs) : 0;
		_metrics.rpcsServiced.fetch_add(1, std::memory_order_relaxed);
		_metrics.rpcLatency.record(serviceUs);
		_metrics.recordCall(static_cast<uint32_t>(dserver_callnum_mach_reply_port), serviceUs);
		// perf #18 D9 (dar-1il.4): heatmap. The surgical mach_reply_port path: ring transport, no fiber,
		// pure mint (never an S2C). It bypasses beginRingReply/processCall, so pass the facts directly.
		_metrics.recordCallHeatmap(
			static_cast<uint32_t>(dserver_callnum_mach_reply_port), serviceUs,
			DarlingServer::Metrics::CallTransport::Ring,
			/* usedFiber */ false, /* didCallerS2c */ false);
	}

	// --- completion cleanup: mirror doWorkInline's (no fiber, no _activeCall ever set) ----------
	bool canRelease = false;
	{
		std::unique_lock<std::shared_mutex> relock(_rwlock);
		if (_suspended) {
			// Impossible for mach_reply_port (it never blocks). Log loudly; clear so we don't wedge.
			microthreadLog.error() << *this << ": doMachReplyPortInline suspended -- mach_reply_port must never block!" << microthreadLog.endLog;
			DarlingServer::Metrics::shared().ringFastSuspend.fetch_add(1, std::memory_order_relaxed);
			_suspended = false;
		}
		dtape_thread_exiting(_dtapeThread);
		currentThreadVar = nullptr;
		_running = false;

		if (_dead && !_activeCall && !_terminating) {
			_terminating = true;
			canRelease = true;
		}
		if (_terminating && !_dead) {
			relock.unlock();
			notifyDead();
		} else {
			if (!_terminating && !_dead && !_pendingInterrupts.empty()) {
				if (!_pendingCall) {
					_pendingCall = _pendingInterrupts.front();
					_pendingInterrupts.pop();
					Server::sharedInstance().scheduleThread(shared_from_this());
				}
			}
			relock.unlock();
		}
	}
	_runningCondvar.notify_all();
	if (canRelease) {
		_scheduleRelease();
	}

	// If the publish failed (s2c full), tell the caller to fall back so the guest still gets a
	// reply via the generic path. The dtape trap already ran (it minted a real port); re-running
	// it via callFromMessage would leak that port. So instead of "return false to re-dispatch",
	// we treat a publish failure as a HARD inline failure only when nothing was minted. Here the
	// port WAS minted and the only loss is the wake; publishReply already UDS-falls-back inside
	// pushCallReply for the generic path, but we don't have that here. Simplest correct choice:
	// if publish failed, send the reply via UDS directly so we never double-mint.
	if (!published) {
		// Build the minimal UDS reply and send it. Reuse the same reply convention.
		dserver_rpc_reply_mach_reply_port_t reply;
		reply.header.number = dserver_callnum_mach_reply_port;
		reply.header.code = 0;
		reply.body.port_name = port;
		Message replyMsg(sizeof(reply), 0);
		replyMsg.data().resize(sizeof(reply));
		memcpy(replyMsg.data().data(), &reply, sizeof(reply));
		replyMsg.setAddress(_address);
		Server::sharedInstance().sendMessage(std::move(replyMsg));
	}
	return true;
};
#endif

void DarlingServer::Thread::suspend(std::function<void()> continuationCallback, libsimple_lock_t* unlockMe) {
	if (this != currentThreadVar.get()) {
		throw std::runtime_error("Attempt to suspend thread other than current thread");
	}

	if (interruptDisableCount > 0) {
		throw std::runtime_error("Attempt to suspend thread while interrupts disabled");
	}

	_rwlock.lock();
	// Consume a wake that arrived before suspend() marked us suspended.
	if (microthreadConsumePendingResume(_resumePermit)) {
		TestDiagnostics::traceLine("microthread.suspend.consume_pending_resume");
		_rwlock.unlock();
		if (unlockMe) {
			libsimple_lock_unlock(unlockMe);
		}
		return;
	}
	_suspended = true;
	_rwlock.unlock();

	unlockMeWhenSuspending = unlockMe;

	getcontext(&_resumeContext);

	_rwlock.lock();
	// Consume a wake that arrived while the resume context was being captured.
	if (microthreadConsumeResumeDuringSuspend(_suspended, _resumePermit)) {
		TestDiagnostics::traceLine("microthread.suspend.consume_resume_during_suspend");
		_rwlock.unlock();
		if (unlockMeWhenSuspending) {
			libsimple_lock_unlock(unlockMeWhenSuspending);
			unlockMeWhenSuspending = nullptr;
		}
		return;
	}
	if (_suspended) {
		if (continuationCallback) {
			// when suspendeding with a continuation, the current continuation and call are discarded (since they can no longer be safely returned to)
			currentContinuation = nullptr;

			_continuationCallback = continuationCallback;
		}
		// jump back to the top of the microthread
		_rwlock.unlock();

#if DSERVER_ASAN
		// if we have a continuation, we don't expect to come back here
		__sanitizer_start_switch_fiber((continuationCallback) ? nullptr : &asanOldFakeStack, asanOldStackBottom, asanOldStackSize);
#endif

		setcontext(&backToThreadTopContext);
		__builtin_unreachable();
	} else {
		// we've been resumed

		// make sure we don't have a continuation when we get here;
		// if we do, that means that doWork() failed to do its job for the continuation case
		assert(!_continuationCallback);

		_rwlock.unlock();

#if DSERVER_ASAN
		__sanitizer_finish_switch_fiber(asanOldFakeStack, &asanOldStackBottom, &asanOldStackSize);
		asanOldFakeStack = nullptr;
#endif
	}
};

void DarlingServer::Thread::resume() {
	bool schedule = false;
	{
		std::unique_lock lock(_rwlock);
		// Coalesce repeated wakes into one permit. If the microthread is still
		// running, it will either consume the permit in suspend() or reschedule
		// itself from doWork() after physically stopping.
		schedule = microthreadRecordResume(_running, _suspended, _resumePermit);
	}

	if (schedule) {
		Server::sharedInstance().scheduleThread(shared_from_this());
	}
};

void DarlingServer::Thread::clearResumePermit() {
	// perf#25a A0 (Part 3d): called by duct-tape's thread_block_parameter when the wait was
	// finalized (thread_unblock ran: wait_result written, TH_WAIT cleared) BEFORE the microthread
	// physically suspended -- thread_block skips the suspension, so the wake permit resume()
	// minted for that unblock is already satisfied. Left set, it would go stale and let the
	// thread's NEXT suspend() return immediately with wait_result still THREAD_WAITING (panics
	// semaphore_convert_wait_result; corrupts other wait protocols silently).
	std::unique_lock lock(_rwlock);
	_resumePermit = false;
};

void DarlingServer::Thread::terminate() {
	if (_process) {
		if (_process.get() != Process::kernelProcess().get()) {
			throw std::runtime_error("terminate() called on non-kernel thread");
		}
	} else {
		throw std::runtime_error("terminate() called on non-kernel thread");
	}

	_rwlock.lock();
	_terminating = true;

	if (currentThreadVar.get() == this) {
		// if it's the current thread, just suspend it;
		// when we return to the "top" of the microthread,
		// doWork() will see that it's terminating and clean up
		_rwlock.unlock();
		suspend();
		throw std::runtime_error("terminate() on current kernel thread returned");
	} else {
		// if it's not the current thread and it's not currently running, just tell it died;
		// it should die once the caller releases their reference(s) on us
		if (!_running) {
			_rwlock.unlock();
			notifyDead();
		} else {
			// otherwise, if it IS running, once it returns to the microthread "top" and sees `_terminating = true`, it'll unregister itself
			_rwlock.unlock();
		}
	}
};

std::shared_ptr<DarlingServer::Thread> DarlingServer::Thread::currentThread() {
	return currentThreadVar;
};

void DarlingServer::Thread::setupKernelThread(std::function<void()> startupCallback) {
	std::unique_lock lock(_rwlock);
	_continuationCallback = startupCallback;
	_suspended = true;
	getcontext(&_resumeContext);
};

void DarlingServer::Thread::startKernelThread(std::function<void()> startupCallback) {
	setupKernelThread(startupCallback);
	resume();
};

void DarlingServer::Thread::impersonate(std::shared_ptr<Thread> thread) {
	std::shared_ptr<Thread> oldThread;

	if (thread) {
		// prevent the thread from running while we're impersonating it
		// FIXME: this may lead to blocking natively while we're on a microthread.
		//        we would prefer to block using duct-taped facilities instead.
		{
			std::unique_lock lock(thread->_rwlock);
			thread->_deferLocked(true, lock);
			thread->_running = true;
		}
		thread->_runningCondvar.notify_all();
	}

	{
		std::unique_lock lock(_rwlock);
		oldThread = _impersonating;
		_impersonating = thread;
	}

	if (oldThread) {
		{
			std::unique_lock lock(oldThread->_rwlock);
			oldThread->_running = false;
			oldThread->_undeferLocked(lock);
		}
		oldThread->_runningCondvar.notify_all();
	}
};

std::shared_ptr<DarlingServer::Thread> DarlingServer::Thread::impersonatingThread() const {
	std::shared_lock lock(_rwlock);
	return _impersonating;
};

void DarlingServer::Thread::interruptDisable() {
	++interruptDisableCount;
};

void DarlingServer::Thread::interruptEnable() {
	if (interruptDisableCount-- == 0) {
		throw std::runtime_error("interruptEnable() called when already enabled");
	}
};

void DarlingServer::Thread::syscallReturn(int resultCode) {
	if (!currentThreadVar) {
		throw std::runtime_error("syscallReturn() called with no current thread");
	}

	{
		auto call = (currentThreadVar->_interruptedForSignal) ? currentThreadVar->_interrupts.top().interruptedCall : currentThreadVar->_activeCall;
		if (!call || !call->isXNUTrap()) {
			throw std::runtime_error("Attempt to return from syscall on thread with no active syscall");
		}
		if (call->isBSDTrap()) {
			call->sendBSDReply(resultCode, currentThreadVar->_bsdReturnValue);
		} else {
			call->sendBasicReply(resultCode);
		}
	}

	if (currentThreadVar->_interruptedForSignal) {
		currentThreadVar->_didSyscallReturnDuringInterrupt = true;
#if DSERVER_ASAN
		if (currentThreadVar->_handlingInterruptedCall) {
			__sanitizer_start_switch_fiber(nullptr, currentThreadVar->_stack.base, currentThreadVar->_stack.size);
		}
#endif
		setcontext(&currentThreadVar->_syscallReturnHereDuringInterrupt);
		__builtin_unreachable();
	}

	// jump back to the top of the thread
#if DSERVER_ASAN
	__sanitizer_start_switch_fiber(nullptr, asanOldStackBottom, asanOldStackSize);
#endif
	setcontext(&backToThreadTopContext);
	__builtin_unreachable();
};

static std::queue<std::function<void()>> kernelAsyncRunnerQueue;

// we have to use a regular lock here because it needs to be lockable from both a microthread and normal thread context.
// additionally, it's only locked for brief periods.
//
// we use libsimple_lock_t so we can pass it to `suspend` to unlock it after suspending.
// XXX: we could use a std::mutex if we add an overload to `suspend` for it.
static libsimple_lock_t kernelAsyncRunnerQueueLock;
static dtape_semaphore_t* kernelAsyncRunnerQueueSempahore = nullptr;
static uint64_t kernelAsyncRunnersAvailable = 0;
static std::vector<std::shared_ptr<DarlingServer::Thread>> permanentKernelAsyncRunners;

#define MAX_PERMANENT_KERNEL_RUNNERS 10

static void kernelAsyncRunnerThreadWorker(bool permanent, std::shared_ptr<DarlingServer::Thread> self) {
	do {
		// we're going to wait for work; we're available now.
		libsimple_lock_lock(&kernelAsyncRunnerQueueLock);
		++kernelAsyncRunnersAvailable;
		libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

		if (!dtape_semaphore_down_simple(kernelAsyncRunnerQueueSempahore)) {
			// we were interrupted. go again if we're permanent; otherwise, die.
			libsimple_lock_lock(&kernelAsyncRunnerQueueLock);
			--kernelAsyncRunnersAvailable;
			libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

			if (permanent) {
				continue;
			} else {
				break;
			}
		}

		libsimple_lock_lock(&kernelAsyncRunnerQueueLock);

		if (kernelAsyncRunnerQueue.empty()) {
			// we didn't find any work (we were probably awoken spuriously).
			// go again if we're permanent; otherwise, die.
			--kernelAsyncRunnersAvailable;
			libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

			if (permanent) {
				continue;
			} else {
				break;
			}
		}

		// we're going to perform some work; we're no longer available
		--kernelAsyncRunnersAvailable;

		auto func = kernelAsyncRunnerQueue.front();
		kernelAsyncRunnerQueue.pop();

		libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

		// perform the work
		func();
	} while (permanent);

	self = nullptr;
	DarlingServer::Thread::currentThread()->terminate();
	__builtin_unreachable();
};

void DarlingServer::Thread::kernelAsync(std::function<void()> fn) {
	static bool inited = []() {
		kernelAsyncRunnerQueueSempahore = dtape_semaphore_create(Process::kernelProcess()->_dtapeTask, 0);
		return true;
	}();

	libsimple_lock_lock(&kernelAsyncRunnerQueueLock);
	kernelAsyncRunnerQueue.push(fn);
	if (kernelAsyncRunnersAvailable == 0) {
		// we need to get some work done, but there are no workers available.
		// if we have less workers than the max permanent number of workers,
		// let's spawn a permanent worker. otherwise, just spawn a temporary worker.
		auto thread = std::make_shared<Thread>(KernelThreadConstructorTag());
		auto permanent = permanentKernelAsyncRunners.size() < MAX_PERMANENT_KERNEL_RUNNERS;
		thread->startKernelThread(std::bind(kernelAsyncRunnerThreadWorker, permanent, thread));
		if (permanent) {
			permanentKernelAsyncRunners.push_back(std::move(thread));
		}
	}
	libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

	// increment the semaphore to let workers know there's work available.
	dtape_semaphore_up(kernelAsyncRunnerQueueSempahore);
};

void DarlingServer::Thread::kernelSync(std::function<void()> fn) {
	std::mutex mutex;
	std::condition_variable condvar;
	bool done = false;

	kernelAsync([&]() {
		fn();

		{
			std::unique_lock lock2(mutex);
			done = true;
		}

		// notify all, but there should only be one thread waiting
		condvar.notify_all();
	});

	{
		std::unique_lock lock(mutex);
		condvar.wait(lock, [&]() {
			return done;
		});
	}
};

std::shared_ptr<DarlingServer::Thread> DarlingServer::Thread::threadForPort(uint32_t thread_port) {
	// prevent the target thread from dying by taking the global thread registry lock
	auto registryLock = threadRegistry().scopedLock();

	dtape_thread_t* thread_handle = dtape_thread_for_port(thread_port);
	if (!thread_handle) {
		return nullptr;
	}

	Thread* thread = static_cast<Thread*>(dtape_thread_context(thread_handle));
	if (!thread) {
		return nullptr;
	}

	return thread->shared_from_this();
};

void DarlingServer::Thread::loadStateFromUser(uint64_t threadState, uint64_t floatState) {
	int ret = dtape_thread_load_state_from_user(_dtapeThread, threadState, floatState);
	if (ret != 0) {
		throw std::system_error(-ret, std::generic_category());
	}
};

void DarlingServer::Thread::saveStateToUser(uint64_t threadState, uint64_t floatState) {
	int ret = dtape_thread_save_state_to_user(_dtapeThread, threadState, floatState);
	if (ret != 0) {
		throw std::system_error(-ret, std::generic_category());
	}
};

int DarlingServer::Thread::pendingSignal() const {
	std::shared_lock lock(_rwlock);
	return (_interrupts.empty()) ? 0 : _interrupts.top().signal;
};

int DarlingServer::Thread::setPendingSignal(int signal) {
	std::unique_lock lock(_rwlock);
	int pendingSignal;
	if (_interrupts.empty()) {
		throw std::runtime_error("Can't set pending signal with no active interrupts");
	} else {
		pendingSignal = _interrupts.top().signal;
		_interrupts.top().signal = signal;
	}
	return pendingSignal;
};

void DarlingServer::Thread::processSignal(int bsdSignalNumber, int linuxSignalNumber, int code, uintptr_t signalAddress, uintptr_t threadStateAddress, uintptr_t floatStateAddress) {
	loadStateFromUser(threadStateAddress, floatStateAddress);

	{
		std::unique_lock lock(_rwlock);
		_interrupts.top().signal = 0;
		_processingSignal = true;
	}

	dtape_thread_process_signal(_dtapeThread, bsdSignalNumber, linuxSignalNumber, code, signalAddress);

	// LLDB commonly suspends the thread upon reception of an exception and assumes
	// that the thread will stay suspended after replying to the exception message,
	// until thread_resume() is called.
	dtape_thread_wait_while_user_suspended(_dtapeThread);

	{
		std::unique_lock lock(_rwlock);
		_processingSignal = false;
	}

	saveStateToUser(threadStateAddress, floatStateAddress);
};

void DarlingServer::Thread::handleSignal(int signal) {
	std::unique_lock lock(_rwlock);
	if (_processingSignal) {
		_interrupts.top().signal = signal;
	} else {
		throw std::runtime_error("Attempt to handle signal while not processing signal");
	}
};

void DarlingServer::Thread::setPendingCallOverride(bool pendingCallOverride) {
	std::unique_lock lock(_rwlock);
	_pendingCallOverride = pendingCallOverride;
};

/*
 * server-to-client (S2C) calls are used by darlingserver to invoke certain functions within managed processes
 * for which there is no in-server alterative.
 *
 * for example, memory allocation can only be done by the managed process itself; there is no Linux syscall to allocate memory in another process.
 * therefore, we have to ask the process to do it for us.
 *
 * an alternative to this system is ptrace. we can attach to the managed process and execute any function we like.
 * this is made even easier by the fact that we have our own code in the managed process, meaning we can help out the server by
 * providing thunks for it to execute that already include a debug trap.
 * the problem with this alternative is that there's no good way to tell when the child is done executing the function:
 *   * we could block with waitpid, but then that would block the worker thread for an indeterminate amount of time (and we want to avoid that).
 *   * we could have the main event loop poll periodically, but polling is undesirable.
 * additionally, if someone else is already ptracing that process, we lose the ability to execute code with this approach.
 * therefore, we have this RPC-based system instead.
 *
 * in order to perform an S2C call, however, the target thread MUST be waiting for a message from the server.
 * thus, when we want to perform an S2C call on a thread that isn't waiting for a message, we send it a real-time signal
 * to ask it to execute the S2C call. the signal is handled with the normal wrappers (interrupt_enter and interrupt_exit)
 * to properly handle the case when we may be accidentally interrupting an ongoing call in the thread (since we may have raced
 * with thread trying to perform a server call).
 */

static DarlingServer::Log s2cLog("s2c");

std::optional<DarlingServer::Message> DarlingServer::Thread::_s2cPerform(Message&& call, dserver_s2c_msgnum_t expectedReplyNumber, size_t expectedReplySize) {
	std::optional<Message> reply = std::nullopt;
	bool usingInterrupt = false;

	// make sure we're the only one performing an S2C call on this thread
	if (!dtape_semaphore_down_simple(_s2cPerformSempahore)) {
		// got interrupted while waiting
		return std::nullopt;
	}

	s2cLog.debug() << *this << ": Going to perform S2C call" << s2cLog.endLog;

	{
		std::unique_lock lock(_rwlock);

		if (!_activeCall) {
			// signal the thread that we want to perform an S2C call and wait for it to give us the green light
			lock.unlock();
			s2cLog.debug() << *this << ": Sending S2C signal" << s2cLog.endLog;
			usingInterrupt = true;
			sendSignal(LINUX_SIGRTMIN + 1);
			if (!dtape_semaphore_down_simple(_s2cInterruptEnterSemaphore)) {
				// got interrupted while waiting
				dtape_semaphore_up(_s2cPerformSempahore);
				return std::nullopt;
			}
			s2cLog.debug() << *this << ": Got green light to perform S2C call" << s2cLog.endLog;
			lock.lock();
		} else if (currentThread().get() != this) {
			// we have an active call, so the client is waiting for a reply and is able to perform an S2C call,
			// but we're not the active thread. thus, in order to guarantee the client doesn't receive a reply
			// and stop waiting before we get a chance to perform our S2C call, let's make sure replies are deferred.
			_deferReplyForS2C = true;
		}

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 P8 D6 (caller-S2C sideband) STEP 1 ATTRIBUTION: this S2C is going to THIS thread (a
		// caller-S2C) iff currentThread()==this. Classify the active parent op's lane so we learn whether
		// the reachable caller-S2C munmap hazard is carried by a RING parent (curable by the sideband) or a
		// UDS parent (future hazard) or no managed call (server-internal). Read under the held _rwlock.
		{
			auto hdr = reinterpret_cast<const dserver_s2c_callhdr_t*>(call.data().data());
			bool isMunmap = (hdr->s2c_number == dserver_s2c_msgnum_munmap);
			bool toCurrentCaller = (currentThread().get() == this);
			if (isMunmap && toCurrentCaller) {
				// classify the active parent op's lane (the D6 attribution: which lane carries a caller-S2C
				// munmap). RING parent => curable by the duplex sideband; UDS parent => today not a ring-
				// deadlock (caller services via recvmsg) but a FUTURE hazard if that op is ever ring-
				// migrated; no managed call => pure server-internal.
				if (_ringReplyPending) {
					Metrics::shared().s2cMunmapRingParent.fetch_add(1, std::memory_order_relaxed);
				} else if (_activeCall) {
					Metrics::shared().s2cMunmapUdsParent.fetch_add(1, std::memory_order_relaxed);
				} else {
					Metrics::shared().s2cMunmapNoParent.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
#endif

		// perf #18 D9 (dar-1il.4) + D14 (dar-1il.9): record that the active call drove a CALLER-S2C
		// upcall (an S2C to THIS thread, i.e. while it is the caller waiting for a reply). Any caller-S2C
		// means the op is NOT a simple closed req->reply -- it needs the duplex lane (Lane 2), not the
		// simple ring (Lane 1) -- so the heatmap must flag it regardless of S2C type or ring build. D14
		// replaces the old sticky per-thread _heatmapCallDidS2c bool (consumed at the NEXT recordCall,
		// which over-attributed an exec/teardown munmap to the wrong op -- D13's mldr_path false latch)
		// with PER-CALL-SCOPED attribution: bump the bucket for the op ACTUALLY executing right now
		// (_activeCall), at the moment the S2C fires. _activeCall is null for a server-internal S2C (no
		// managed parent) -- already counted by s2cMunmapNoParent, no per-op bucket to charge.
		if (currentThread().get() == this && _activeCall) {
			Metrics::shared().recordCallerS2cFor(static_cast<uint32_t>(_activeCall->number()));
		}

		call.setAddress(_address);
	}

	// at least for now, in order to wait for the S2C reply, we need the calling thread to be a microthread,
	// so that waiting on the duct-taped semaphore will work
	if (!currentThread()) {
		dtape_semaphore_up(_s2cPerformSempahore);
		throw std::runtime_error("Must be in a microthread (any microthread) to wait for S2C reply");
	}

	s2cLog.debug() << *this << ": Going to send S2C message" << s2cLog.endLog;

#ifdef DSERVER_RING_TRANSPORT
	// perf #18 P8 D4 (dar-1il.3.2.1): if this thread's current call is a DUPLEX PARENT (a ring-originated
	// mach_port_deallocate from a duplex-deallocate-capable caller) and the S2C is the munmap shape, route
	// the upcall through the duplex MAILBOX instead of the UDS send. The fiber then parks on
	// _s2cReplySempahore exactly as the UDS path below; the main-loop _drainDuplexReply harvests the
	// correlated munmap reply, synthesizes _s2cReply, and ups the semaphore. The op runs entirely on the
	// ring -- no UDS fallback after this point, so the destroy side effect is applied exactly once.
	//
	// The guard can DECLINE (no v5 ring / no DEALLOCATE cap / mailbox busy). That decline happens BEFORE
	// any mutation only at the ROUTING layer (ringServiceThread), NOT here -- by the time _s2cPerform runs
	// the dtape op has already begun mutating (we're mid-ipc_right_dealloc). So a decline HERE must NOT
	// silently fall back to a UDS S2C for a ring-parked caller (it can't service it -> the very deadlock
	// we're avoiding). Instead, a decline here is a hard error: we leave the in-flight markers clear and
	// fall through to the UDS send, which is correct ONLY if the caller is NOT ring-parked. Since
	// _ringDuplexParentActive implies a ring-parked caller, a decline here is a should-not-happen
	// (the routing layer already verified the cap + clean mailbox); we log + take the UDS path as a
	// last resort (the bounded guest wait then UDS-falls-back the op). In practice the guard passes.
	bool duplexUpcallTaken = false;
	if (_ringDuplexParentActive && expectedReplyNumber == dserver_s2c_msgnum_munmap) {
		auto* mcall = reinterpret_cast<const dserver_s2c_call_munmap_t*>(call.data().data());
		uint64_t addr = mcall->address;
		uint64_t len = mcall->length;
		std::unique_lock lock(_rwlock);
		duplexUpcallTaken = _s2cTryDuplexMunmapLocked(dserver_s2c_msgnum_munmap, addr, len, lock);
		if (!duplexUpcallTaken) {
			s2cLog.error() << *this << ": duplex munmap guard declined mid-op (mailbox busy?); "
			               << "falling through to UDS S2C (caller may be ring-parked)" << s2cLog.endLog;
		}
	}
	if (!duplexUpcallTaken)
#endif
	{
		// send the call (UDS S2C -- the unchanged path)
		Server::sharedInstance().sendMessage(std::move(call));
	}

	// now let's wait for the reply
	if (!dtape_semaphore_down_simple(_s2cReplySempahore)) {
		// got interrupted while waiting
		dtape_semaphore_up(_s2cPerformSempahore);
		return std::nullopt;
	}

	s2cLog.debug() << *this << ": Received S2C reply" << s2cLog.endLog;

	// extract the reply
	{
		std::unique_lock lock(_rwlock);

		if (!_s2cReply) {
			// impossible, but just in case
			dtape_semaphore_up(_s2cPerformSempahore);
			throw std::runtime_error("S2C reply semaphore incremented, but no reply present");
		}

		reply = std::move(_s2cReply);
		_s2cReply = std::nullopt;

		// if we had replies deferred, now's the time to send them
		if (_deferReplyForS2C) {
			_deferReplyForS2C = false;
			if (_deferredReply) {
#ifdef DSERVER_RING_TRANSPORT
				// perf #18 P5-bulk (dar-1il.1): a ring-originated call that triggered this S2C
				// upcall deferred its reply; it MUST go back onto the s2c ring, not UDS, or the
				// ring-waiting guest never wakes. _publishReplyToRingLocked consumes _ringReplyPending
				// and returns false (not a ring call) -> fall through to the UDS send below.
				if (!_publishReplyToRingLocked(*_deferredReply))
#endif
				{
					Server::sharedInstance().sendMessage(std::move(*_deferredReply));
				}
				_deferredReply = std::nullopt;
				// A0 RPC TRACE: the STASH-DEFERRED[s2c] reply is now flushed. Its absence for a tid that
				// logged STASH-DEFERRED is the stall signature (deferred reply never flushed).
				DarlingServer::__rpctrace("FLUSH-DEFERRED htid=%d nstid=%lld", id(), (long long)nsid());
			}
		}
	}

	s2cLog.debug() << *this << ": Done performing S2C call" << s2cLog.endLog;

	// we're done performing the call; allow others to have a chance at performing an S2C call on this thread
	dtape_semaphore_up(_s2cPerformSempahore);

	if (usingInterrupt) {
		// if we used the S2C signal to perform the call, then the s2c_perform call is currently waiting for us to finish;
		// let it know that we're done
		s2cLog.debug() << *this << ": Allowing thread to resume from S2C interrupt" << s2cLog.endLog;
		dtape_semaphore_up(_s2cInterruptExitSemaphore);
	}

	// partially validate the reply

	if (reply->data().size() != expectedReplySize) {
		throw std::runtime_error("Invalid S2C reply: unxpected size");
	}

	auto replyHeader = reinterpret_cast<dserver_s2c_replyhdr_t*>(reply->data().data());
	if (replyHeader->s2c_number != expectedReplyNumber) {
		throw std::runtime_error("Invalid S2C reply: unexpected S2C reply number");
	}

	return std::move(*reply);
};

#ifdef DSERVER_RING_TRANSPORT
// perf #18 P8 D3 (dar-1il.3.1.1): the GUARDED duplex publish for an S2C upcall -- the iron conjunction
// guard + the mailbox publish. This is the SHARED primitive: in D3 the synthetic sentinel parent op
// drives it (state-machine completion in the drain); in Phase E the real _s2cPerform of a duplex-
// eligible op (deallocate) will drive it the same way and complete by upping its parked fiber. Returns
// true iff the duplex path was taken (the upcall is now in flight); false means the guard declined and
// the caller MUST take its verbatim fallback (UDS for a real op; failure-reply for the synthetic op --
// never a fabricated success).
//
// MUST be entered with `lock` (a unique_lock on _rwlock) held. On success it leaves the in-flight
// markers armed and `lock` STILL HELD (the caller drops it); the drain side (_drainDuplexReply) takes
// _rwlock too, so the markers are published atomically vs. the harvest. The upcall mailbox publish is
// a release-store linearization point; the guest pump observes it with acquire.
bool DarlingServer::Thread::_s2cTryDuplexLocked(uint32_t upcallOp, uint32_t arg, std::unique_lock<std::shared_mutex>& lock) {
	(void)lock; // documents the lock contract; we operate under the held lock
	// --- the iron conjunction guard: duplex ONLY if ALL hold, else the caller falls back ----------
	// (2) the caller has a v4+ ring that advertised the SELFTEST duplex capability.
	auto ring = _ring; // _ring is guarded by _rwlock; we hold it
	if (!ring || !ring->duplexCapable(DSERVER_RING_DUPLEX_CAP_SELFTEST)) {
		return false;
	}
	dserver_ring_shm_t* cb = ring->liveControlBlock();
	if (!cb) {
		return false;
	}
	// (4) the upcall shape is one the minimal protocol supports.
	if (upcallOp != DSERVER_RING_DUPLEX_UPCALL_ECHO) {
		return false;
	}
	// (5) one-outstanding invariant: no upcall already in flight on this thread, and the mailbox slot
	//     is free (no stale unconsumed upcall/reply). A stale ready bit -> decline (don't clobber).
	if (_duplexUpcallInFlight) {
		return false;
	}
	if (dserver_ring_duplex_upcall_available(cb)) {
		return false; // a prior upcall is still unhandled in the mailbox -> not safe to publish
	}
	if (__atomic_load_n(&cb->duplex_reply_ready, __ATOMIC_ACQUIRE) != 0u) {
		return false; // a stale reply sits unconsumed -> decline rather than risk mis-correlation
	}
	// (3)+(6) allocate NONZERO, monotonic correlation ids (ABA-safe: never reused while a stale reply
	//     could exist, because we just checked the mailbox is clean and we bump per upcall).
	uint32_t parentId = _duplexNextId++;
	uint32_t upcallId = _duplexNextId++;
	if (parentId == 0) { parentId = _duplexNextId++; } // wrap guard: 0 means "none"
	if (upcallId == 0) { upcallId = _duplexNextId++; }

	// Arm the in-flight markers BEFORE publishing the upcall, so the drain (which takes _rwlock) can
	// only ever observe a consistent (in-flight + correlation ids) state. (1)+(7) the caller-is-in-
	// duplex-wait and same-ring-instance conditions are intrinsic here: this IS the caller thread's
	// own ring, and the upcall targets that same ring's mailbox -- there is no cross-thread or
	// cross-ring delivery in this path.
	_duplexParentId = parentId;
	_duplexUpcallId = upcallId;
	_duplexReplyReady = false;
	_duplexUpcallInFlight = true;
	_duplexInFlightFast.store(true, std::memory_order_release); // lock-free mirror for the hot drain

	// publish the upcall (release-store linearization) while still holding the lock.
	dserver_ring_duplex_publish_upcall(cb, upcallOp, parentId, upcallId, arg);

	// the guest may be spinning its wait-pump (no syscall) or parked on the s2c futex; bump+conditional
	// FUTEX_WAKE exactly like a Lane-1 reply so a parked pump is woken without a syscall when it spins.
	ring->wakeGuest();

	return true;
};

// perf #18 P8 D3: harvest a correlated duplex reply for this thread, if one is in flight + ready, and
// COMPLETE the parent op. Called from the main-loop ring drain for every ring thread -- this is the
// resume point that keeps the server wait SCOPED to the op (no dedicated waiter thread, no blocking of
// the dserver). For the D3 synthetic sentinel, completion = publish the parent FINAL reply onto the
// s2c ring (routing #8). Returns true iff it did work (harvest or reject). Takes _rwlock itself.
bool DarlingServer::Thread::_drainDuplexReply() {
	// Hot path: the overwhelming common case is no duplex in flight on this thread. Skip the _rwlock
	// entirely with a single relaxed atomic load so the Lane-1 drain cost is unchanged (one load per
	// thread per drain, no lock). Only when a duplex upcall is genuinely in flight do we take the lock.
	if (!_duplexInFlightFast.load(std::memory_order_acquire)) {
		return false;
	}
	std::shared_ptr<RingBuffer> ringForReply;
	uint32_t finalSeq = 0;
	uint32_t finalResult = 0;
	int32_t  finalCode = 0;
	bool completeSentinel = false;
	bool resumeRealFiber = false; // perf #18 P8 D4: up _s2cReplySempahore for a real-op duplex parent
	{
		std::unique_lock lock(_rwlock);
		if (!_duplexUpcallInFlight) {
			return false; // nothing parked on a duplex reply
		}
		auto ring = _ring;
		if (!ring) {
			return false;
		}
		dserver_ring_shm_t* cb = ring->liveControlBlock();
		if (!cb) {
			return false;
		}
		int mismatch = 0;
		if (dserver_ring_duplex_reply_ready(cb, _duplexParentId, _duplexUpcallId, &mismatch)) {
			// correlated reply: harvest it, consume the slot, clear the in-flight state.
			_duplexReplyStatus = cb->duplex_reply_status;
			_duplexReplyArg = cb->duplex_reply_arg;
			int32_t replyErrno = cb->duplex_reply_errno;
			_duplexReplyReady = true;
			uint32_t realUpcallNum = _duplexRealUpcallNum;
			dserver_ring_duplex_consume_reply(cb);
			_duplexUpcallInFlight = false;
			_duplexRealUpcallNum = 0;
			_duplexUpcallDeadlineNs = 0; // perf #18 P8 D6: disarm the fail-closed deadline on success
			_duplexInFlightFast.store(false, std::memory_order_release);
			Metrics::shared().ringDuplexS2c.fetch_add(1, std::memory_order_relaxed);
			if (_duplexSentinelSeq != 0) {
				// D3 synthetic sentinel completion: stage the parent final reply (published below, lock
				// dropped). The parent op's result IS the echo result the guest computed for the upcall.
				completeSentinel = true;
				ringForReply = ring;
				finalSeq = _duplexSentinelSeq;
				finalResult = _duplexReplyArg;
				finalCode = (_duplexReplyStatus == 0) ? 0 : -1;
				_duplexSentinelSeq = 0;
			} else if (realUpcallNum == (uint32_t)dserver_s2c_msgnum_munmap) {
				// perf #18 P8 D5 (dar-1il.3.2.2) boot-scoped proof: a REAL caller-S2C munmap just completed
				// over the duplex mailbox for a vm_deallocate proof parent. Count it + mark the sticky flag
				// so the dispatcher spends a proof-budget unit (auto-disarm). THIS is the live caller-S2C
				// cure the duplex lane was built for, finally on the real op.
				if (_ringDuplexVmdeallocProof) {
					Metrics::shared().ringDuplexVmdeallocS2c.fetch_add(1, std::memory_order_relaxed);
					_ringDuplexVmdeallocS2cFired = true;
				}
				// perf #18 P8 D4: REAL-op resume. Synthesize the _s2cReply Message exactly as the UDS S2C
				// reply would arrive (a dserver_s2c_reply_munmap_t carrying the guest's munmap result), so
				// _s2cPerform's reply-extraction + validation path is byte-identical to UDS. Then up
				// _s2cReplySempahore to resume the parked fiber, which finishes the deallocate and emits
				// its final reply via the existing deferred-reply->ring path.
				Message s2cReply(sizeof(dserver_s2c_reply_munmap_t), 0);
				auto* r = reinterpret_cast<dserver_s2c_reply_munmap_t*>(s2cReply.data().data());
				r->header.call_number = 0;
				r->header.pid = 0;
				r->header.tid = 0;
				r->header.architecture = 0;
				r->header.s2c_number = dserver_s2c_msgnum_munmap;
				r->return_value = _duplexReplyStatus; // guest's munmap return_value (0 ok / -1 error)
				r->errno_result = replyErrno;
				if (_s2cReply) {
					// should be impossible (one outstanding upcall) -- but never clobber a pending reply.
					s2cLog.error() << *this << ": duplex munmap reply but an _s2cReply was already pending" << s2cLog.endLog;
				} else {
					_s2cReply = std::move(s2cReply);
					resumeRealFiber = true;
				}
			}
		} else if (mismatch) {
			// a reply landed that does NOT correlate (wrong parent/upcall id: a buggy/hostile guest or
			// a stale reply from a torn-down op). Refuse to resume on it: drop the slot, count it, and
			// FAIL the parent (no wedge, no fabricated success). pitfall #2 (ABA) + #3 (wrong corr).
			uint32_t realUpcallNum = _duplexRealUpcallNum;
			dserver_ring_duplex_consume_reply(cb);
			_duplexUpcallInFlight = false;
			_duplexRealUpcallNum = 0;
			_duplexUpcallDeadlineNs = 0; // perf #18 P8 D6: disarm the fail-closed deadline on mis-correlation
			_duplexInFlightFast.store(false, std::memory_order_release);
			Metrics::shared().ringDuplexReject.fetch_add(1, std::memory_order_relaxed);
			if (_duplexSentinelSeq != 0) {
				completeSentinel = true;
				ringForReply = ring;
				finalSeq = _duplexSentinelSeq;
				finalResult = 0;
				finalCode = -1; // mis-correlation -> the synthetic parent fails
				_duplexSentinelSeq = 0;
			} else if (realUpcallNum == (uint32_t)dserver_s2c_msgnum_munmap) {
				// perf #18 P8 D4: a mis-correlated reply for a REAL munmap upcall. We must NOT resume the
				// fiber on a bogus munmap result (that could tell the kernel the unmap succeeded when it
				// didn't = corruption). Synthesize a FAILED munmap reply (return_value=-1, EINTR) so the
				// fiber resumes, the deallocate sees the S2C failed, and the op surfaces an error rather
				// than fabricating success. Bounded: the fiber resumes immediately (no wedge).
				Message s2cReply(sizeof(dserver_s2c_reply_munmap_t), 0);
				auto* r = reinterpret_cast<dserver_s2c_reply_munmap_t*>(s2cReply.data().data());
				r->header.call_number = 0;
				r->header.pid = 0;
				r->header.tid = 0;
				r->header.architecture = 0;
				r->header.s2c_number = dserver_s2c_msgnum_munmap;
				r->return_value = -1;
				r->errno_result = 4 /*EINTR*/;
				if (!_s2cReply) {
					_s2cReply = std::move(s2cReply);
					resumeRealFiber = true;
				}
			}
		} else {
			// in flight, no correlated reply yet. perf #18 P8 D6: enforce the SERVER-side fail-closed
			// deadline (gist failure policy #12). If the guest never pumped this S2C by the deadline,
			// resume the fiber with a FAILED reply (so the op surfaces an error, never a leaked fiber /
			// fabricated success), count the timeout, clear the in-flight + mailbox state, and disarm the
			// vm_deallocate proof so a stuck guest can't keep re-arming. Bounded: the fiber resumes here.
			if (_duplexUpcallDeadlineNs != 0) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
				if (now >= _duplexUpcallDeadlineNs) {
					uint32_t realUpcallNum = _duplexRealUpcallNum;
					// clear the mailbox upcall slot so a late guest pump can't act on a stale upcall, and
					// reset all in-flight markers.
					dserver_ring_duplex_consume_reply(cb); // idempotent clear of any partial reply state
					__atomic_store_n(&cb->duplex_upcall_ready, 0u, __ATOMIC_RELEASE); // retract the unhandled upcall
					_duplexUpcallInFlight = false;
					_duplexRealUpcallNum = 0;
					_duplexUpcallDeadlineNs = 0;
					_duplexInFlightFast.store(false, std::memory_order_release);
					Metrics::shared().ringDuplexVmdeallocTimeout.fetch_add(1, std::memory_order_relaxed);
					if (_ringDuplexVmdeallocProof) {
						// disarm the boot/synthetic proof so a non-pumping guest can't keep arming it.
						Metrics::shared().ringDuplexVmdeallocDisarmed.fetch_add(1, std::memory_order_relaxed);
					}
					s2cLog.error() << *this << ": [D6] duplex S2C upcall TIMED OUT (no guest pump by deadline); "
					               << "failing the parent op closed (bounded, no leaked fiber)" << s2cLog.endLog;
					if (_duplexSentinelSeq != 0) {
						completeSentinel = true;
						ringForReply = ring;
						finalSeq = _duplexSentinelSeq;
						finalResult = 0;
						finalCode = -1;
						_duplexSentinelSeq = 0;
					} else if (realUpcallNum == (uint32_t)dserver_s2c_msgnum_munmap) {
						Message s2cReply(sizeof(dserver_s2c_reply_munmap_t), 0);
						auto* r = reinterpret_cast<dserver_s2c_reply_munmap_t*>(s2cReply.data().data());
						r->header.call_number = 0;
						r->header.pid = 0;
						r->header.tid = 0;
						r->header.architecture = 0;
						r->header.s2c_number = dserver_s2c_msgnum_munmap;
						r->return_value = -1;
						r->errno_result = 110 /*ETIMEDOUT*/;
						if (!_s2cReply) {
							_s2cReply = std::move(s2cReply);
							resumeRealFiber = true;
						}
					}
				} else {
					return false; // in flight, deadline not yet reached
				}
			} else {
				return false; // in flight but no reply yet (no deadline armed -- e.g. D3 sentinel echo)
			}
		}
	}

	if (resumeRealFiber) {
		// resume the parked deallocate fiber with the synthesized munmap reply (the UDS path's wakeup).
		dtape_semaphore_up(_s2cReplySempahore);
		return true;
	}

	if (completeSentinel && ringForReply) {
		// publish the synthetic parent's final reply onto the s2c ring. Reply convention mirrors the
		// Lane-1 port-trap reply ({reply_hdr.code, uint32 result}); the guest selftest reads `result`.
		uint32_t body = finalResult;
		if (!ringForReply->publishReply(finalSeq, DSERVER_RING_DUPLEX_SELFTEST_CALLNUM, finalCode, &body, (uint32_t)sizeof(body))) {
			// s2c full: the guest will time out on the parent + UDS-fall-back in a real lane. For the
			// synthetic selftest this is a transient miss; count it as an s2c-full like Lane-1.
			Metrics::shared().ringS2cFull.fetch_add(1, std::memory_order_relaxed);
		}
		ringForReply->wakeGuest();
		return true;
	}
	return true;
};

bool DarlingServer::Thread::drainDuplexReply() {
	return _drainDuplexReply();
};

// perf #18 P8 D3: issue ONE synthetic duplex ECHO upcall for the sentinel parent op (seq = the parent
// request's ring seq). Runs the guarded publish; if the guard passes, the upcall is in flight and the
// parent reply will be published by _drainDuplexReply when the correlated reply arrives. Returns true
// if the duplex path was taken (parent reply deferred to the drain); false if the guard declined, in
// which case the CALLER must publish the synthetic parent's failure reply immediately (no fabricated
// success). NOT a microthread/fiber path -- this is a synchronous state-machine kickoff from the ring
// service loop, so it never parks (the brief's "publish upcall + return; later drain completes").
bool DarlingServer::Thread::duplexSelftestUpcall(uint32_t arg, uint32_t seq) {
	std::unique_lock lock(_rwlock);
	if (_duplexUpcallInFlight) {
		return false; // one-outstanding: a prior selftest is mid-flight on this thread
	}
	_duplexSentinelSeq = seq; // mark this in-flight upcall as a sentinel parent (drain completes it)
	bool took = _s2cTryDuplexLocked(DSERVER_RING_DUPLEX_UPCALL_ECHO, arg, lock);
	if (!took) {
		_duplexSentinelSeq = 0; // guard declined -> not in flight; caller publishes the failure reply
	}
	return took;
};

// perf #18 P8 D4 (dar-1il.3.2.1): publish a REAL munmap S2C upcall into the duplex mailbox for a duplex-
// parent call. The structural sibling of _s2cTryDuplexLocked, but for the MUNMAP shape + the typed
// addr/len payload, and it ALSO requires DUPLEX_CAP_DEALLOCATE (the echo guard required only SELFTEST).
// On success it arms the in-flight markers + publishes + wakes, and the CALLER (_s2cPerform) then parks
// the fiber on _s2cReplySempahore exactly as the UDS S2C path does. The main-loop _drainDuplexReply
// harvests the correlated munmap reply, synthesizes the _s2cReply Message, and ups _s2cReplySempahore.
bool DarlingServer::Thread::_s2cTryDuplexMunmapLocked(uint32_t s2cNumber, uint64_t address, uint64_t length, std::unique_lock<std::shared_mutex>& lock) {
	(void)lock; // documents the held-lock contract
	auto ring = _ring;
	// require a v5+ duplex-capable ring AND a munmap-pump cap. The munmap shape is reachable from EITHER a
	// (D4) mach_port_deallocate parent OR a (D5) vm_deallocate parent; both advertise they can pump munmap,
	// via CAP_DEALLOCATE (0x2) or CAP_VM_DEALLOCATE (0x4). SELFTEST alone is not enough. The per-op routing
	// decline (ringServiceThread) already keyed on the SPECIFIC bit before dispatch, so by the time we
	// publish a munmap upcall the caller has the matching cap; this guard is the belt-and-suspenders check.
	if (!ring || !ring->duplexCapable(DSERVER_RING_DUPLEX_CAP_MUNMAP_PUMP)) {
		return false;
	}
	dserver_ring_shm_t* cb = ring->liveControlBlock();
	if (!cb) {
		return false;
	}
	// one-outstanding invariant: nothing already in flight + a clean mailbox (no stale upcall/reply).
	if (_duplexUpcallInFlight) {
		return false;
	}
	if (dserver_ring_duplex_upcall_available(cb)) {
		return false;
	}
	if (__atomic_load_n(&cb->duplex_reply_ready, __ATOMIC_ACQUIRE) != 0u) {
		return false;
	}
	// nonzero, monotonic correlation ids (ABA-safe: the mailbox is clean + we bump per upcall).
	uint32_t parentId = _duplexNextId++;
	uint32_t upcallId = _duplexNextId++;
	if (parentId == 0) { parentId = _duplexNextId++; }
	if (upcallId == 0) { upcallId = _duplexNextId++; }

	_duplexParentId = parentId;
	_duplexUpcallId = upcallId;
	_duplexReplyReady = false;
	_duplexUpcallInFlight = true;
	_duplexRealUpcallNum = s2cNumber; // mark this as a REAL-op upcall (drain synthesizes _s2cReply)
	_duplexInFlightFast.store(true, std::memory_order_release);
	// perf #18 P8 D6: arm the SERVER-side fail-closed deadline (gist failure policy #12). If the guest
	// never pumps this munmap S2C (a buggy/torn-down/RED-no-pump guest), the main-loop drain resumes the
	// fiber with a FAILED reply at the deadline instead of leaking the fiber forever. Generous (5s) -- a
	// real pump replies in microseconds, so this never trips in GREEN; it bounds only the pathological case.
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		_duplexUpcallDeadlineNs = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + 5000000000ull;
	}

	dserver_ring_duplex_publish_munmap_upcall(cb, parentId, upcallId, address, length);
	ring->wakeGuest();
	return true;
};

void DarlingServer::Thread::setRingDuplexParentActive(bool active) {
	std::unique_lock lock(_rwlock);
	_ringDuplexParentActive = active;
};

// perf #18 P8 D5 (dar-1il.3.2.2) boot-scoped proof.
void DarlingServer::Thread::setRingDuplexVmdeallocProof(bool active) {
	std::unique_lock lock(_rwlock);
	_ringDuplexVmdeallocProof = active;
	if (active) {
		_ringDuplexVmdeallocS2cFired = false; // reset the per-dispatch sticky flag at arm time
	}
};

bool DarlingServer::Thread::takeRingDuplexVmdeallocS2cFired() {
	std::unique_lock lock(_rwlock);
	bool v = _ringDuplexVmdeallocS2cFired;
	_ringDuplexVmdeallocS2cFired = false;
	return v;
};

bool DarlingServer::Thread::duplexDeallocateCapable() const {
	std::unique_lock lock(const_cast<std::shared_mutex&>(_rwlock));
	auto ring = _ring;
	if (!ring) {
		return false;
	}
	return ring->duplexCapable(DSERVER_RING_DUPLEX_CAP_DEALLOCATE);
};

// perf #18 P8 D5 (dar-1il.3.2.2): the per-op routing gate for vm_deallocate. Keys on the SPECIFIC
// VM_DEALLOCATE cap bit (not the shared munmap-pump mask): a guest that advertised only the (D4)
// DEALLOCATE cap must NOT have a vm_deallocate routed onto the lane on its behalf.
bool DarlingServer::Thread::duplexVmDeallocateCapable() const {
	std::unique_lock lock(const_cast<std::shared_mutex&>(_rwlock));
	auto ring = _ring;
	if (!ring) {
		return false;
	}
	return ring->duplexCapable(DSERVER_RING_DUPLEX_CAP_VM_DEALLOCATE);
};
#endif // DSERVER_RING_TRANSPORT

uintptr_t DarlingServer::Thread::_mmap(uintptr_t address, size_t length, int protection, int flags, int fd, off_t offset, int& outErrno) {
	// XXX: not sure if we want to force all allocations in 32-bit processes to be in the 32-bit address space.
	//      for now, we leave it up to the caller.
#if 0
	auto process = _process.lock();

	if (!process) {
		throw std::runtime_error("Cannot perform mmap without valid process");
	}

	if (process->architecture() == Process::Architecture::i386 || process->architecture() == Process::Architecture::ARM32) {
		flags |= MAP_32BIT;
	}
#endif

	Message callMessage(sizeof(dserver_s2c_call_mmap_t), (fd < 0) ? 0 : 1);
	auto call = reinterpret_cast<dserver_s2c_call_mmap_t*>(callMessage.data().data());

	call->header.call_number = dserver_callnum_s2c;
	call->header.s2c_number = dserver_s2c_msgnum_mmap;
	call->address = address;
	call->length = length;
	call->protection = protection;
	call->flags = flags;
	call->fd = (fd < 0) ? -1 : 0;
	call->offset = offset;

	if (fd >= 0) {
		auto dupfd = dup(fd);
		if (dupfd < 0) {
			outErrno = errno;
			return (uintptr_t)MAP_FAILED;
		}
		callMessage.pushDescriptor(dupfd);
	}

	s2cLog.debug() << "Performing _mmap with address=" << call->address << ", length=" << call->length << ", protection=" << call->protection << ", flags=" << call->flags << ", fd=" << call->fd << " (" << fd << ")" << ", offset=" << call->offset << s2cLog.endLog;

	auto maybeReplyMessage = _s2cPerform(std::move(callMessage), dserver_s2c_msgnum_mmap, sizeof(dserver_s2c_reply_mmap_t));
	if (!maybeReplyMessage) {
		s2cLog.debug() << "_mmap call interrupted" << s2cLog.endLog;
		outErrno = EINTR;
		return (uintptr_t)MAP_FAILED;
	}

	auto replyMessage = std::move(*maybeReplyMessage);
	auto reply = reinterpret_cast<dserver_s2c_reply_mmap_t*>(replyMessage.data().data());

	s2cLog.debug() << "_mmap returned address=" << reply->address << ", errno_result=" << reply->errno_result << s2cLog.endLog;

	outErrno = reply->errno_result;
	return reply->address;
};

int DarlingServer::Thread::_munmap(uintptr_t address, size_t length, int& outErrno) {
	Message callMessage(sizeof(dserver_s2c_call_munmap_t), 0);
	auto call = reinterpret_cast<dserver_s2c_call_munmap_t*>(callMessage.data().data());

	call->header.call_number = dserver_callnum_s2c;
	call->header.s2c_number = dserver_s2c_msgnum_munmap;
	call->address = address;
	call->length = length;

	s2cLog.debug() << "Performing _munmap with address=" << call->address << ", length=" << call->length << s2cLog.endLog;

	auto maybeReplyMessage = _s2cPerform(std::move(callMessage), dserver_s2c_msgnum_munmap, sizeof(dserver_s2c_reply_munmap_t));
	if (!maybeReplyMessage) {
		s2cLog.debug() << "_munmap call interrupted" << s2cLog.endLog;
		outErrno = EINTR;
		return -1;
	}

	auto replyMessage = std::move(*maybeReplyMessage);
	auto reply = reinterpret_cast<dserver_s2c_reply_munmap_t*>(replyMessage.data().data());

	s2cLog.debug() << "_munmap returned return_value=" << reply->return_value << ", errno_result=" << reply->errno_result << s2cLog.endLog;

	outErrno = reply->errno_result;
	return reply->return_value;
};

int DarlingServer::Thread::_mprotect(uintptr_t address, size_t length, int protection, int& outErrno) {
	Message callMessage(sizeof(dserver_s2c_call_mprotect_t), 0);
	auto call = reinterpret_cast<dserver_s2c_call_mprotect_t*>(callMessage.data().data());

	call->header.call_number = dserver_callnum_s2c;
	call->header.s2c_number = dserver_s2c_msgnum_mprotect;
	call->address = address;
	call->length = length;
	call->protection = protection;

	s2cLog.debug() << "Performing _mprotect with address=" << call->address << ", length=" << call->length << ", protection=" << call->protection << s2cLog.endLog;

	auto maybeReplyMessage = _s2cPerform(std::move(callMessage), dserver_s2c_msgnum_mprotect, sizeof(dserver_s2c_reply_mprotect_t));
	if (!maybeReplyMessage) {
		s2cLog.debug() << "_mprotect call interrupted" << s2cLog.endLog;
		outErrno = EINTR;
		return -1;
	}

	auto replyMessage = std::move(*maybeReplyMessage);
	auto reply = reinterpret_cast<dserver_s2c_reply_mprotect_t*>(replyMessage.data().data());

	s2cLog.debug() << "_mprotect returned return_value=" << reply->return_value << ", errno_result=" << reply->errno_result << s2cLog.endLog;

	outErrno = reply->errno_result;
	return reply->return_value;
};

int DarlingServer::Thread::_msync(uintptr_t address, size_t size, int sync_flags, int& outErrno) {
	Message callMessage(sizeof(dserver_s2c_call_msync_t), 0);
	auto call = reinterpret_cast<dserver_s2c_call_msync_t*>(callMessage.data().data());

	call->header.call_number = dserver_callnum_s2c;
	call->header.s2c_number = dserver_s2c_msgnum_msync;
	call->address = address;
	call->size = size;
	call->sync_flags = sync_flags;

	s2cLog.debug() << "Performing _msync with address=" << call->address << ", size=" << call->size << ", sync_flags=" << call->sync_flags << s2cLog.endLog;

	auto maybeReplyMessage = _s2cPerform(std::move(callMessage), dserver_s2c_msgnum_msync, sizeof(dserver_s2c_reply_msync_t));
	if (!maybeReplyMessage) {
		s2cLog.debug() << "_msync call interrupted" << s2cLog.endLog;
		outErrno = EINTR;
		return -1;
	}

	auto replyMessage = std::move(*maybeReplyMessage);
	auto reply = reinterpret_cast<dserver_s2c_reply_msync_t*>(replyMessage.data().data());

	s2cLog.debug() << "_msync returned return_value=" << reply->return_value << ", errno_result=" << reply->errno_result << s2cLog.endLog;

	outErrno = reply->errno_result;
	return reply->return_value;
};

uintptr_t DarlingServer::Thread::allocatePages(size_t pageCount, int protection, uintptr_t addressHint, bool fixed, bool overwrite) {
	int err = 0;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	if (fixed && overwrite) {
		flags |= MAP_FIXED;
	} else if (fixed) {
		flags |= MAP_FIXED_NOREPLACE;
	}
	auto result = _mmap(addressHint, pageCount * sysconf(_SC_PAGESIZE), protection, flags, -1, 0, err);
	if (result == (uintptr_t)MAP_FAILED) {
		throw std::system_error(err, std::generic_category(), "S2C mmap call failed");
	}
	return result;
};

void DarlingServer::Thread::freePages(uintptr_t address, size_t pageCount) {
	int err = 0;
	if (_munmap(address, pageCount * sysconf(_SC_PAGESIZE), err) < 0) {
		throw std::system_error(err, std::generic_category(), "S2C munmap call failed");
	}
};

uintptr_t DarlingServer::Thread::mapFile(int fd, size_t pageCount, int protection, uintptr_t addressHint, size_t pageOffset, bool fixed, bool overwrite) {
	int err = 0;
	int flags = MAP_SHARED;
	if (fixed && overwrite) {
		flags |= MAP_FIXED;
	} else if (fixed) {
		flags |= MAP_FIXED_NOREPLACE;
	}
	auto result = _mmap(addressHint, pageCount * sysconf(_SC_PAGESIZE), protection, flags, fd, pageOffset * sysconf(_SC_PAGESIZE), err);
	if (result == (uintptr_t)MAP_FAILED) {
		throw std::system_error(err, std::generic_category(), "S2C mmap call failed");
	}
	return result;
};

void DarlingServer::Thread::changeProtection(uintptr_t address, size_t pageCount, int protection) {
	int err = 0;
	if (_mprotect(address, pageCount * sysconf(_SC_PAGESIZE), protection, err) < 0) {
		throw std::system_error(err, std::generic_category(), "S2C mprotect call failed");
	}
};

void DarlingServer::Thread::syncMemory(uintptr_t address, size_t size, int sync_flags) {
	int err = 0;
	if (_msync(address, size, sync_flags, err) < 0) {
		throw std::system_error(err, std::generic_category(), "S2C msync call failed");
	}
};

void DarlingServer::Thread::waitUntilRunning() {
	std::shared_lock lock(_rwlock);
	_runningCondvar.wait(lock, [&]() {
		return _running;
	});
};

void DarlingServer::Thread::waitUntilNotRunning() {
	std::shared_lock lock(_rwlock);
	_runningCondvar.wait(lock, [&]() {
		return !_running;
	});
};

void DarlingServer::Thread::_deferLocked(bool wait, std::unique_lock<std::shared_mutex>& lock) {
	if (_deferralState == DeferralState::NotDeferred) {
		_deferralState = DeferralState::DeferredNotPending;
	}

	if (wait) {
		_runningCondvar.wait(lock, [&]() {
			return !_running;
		});
	}
};

void DarlingServer::Thread::defer(bool wait) {
	std::unique_lock lock(_rwlock);
	_deferLocked(wait, lock);
};

void DarlingServer::Thread::_undeferLocked(std::unique_lock<std::shared_mutex>& lock) {
	DeferralState previousDeferralState;

	previousDeferralState = _deferralState;
	_deferralState = DeferralState::NotDeferred;

	if (previousDeferralState == DeferralState::DeferredPending) {
		Server::sharedInstance().scheduleThread(shared_from_this());
	}
};

void DarlingServer::Thread::undefer() {
	std::unique_lock lock(_rwlock);
	_undeferLocked(lock);
};

uint32_t* DarlingServer::Thread::bsdReturnValuePointer() {
	return &_bsdReturnValue;
};

void DarlingServer::Thread::logToStream(Log::Stream& stream) const {
	stream << "[T:" << _tid << "(" << _nstid << ")]";
};

#ifdef DSERVER_RING_TRANSPORT
// perf #18 P3 / P5-bulk: publish a complete reply Message onto the s2c ring if this thread's current
// call is ring-originated. Returns true if it took ownership of the reply (published, OR consumed it
// to UDS-fall-back on a full ring); false if this is not a ring call (caller must send it via UDS).
// Consumes _ringReplyPending. _rwlock MUST be held by the caller. The reply Message data is
// dserver_rpc_reply_<call>_t = {replyhdr{number,code}, body}; we carry the code + body bytes onto the
// ring (the guest reconstructs the same body).
bool DarlingServer::Thread::_publishReplyToRingLocked(Message& reply) {
	if (!_ringReplyPending || !_ring) {
		return false;
	}
	_ringReplyPending = false;
	const auto& bytes = reply.data();
	if (bytes.size() < sizeof(dserver_rpc_replyhdr_t)) {
		return false; // malformed (too short) -> caller UDS-falls-back
	}
	const dserver_rpc_replyhdr_t* rhdr = reinterpret_cast<const dserver_rpc_replyhdr_t*>(bytes.data());
	const uint8_t* body = bytes.data() + sizeof(dserver_rpc_replyhdr_t);
	uint32_t bodyLen = static_cast<uint32_t>(bytes.size() - sizeof(dserver_rpc_replyhdr_t));
	auto ring = _ring; // keep alive across the publish
	uint32_t seq = _ringReplySeq;
	uint32_t callnum = static_cast<uint32_t>(rhdr->number);
	int32_t code = rhdr->code;
	// publish under the lock is fine (no suspend, no Server-state mutation); the FUTEX_WAKE is a
	// bare syscall and likewise can't re-enter our locks.
#ifdef DSERVER_RING_PHASE_PROF
	uint64_t _pubT0 = Metrics::rdtscCycles();
#endif
	bool published = ring->publishReply(seq, callnum, code, body, bodyLen);
#ifdef DSERVER_RING_PHASE_PROF
	_ringPublishCycles = Metrics::rdtscCycles() - _pubT0; // read back by ringServiceThread
#endif
	if (!published) {
		// s2c full: fall back to a UDS reply so the guest still gets its answer.
		Metrics::shared().ringS2cFull.fetch_add(1, std::memory_order_relaxed);
		Server::sharedInstance().sendMessage(std::move(reply));
	} else {
		ring->wakeGuest();
	}
	return true;
}
#endif

void DarlingServer::Thread::pushCallReply(std::shared_ptr<Call> expectedCall, Message&& reply) {
	std::unique_lock lock(_rwlock);

	if (expectedCall) {
		_deactivateCallLocked(expectedCall);
	}

	// A0 RPC TRACE: name the disposition of this reply. Four outcomes, keyed by tid/nsid so it lines up
	// with the RECV line and the guest-capture /proc/<tid>. A stall reads directly off the tape: a RECV
	// whose reply ends in STASH-SAVED or STASH-DEFERRED with no later FLUSH-* line, or a RECV that never
	// produces any REPLY-* line, names the stuck call + drop site. (call number from expectedCall.)
	{
		unsigned callnum = expectedCall ? (unsigned)expectedCall->number() : 0u;
		const char* disp = _interruptedForSignal ? "STASH-SAVED[interrupt]"
			: _deferReplyForS2C ? "STASH-DEFERRED[s2c]"
			: _dead ? "DROP-DEAD"
			: "SEND";
		DarlingServer::__rpctrace("REPLY-DISP htid=%d nstid=%lld call=%u disp=%s",
			id(), (long long)nsid(), callnum, disp);
	}

	if (_interruptedForSignal) {
		if (_interrupts.top().savedReply) {
			throw std::runtime_error("New reply would overwrite existing saved reply");
		}

		_interrupts.top().savedReply = std::move(reply);
	} else if (_deferReplyForS2C) {
		// A ring-originated call that performs an S2C upcall defers its reply here; the flush in
		// _s2cPerform() (NOT this path) republishes it -- and MUST honor _ringReplyPending or a
		// ring-waiting guest wedges. We deliberately do NOT consume _ringReplyPending here so the
		// flush still knows the reply belongs to the ring. (perf #18 P5-bulk / dar-1il.1)
		_deferredReply = std::move(reply);
	} else if (!_dead) {
#ifdef DSERVER_RING_TRANSPORT
		if (_publishReplyToRingLocked(reply)) {
			DarlingServer::__rpctrace("SENT-RING htid=%d nstid=%lld", id(), (long long)nsid());
			return;
		}
#endif
		DarlingServer::__rpctrace("SENT-UDS htid=%d nstid=%lld", id(), (long long)nsid());
		Server::sharedInstance().sendMessage(std::move(reply));
	}
};

bool DarlingServer::Thread::isCurrentlySuspended() const {
	// perf #2b: read the same _suspended flag doWork()/suspend() maintain. A microthread
	// that ran to completion inline never set _suspended; one that blocked has it set
	// (until a resume clears it). Shared-lock is enough -- this is a single-bool read.
	std::shared_lock lock(_rwlock);
	return _suspended;
};

DarlingServer::Thread::RunState DarlingServer::Thread::getRunState() const {
	auto process = this->process();
	if (!process || isDead()) {
		return RunState::Dead;
	}

	std::ifstream file("/proc/" + std::to_string(process->id()) + "/task/" + std::to_string(id()) + "/stat");
	std::string line;
	if (!std::getline(file, line)) {
		return RunState::Dead;
	}

	auto endOfComm = line.find(')');
	if (endOfComm == std::string::npos) {
		return RunState::Dead;
	}

	if (line.size() <= endOfComm + 2) {
		return RunState::Dead;
	}

	switch (line[endOfComm + 2]) {
		case 'R':
			return RunState::Running;
		case 'S':
			return RunState::Interruptible;
		case 'D':
			return RunState::Uninterruptible;
		case 'T':
			return RunState::Stopped;
		default:
			return RunState::Dead;
	}
};

void DarlingServer::Thread::waitWhileUserSuspended(uintptr_t threadStateAddress, uintptr_t floatStateAddress) {
	loadStateFromUser(threadStateAddress, floatStateAddress);
	dtape_thread_wait_while_user_suspended(_dtapeThread);
	try {
		saveStateToUser(threadStateAddress, floatStateAddress);
	} catch (std::system_error e) {
		// if we fail to save the state back to the process, that likely means the process died or was killed while waiting.
		// it's nothing to worry about. just log it and move on.
		threadLog.warning() << *this << ": failed to save state back to user in waitWhileUserSuspended: " << e.code() << " (" << e.what() << ")" << threadLog.endLog;
	}
};

void DarlingServer::Thread::sendSignal(int signal) const {
	if (isDead()) {
		return;
	}
	if (_process) {
		if (syscall(SYS_tgkill, _process->id(), id(), signal) < 0) {
			int code = errno;
			throw std::system_error(code, std::generic_category());
		}
	} else {
		throw std::system_error(ESRCH, std::generic_category());
	}
};

void DarlingServer::Thread::jumpToResume(void* stack, size_t stackSize) {
#if DSERVER_ASAN
	__sanitizer_start_switch_fiber(&asanOldFakeStack, stack, stackSize);
#endif
	setcontext(&_resumeContext);
	__builtin_unreachable();
};

void DarlingServer::Thread::notifyDead() {
	bool canRelease = false;

#ifdef DSERVER_RING_TRANSPORT
	// perf #18 (dar-dar6x4-perf-5dq.30): grab the ring + its Monitor out from under _rwlock
	// and release them AFTER unlocking -- removeMonitor() touches Server state and could
	// otherwise reintroduce the dar-6x4 lock-across-suspend hazard class. The RingBuffer dtor
	// unmaps + closes the eventfd.
	std::shared_ptr<RingBuffer> ringToRelease = nullptr;
	std::shared_ptr<Monitor> ringMonitorToRelease = nullptr;
#endif

	{
		std::unique_lock lock(_rwlock);
		if (_dead) {
			return;
		}

		threadLog.info() << *this << ": thread dying" << threadLog.endLog;
		_dead = true;

#ifdef DSERVER_RING_TRANSPORT
		ringMonitorToRelease = std::move(_ringMonitor);
		ringToRelease = std::move(_ring);
		_ringMonitor = nullptr;
		_ring = nullptr;
#endif

		if (!_activeCall) {
			// if we have no active call, we won't ever need to run again,
			// so set `_terminating` to make sure that doesn't happen
			_terminating = true;
			canRelease = true;
		}
	}

#ifdef DSERVER_RING_TRANSPORT
	// _rwlock is dropped now; remove ourselves from the main-loop spin registry (perf #18 P4),
	// tear down the ring's epoll Monitor, and release the mapping.
	if (ringToRelease) {
		Server::sharedInstance().unregisterRingThread(shared_from_this());
	}
	if (ringMonitorToRelease) {
		Server::sharedInstance().removeMonitor(ringMonitorToRelease);
	}
	ringMonitorToRelease = nullptr;
	ringToRelease = nullptr; // RingBuffer dtor: munmap + close eventfd
#endif

	// keep ourselves alive until the duct-taped context is done
	_selfReference = shared_from_this();

	dtape_thread_dying(_dtapeThread);

	if (canRelease) {
		_scheduleRelease();
	} else {
		resume();
	}

	threadRegistry().unregisterEntry(shared_from_this());
};

bool DarlingServer::Thread::isDead() const {
	std::shared_lock lock(_rwlock);
	return _dead;
};

#ifdef DSERVER_RING_TRANSPORT
void DarlingServer::Thread::attachRing(std::shared_ptr<RingBuffer> ring, std::shared_ptr<Monitor> monitor) {
	std::shared_ptr<RingBuffer> oldRing = nullptr;
	std::shared_ptr<Monitor> oldMonitor = nullptr;
	{
		std::unique_lock lock(_rwlock);
		// a thread attaches at most once in practice, but replace defensively
		oldRing = std::move(_ring);
		oldMonitor = std::move(_ringMonitor);
		_ring = ring;
		_ringMonitor = monitor;
	}
	// drop any prior ring's Monitor outside the lock (same hazard discipline as notifyDead)
	if (oldMonitor) {
		Server::sharedInstance().removeMonitor(oldMonitor);
	}
};

std::shared_ptr<DarlingServer::RingBuffer> DarlingServer::Thread::ring() const {
	std::shared_lock lock(_rwlock);
	return _ring;
};

void DarlingServer::Thread::beginRingReply(uint32_t seq) {
	std::unique_lock lock(_rwlock);
	_ringReplyPending = true;
	_ringReplySeq = seq;
	// perf #18 D9: latch that this call is ring-originated so the heatmap can attribute its transport
	// at recordCall time (by then _ringReplyPending has been consumed by the reply publish).
	_heatmapCallWasRing = true;
};
#ifdef DSERVER_RING_PHASE_PROF
uint64_t DarlingServer::Thread::takeRingPublishCycles() {
	// no lock: only the main loop touches this, in the same ringServiceThread call chain.
	uint64_t v = _ringPublishCycles;
	_ringPublishCycles = 0;
	return v;
};
#endif
#endif

void DarlingServer::Thread::_dispose() {
	threadLog.debug() << *this << ": dispose thread context" << threadLog.endLog;
	_selfReference = nullptr;
};

void DarlingServer::Thread::_scheduleRelease() {
	// schedule the duct-taped thread to be released
	// dtape_thread_release needs a microthread context, so we call it within a kernel microthread
	threadLog.debug() << *this << ": scheduling release" << threadLog.endLog;
	kernelAsync([self = shared_from_this()]() {
		if (self->_s2cPerformSempahore) {
			dtape_semaphore_destroy(self->_s2cPerformSempahore);
			self->_s2cPerformSempahore = nullptr;
		}
		if (self->_s2cReplySempahore) {
			dtape_semaphore_destroy(self->_s2cReplySempahore);
			self->_s2cReplySempahore = nullptr;
		}
		if (self->_s2cInterruptEnterSemaphore) {
			dtape_semaphore_destroy(self->_s2cInterruptEnterSemaphore);
			self->_s2cInterruptEnterSemaphore = nullptr;
		}
		if (self->_s2cInterruptExitSemaphore) {
			dtape_semaphore_destroy(self->_s2cInterruptExitSemaphore);
			self->_s2cInterruptExitSemaphore = nullptr;
		}
		dtape_thread_release(self->_dtapeThread);
		self->_dtapeThread = nullptr;
	});
};

void DarlingServer::Thread::_handleInterruptEnterForCurrentThread() {
	// perf#25a A0 (Part 3f, replaces the old "FIXME: does not work if suspended waiting for a
	// lock"): this function's fiber can SUSPEND mid-flight -- dtape_thread_sigexc_enter takes
	// thread_lock (a dtape mutex whose contention raw-suspends the microthread), and the
	// interrupted continuation resumed below can block again. When the fiber suspends, the OS
	// thread's doneWorking clears the thread_local currentThreadVar; when the fiber resumes
	// (possibly on a DIFFERENT OS thread: the main loop and the worker both run fibers), code
	// here that re-reads currentThreadVar dereferences an empty shared_ptr (captured SIGSEGV:
	// null + offsetof(_handlingInterruptedCall), cvstorm2 storm). Pin the thread in a local
	// `self` at entry, use it throughout, and re-assert the TLS after every potentially
	// suspending call so downstream TLS readers (dtape hooks) stay correct too.
	std::shared_ptr<Thread> self = currentThreadVar;

	{
		std::unique_lock lock(self->_rwlock);

		if (self->_pendingSavedReply) {
			if (self->_interrupts.top().savedReply) {
				throw std::runtime_error("Pending saved reply would overwrite saved reply");
			}

			self->_interrupts.top().savedReply = std::move(*self->_pendingSavedReply);
			self->_pendingSavedReply = std::nullopt;
			// A0 RPC TRACE: pendingSaved -> interruptTop promotion at interrupt_enter; the reply is now on
			// the interrupt stack and will flush at interrupt_exit (FLUSH-SAVED). Not sent here.
			DarlingServer::__rpctrace("PENDINGSAVED->INTERRUPTTOP htid=%d nstid=%lld", self->id(), (long long)self->nsid());
		}

		self->_interruptedForSignal = true;
	}

	dtape_thread_sigexc_enter(self->_dtapeThread);
	currentThreadVar = self; // may have suspended+migrated inside (thread_lock contention)

	// Extract the interrupted continuation into a LOCAL (fiber-stack) variable, not the old
	// thread_local: a fiber that suspends and migrates OS threads keeps its stack but not the
	// previous OS thread's TLS, and the syscall-return re-entry below must not read/clobber
	// whatever unrelated value the current OS thread's TLS happens to hold. Locals assigned
	// before getcontext() are restored consistently by the setcontext() re-entry.
	std::function<void()> localInterruptedContinuation = nullptr;
	{
		std::unique_lock lock(self->_rwlock);
		localInterruptedContinuation = self->_interruptedContinuation;
		self->_interruptedContinuation = nullptr;
	}

	// perf#25a A0 (Part 3b): sigexc_enter's clear_wait_internal -> thread_go -> thread_unblock
	// fires the dtape thread_resume hook, which mints a _resumePermit for this (currently
	// _running) thread. The interrupt path resumes the interrupted continuation SYNCHRONOUSLY
	// below (jumpToResume / interruptedContinuation), so that permit is already satisfied here.
	// If left set, it goes stale: the thread's NEXT genuine wait (e.g. the guest's retried
	// fork_wait_for_child) has its suspend() consume the stale permit and return immediately --
	// a spurious wakeup with wait_result still THREAD_WAITING, which panics
	// semaphore_convert_wait_result ("semaphore_block") and, in wait paths that tolerate it,
	// silently corrupts the wait protocol. Any permit present at this point can only refer to
	// resuming the interrupted context (doWork already consumed pre-existing permits at
	// dispatch), so consuming it here is exact.
	{
		std::unique_lock lock(self->_rwlock);
		self->_resumePermit = false;
	}

	self->_didSyscallReturnDuringInterrupt = false;
	getcontext(&self->_syscallReturnHereDuringInterrupt);

	// re-entered here either directly or via the interrupted call's syscall-return setcontext;
	// in the latter case the executing OS thread's TLS is already self (the fiber only runs
	// inside a doWork stint), but re-assert for the direct path after suspensions above.
	currentThreadVar = self;

	if (!self->_didSyscallReturnDuringInterrupt) {
		if (localInterruptedContinuation) {
			localInterruptedContinuation();
		} else if (self->_interrupts.top().interruptedCall) {
			self->_handlingInterruptedCall = true;
			self->_pendingCallOverride = true;
			self->jumpToResume(self->_interrupts.top().savedStack.base, self->_interrupts.top().savedStack.size);
		}
	} else if (self->_handlingInterruptedCall) {
#if DSERVER_ASAN
		const void* dummy;
		size_t dummy2;
		__sanitizer_finish_switch_fiber(nullptr, &dummy, &dummy2);
#endif

		self->_handlingInterruptedCall = false;
		self->_pendingCallOverride = false;
	}

	{
		std::unique_lock lock(self->_rwlock);

		if (self->_interrupts.top().savedStack.isValid()) {
			stackPool.free(self->_interrupts.top().savedStack);
		}

		self->_interruptedForSignal = false;
		self->_interrupts.top().interruptedCall = nullptr;
	}

	dtape_thread_sigexc_enter2(self->_dtapeThread);
	currentThreadVar = self;
};
