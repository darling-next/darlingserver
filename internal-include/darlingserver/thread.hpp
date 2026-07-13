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

#ifndef _DARLINGSERVER_THREAD_HPP_
#define _DARLINGSERVER_THREAD_HPP_

#include <memory>
#include <sys/types.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <stack>
#include <queue>

#include <darlingserver/message.hpp>
#include <darlingserver/duct-tape.h>
#include <darlingserver/logging.hpp>
#include <darlingserver/stack-pool.hpp>
#include <darlingserver/registry.hpp>

#include <ucontext.h>

struct DTapeHooks;

namespace DarlingServer {
	class Process;
	class Call;

	class Thread: public std::enable_shared_from_this<Thread>, public Loggable {
		friend class Process;
		friend class Call; // HACK, see call.cpp
		friend class Registry<Thread>;

	public:
		enum class RunState {
			Dead = dtape_thread_state_dead,
			Running = dtape_thread_state_running,
			Stopped = dtape_thread_state_stopped,
			Interruptible = dtape_thread_state_interruptible,
			Uninterruptible = dtape_thread_state_uninterruptible,
		};

	private:
		enum class DeferralState: uint8_t {
			/**
			 * The thread has not been told to defer execution.
			 */
			NotDeferred,
			/**
			 * The thread has been told to defer execution, but no executions have actually been deferred yet.
			 */
			DeferredNotPending,
			/**
			 * The thread has been told to defer execution and there are one or more executions that have actually been deferred.
			 */
			DeferredPending,
		};

		pid_t _tid;
		pid_t _nstid;
		EternalID _eid;
		std::shared_ptr<Process> _process;
		std::shared_ptr<Call> _pendingCall;
		Address _address;
		mutable std::shared_mutex _rwlock;
		StackPool::Stack _stack;
		// These are orthogonal lifecycle dimensions, not one enum state:
		// _running means a worker currently owns this microthread;
		// _suspended means it has a resumable context; and _resumePermit is a
		// coalesced wake request that bridges the transition between the two.
		bool _suspended = false;
		bool _resumePermit = false;
		// perf#25a A0: a dispatch (scheduleThread) can be popped by a worker and
		// enter doWork() while this microthread is still _running on another worker
		// (mid suspend()/doneWorking transition; _running clears only at the
		// doneWorking tail). doWork() cannot run it now, but must NOT silently drop
		// it: it records the owed re-run here, and the doneWorking tail (once
		// _running is cleared) reschedules exactly once. This complements
		// _resumePermit, which only bridges the wake-before-suspend direction.
		bool _rerunPending = false;
		ucontext_t _resumeContext;
		dtape_thread_t* _dtapeThread;
		std::function<void()> _continuationCallback = nullptr;
		bool _running = false;
		bool _terminating = false;
		std::shared_ptr<Call> _activeCall = nullptr;
		std::shared_ptr<Thread> _impersonating = nullptr;
		bool _processingSignal = false;
		bool _pendingCallOverride = false;
		dtape_semaphore_t* _s2cPerformSempahore = nullptr;
		dtape_semaphore_t* _s2cReplySempahore = nullptr;
		std::optional<Message> _s2cReply = std::nullopt;
		std::condition_variable_any _runningCondvar;
		DeferralState _deferralState = DeferralState::NotDeferred;
		uint32_t _bsdReturnValue = 0;
		bool _interruptedForSignal = false;
		std::function<void()> _interruptedContinuation = nullptr;
		ucontext_t _syscallReturnHereDuringInterrupt;
		bool _didSyscallReturnDuringInterrupt = false;
		bool _handlingInterruptedCall = false;
		dtape_semaphore_t* _s2cInterruptEnterSemaphore = nullptr;
		dtape_semaphore_t* _s2cInterruptExitSemaphore = nullptr;
		bool _deferReplyForS2C = false;
		std::optional<Message> _deferredReply = std::nullopt;

		// perf #18 D9 (dar-1il.4): sticky per-call heatmap transport latch, recorded at the recordCall
		// site. It must SURVIVE past pushCallReply (which consumes _ringReplyPending before recordCall
		// runs), so it is tracked separately here. _heatmapCallWasRing latches at dispatch (the generic
		// ring path sets it right after beginRingReply) and is reset when a fresh call becomes active.
		// Pure diagnostic: only READ when the heatmap is armed, but the bool write is unconditional and
		// trivial (a single store), so no hot-path branch is added when disarmed. perf #18 D14
		// (dar-1il.9): the former companion _heatmapCallDidS2c sticky bool was REMOVED -- caller-S2C is
		// now attributed PER-CALL at _s2cPerform via Metrics::recordCallerS2cFor(_activeCall), which
		// eliminates the cross-op leak the sticky latch caused on exec/teardown threads (D13 finding).
		bool _heatmapCallWasRing = false;

		struct InterruptContext {
			std::optional<Message> savedReply = std::nullopt;
			std::shared_ptr<Call> interruptedCall = nullptr;
			StackPool::Stack savedStack;
			int signal = 0;
		};
		std::stack<InterruptContext> _interrupts;
		std::queue<std::shared_ptr<Call>> _pendingInterrupts;
		std::optional<Message> _pendingSavedReply = std::nullopt;
		bool _dead = false;
		std::shared_ptr<Thread> _selfReference = nullptr;

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 (dar-dar6x4-perf-5dq.30): the shared-memory ring this thread attached (if
		// any), and the Monitor watching its wake eventfd. Held here so they live exactly as
		// long as the Thread and are torn down on death. _ringMonitor is removed + _ring reset
		// in notifyDead() so the mapping + eventfd are released with the thread.
		std::shared_ptr<class RingBuffer> _ring = nullptr;
		std::shared_ptr<class Monitor> _ringMonitor = nullptr;
		// P3: one-shot "next reply goes to the ring" state, set by beginRingReply() and consumed
		// by pushCallReply(). _ringReplyPending gates it; _ringReplySeq is the request seq the
		// reply must echo. Guarded by _rwlock like the rest of the reply state.
		bool _ringReplyPending = false;
		uint32_t _ringReplySeq = 0;
		// perf #18 P5-bulk (dar-1il.1): publish a complete reply Message onto the s2c ring if this
		// thread's current call is ring-originated (_ringReplyPending), else return false so the
		// caller sends it over UDS. Consumes _ringReplyPending. MUST be called with _rwlock held.
		// Used by BOTH pushCallReply() (the normal path) and the deferred-S2C reply flush in
		// _s2cPerform() -- a ring call that triggers an S2C upcall has its reply deferred, and the
		// flush MUST go back to the ring (not UDS) or the ring-waiting guest wedges forever.
		bool _publishReplyToRingLocked(Message& reply);

		// --- perf #18 P8 D3 (dar-1il.3.1.1): duplex-lane S2C state ----------------------------------
		// When _s2cPerform's iron conjunction guard passes, it publishes the S2C upcall into THIS
		// thread's ring duplex mailbox and parks the microthread fiber on _s2cReplySempahore -- EXACTLY
		// as the UDS path parks it (so the main loop keeps draining: the fiber suspends, control
		// returns to doWork()'s caller). The difference is the wakeup source: instead of a UDS S2C
		// reply arriving at callFromMessage, the main-loop ring drain (ringServiceThread) notices the
		// correlated duplex reply in the mailbox, copies it into _duplexReply*, consumes the slot, and
		// ups _s2cReplySempahore. These fields carry the in-flight correlation + the harvested result;
		// all guarded by _rwlock like the rest of the S2C state. The whole feature is env-gated and
		// only the synthetic selftest op reaches it in D3 (no real op rides duplex yet).
		bool     _duplexUpcallInFlight = false; // an upcall is published + we are parked on its reply
		// Lock-free mirror of _duplexUpcallInFlight so the hot main-loop drain can skip the _rwlock
		// entirely for the overwhelming common case (no duplex in flight on this thread). Set with
		// release AFTER arming the in-flight state, cleared with release after tearing it down; the
		// drain loads it relaxed and only takes the lock when it's true. Keeps Lane-1 cost at one
		// relaxed atomic load per thread per drain (no lock), so mach_reply_port p50 is untouched.
		std::atomic<bool> _duplexInFlightFast {false};
		uint32_t _duplexParentId = 0;           // correlation: which parent op this upcall belongs to
		uint32_t _duplexUpcallId = 0;           // correlation: the unique upcall id
		bool     _duplexReplyReady = false;     // the drain harvested a correlated reply for us
		int32_t  _duplexReplyStatus = 0;        // the guest's upcall result status
		uint32_t _duplexReplyArg = 0;           // the guest's echo result
		uint32_t _duplexSentinelSeq = 0;        // != 0: in-flight upcall is a synthetic sentinel parent;
		                                        // its ring request seq (the drain publishes the final reply)
		// --- perf #18 P8 D4 (dar-1il.3.2.1): REAL-op duplex parent state ----------------------------
		// Set by ringServiceThread when it dispatches a duplex-eligible REAL op (mach_port_deallocate from
		// a DUPLEX_CAP_DEALLOCATE-capable caller) onto the fiber: it marks that this thread's CURRENT call
		// is a duplex parent, so _s2cPerform routes its S2C upcall (the munmap) through the duplex MAILBOX
		// (publish + park the fiber on _s2cReplySempahore) instead of the UDS S2C send -- and so
		// _drainDuplexReply, on harvesting the correlated munmap reply, synthesizes the _s2cReply Message +
		// ups _s2cReplySempahore (the REAL fiber resume, vs the sentinel's publish-final-reply state
		// machine). Cleared when the call finishes. Distinct from _duplexSentinelSeq (synthetic, no fiber).
		bool     _ringDuplexParentActive = false; // this thread's current ring call is a duplex parent (real op)
		// perf #18 P8 D5 (dar-1il.3.2.2) boot-scoped proof: the current duplex parent is a vm_deallocate
		// proof op (drives the vmdealloc-specific counters); and a sticky flag recording whether its real
		// caller-S2C actually fired (read+cleared by the auto-disarm after the dispatch).
		bool     _ringDuplexVmdeallocProof = false;
		bool     _ringDuplexVmdeallocS2cFired = false;
		uint32_t _duplexRealUpcallNum = 0;         // the in-flight real S2C upcall's dserver_s2c_msgnum_* (0 == none)
		// perf #18 P8 D6 (caller-S2C sideband): CLOCK_MONOTONIC ns deadline for the in-flight duplex upcall.
		// Set when an upcall is published; the main-loop drain fails it closed (synthesize FAILED reply +
		// resume fiber + count timeout + clear + disarm) if no correlated reply arrives by the deadline.
		// Only read when an upcall is actually in flight (off the hot path). 0 == no deadline armed.
		uint64_t _duplexUpcallDeadlineNs = 0;
		// Monotonic per-thread allocator for (parent_id, upcall_id) so a stale reply from a prior
		// upcall can never be mistaken for the current one (pitfall #2 ABA). Starts at 1 (0 == "none").
		uint32_t _duplexNextId = 1;
		// Drain-side hook: if this thread has a duplex upcall in flight, check its mailbox for a
		// correlated reply; on a match harvest+consume+up the reply semaphore (returns true so the
		// caller counts it), on a correlation mismatch flag it + drop it (returns false). MUST be
		// called WITHOUT _rwlock held (it takes the lock); safe to call when nothing is in flight.
		bool _drainDuplexReply();
		// The guarded duplex delivery used by _s2cPerform: publishes the upcall + parks. Returns true
		// if the duplex path was taken (caller must NOT also do the UDS send), false if the guard
		// declined (caller proceeds with the verbatim UDS path). MUST hold _rwlock on entry; may
		// unlock/relock internally exactly like the UDS branch.
		bool _s2cTryDuplexLocked(uint32_t upcallOp, uint32_t arg, std::unique_lock<std::shared_mutex>& lock);
		// perf #18 P8 D4: publish a REAL munmap S2C upcall into the duplex mailbox for a duplex-parent
		// call. Same conjunction guard as _s2cTryDuplexLocked but for the MUNMAP shape + the typed
		// addr/len payload, and it requires DUPLEX_CAP_DEALLOCATE. Returns true if the upcall is in
		// flight (the caller then parks the fiber on _s2cReplySempahore exactly like the UDS S2C path);
		// false if the guard declines (the caller MUST take the verbatim UDS S2C path). Hold _rwlock.
		bool _s2cTryDuplexMunmapLocked(uint32_t s2cNumber, uint64_t address, uint64_t length, std::unique_lock<std::shared_mutex>& lock);
#ifdef DSERVER_RING_PHASE_PROF
		// perf #18 P6: scratch for the TSC cycles publishReply consumed during this call's
		// doWork(), so ringServiceThread can subtract them from the body window. One-shot.
		uint64_t _ringPublishCycles = 0;
#endif
#endif

		static void microthreadWorker();
		static void microthreadContinuation();

		friend struct ::DTapeHooks;

		std::optional<Message> _s2cPerform(Message&& call, dserver_s2c_msgnum_t expectedReplyNumber, size_t expectedReplySize);

		uintptr_t _mmap(uintptr_t address, size_t length, int protection, int flags, int fd, off_t offset, int& outErrno);
		int _munmap(uintptr_t address, size_t length, int& outErrno);
		int _mprotect(uintptr_t address, size_t length, int protection, int& outErrno);
		int _msync(uintptr_t address, size_t size, int sync_flags, int& outErrno);

		void _deferLocked(bool wait, std::unique_lock<std::shared_mutex>& lock);
		void _undeferLocked(std::unique_lock<std::shared_mutex>& lock);

		void _deactivateCallLocked(std::shared_ptr<Call> expectedCall);

		[[noreturn]]
		void jumpToResume(void* stack, size_t stackSize);

		void _dispose();
		void _scheduleRelease();

		static void _handleInterruptEnterForCurrentThread();

		static StackPool stackPool;

		void _setEternalID(EternalID eid);

	public:
		using ID = pid_t;
		using NSID = ID;

		struct KernelThreadConstructorTag {};

		Thread(std::shared_ptr<Process> process, NSID nsid, void* stackHint = nullptr);
		Thread(KernelThreadConstructorTag tag);
		~Thread() noexcept(false);

		void registerWithProcess();

		Thread(const Thread&) = delete;
		Thread& operator=(const Thread&) = delete;
		Thread(Thread&&) = delete;
		Thread& operator=(Thread&&) = delete;

		std::shared_ptr<Process> process() const;

		std::shared_ptr<Call> pendingCall() const;
		void setPendingCall(std::shared_ptr<Call> newPendingCall);

		std::shared_ptr<Call> activeCall() const;
		void makePendingCallActive();
		void deactivateCall(std::shared_ptr<Call> expectedCall);

		bool waitingForReply() const;

		/**
		 * The TID of this Thread as seen from darlingserver's namespace.
		 */
		ID id() const;

		/**
		 * The TID of this Thread as seen from within the container (i.e. launchd's namespace).
		 */
		NSID nsid() const;

		EternalID eternalID() const;

		Address address() const;
		void setAddress(Address address);

		void doWork();

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 P6.1 (dar-ohp): run a PROVEN-NON-BLOCKING Call to completion WITHOUT the
		// microthread fiber. doWork() always allocates a stack and makecontext/setcontext-swaps
		// onto a fiber so the call can suspend; for a tiny self-trap that never suspends (e.g.
		// mach_reply_port) that fiber machinery is ~half the hot-path cost. doWorkInline()
		// establishes the SAME duct-tape context (currentThreadVar + dtape_thread_entering, so
		// current_task()/current_thread() resolve identically) and calls the SAME processCall()
		// on the current (main-loop) stack, then runs the same completion cleanup.
		//
		// CONTRACT: the caller MUST guarantee the call never suspends (never calls suspend()).
		// If it does, there is no fiber to switch back to -> UB. Use ONLY for the ring fast-path
		// allowlist of no-block ops. Returns true if it ran inline to completion; false if it
		// declined (deferred/running/terminating/dead) so the caller can fall back to doWork().
		bool doWorkInline();

		// perf #18 P6.1 step 2 (dar-ohp): SURGICAL direct dispatch for EXACTLY mach_reply_port.
		// doWorkInline() still pays the generic RPC framing (rebuild a Message, callFromMessage
		// registry re-lookup, heap-allocate a Call, decode) before running the op. For the single
		// hottest no-arg trap we skip ALL of it: establish the SAME duct-tape context as
		// doWorkInline (currentThreadVar + dtape_thread_entering, so current_task() resolves), call
		// dtape_mach_reply_port() DIRECTLY (the identical primitive MachReplyPort::processCall uses
		// -- no reimplemented ipc_port_alloc), publish the {replyhdr.code=0}{uint32 port} reply
		// straight onto this thread's s2c ring + wake the guest, then the same completion cleanup.
		//
		// CONTRACT: callable ONLY from the ring service loop for this exact thread, with the c2s
		// request slot ALREADY consumed (consumer_advance) and a non-zero `seq`. The op never
		// suspends (same as doWorkInline). Returns true if it ran inline + published; false if it
		// declined (deferred/running/terminating/dead, or the ring publish failed) so the caller
		// can fall back to the generic step-1 callFromMessage + doWorkInline path.
		bool doMachReplyPortInline(uint32_t seq);
#endif

		// perf #2b (dar-dar6x4-perf-5dq.8): the main event loop runs cheap, non-blocking
		// RPCs inline via doWork() instead of paying a worker-thread wakeup. After an inline
		// doWork() returns, this reports whether the microthread is still suspended (i.e. the
		// call blocked and will be resumed on the worker pool) vs ran to completion.
		// Cheap, lock-protected read of the same _suspended flag doWork() itself consults.
		bool isCurrentlySuspended() const;

		/**
		 * NOTE: This currently only works if this thread is the current thread.
		 *       It will throw an error in all other cases.
		 */
		void suspend(std::function<void()> continuationCallback = nullptr, libsimple_lock_t* unlockMe = nullptr);
		void resume();
		void clearResumePermit();
		void terminate();

		void setThreadHandles(uintptr_t pthreadHandle, uintptr_t dispatchQueueAddress);

		void startKernelThread(std::function<void()> startupCallback);
		void setupKernelThread(std::function<void()> startupCallback);

		/**
		 * Pretend to be another thread for the purpose of running duct-taped code.
		 *
		 * This is useful, for example, to trick duct-taped code into thinking
		 * that it's running on a particular user microthread when in fact
		 * it is running on a kernel microthread.
		 *
		 * Pass `nullptr` to reset.
		 */
		void impersonate(std::shared_ptr<Thread> thread);

		/**
		 * The thread that this thread is impersonating.
		 */
		std::shared_ptr<Thread> impersonatingThread() const;

		void loadStateFromUser(uint64_t threadState, uint64_t floatState);
		void saveStateToUser(uint64_t threadState, uint64_t floatState);

		int pendingSignal() const;

		/**
		 * Sets the new pending signal for this thread and returns the previous one.
		 */
		int setPendingSignal(int signal);

		void processSignal(int bsdSignalNumber, int linuxSignalNumber, int code, uintptr_t signalAddress, uintptr_t threadStateAddress, uintptr_t floatStateAddress);

		void handleSignal(int signal);

		void setPendingCallOverride(bool pendingCallOverride);

		uintptr_t allocatePages(size_t pageCount, int protection, uintptr_t addressHint, bool fixed, bool overwrite);
		void freePages(uintptr_t address, size_t pageCount);
		uintptr_t mapFile(int fd, size_t pageCount, int protection, uintptr_t addressHint, size_t pageOffset, bool fixed, bool overwrite);
		void changeProtection(uintptr_t address, size_t pageCount, int protection);
		void syncMemory(uintptr_t address, size_t size, int sync_flags);

		void defer(bool wait = false);
		void undefer();
		void waitUntilNotRunning();
		void waitUntilRunning();

		/**
		 * @note Only to be used for BSD syscalls and only for the current thread!
		 */
		uint32_t* bsdReturnValuePointer();

		void pushCallReply(std::shared_ptr<Call> expectedCall, Message&& reply);

		RunState getRunState() const;

		void waitWhileUserSuspended(uintptr_t threadStateAddress, uintptr_t floatStateAddress);
		void sendSignal(int signal) const;

		/**
		 * Informs this Thread instance that the thread it was managing has died.
		 */
		void notifyDead();
		bool isDead() const;

#ifdef DSERVER_RING_TRANSPORT
		// perf #18: attach a validated shared-memory ring to this thread and start watching its
		// wake eventfd. Takes ownership of the ring + its Monitor; both are released in
		// notifyDead(). Replaces any prior ring (a thread attaches at most once in practice).
		void attachRing(std::shared_ptr<class RingBuffer> ring, std::shared_ptr<class Monitor> monitor);
		std::shared_ptr<class RingBuffer> ring() const;

		// perf #18 P3: redirect the NEXT reply this thread produces onto its s2c ring instead
		// of UDS. The server C2S service loop sets this (with the request's seq) right before
		// running a ring-originated Call through the normal Call path; pushCallReply() consults
		// it so the reply lands on the ring + wakes the guest via FUTEX_WAKE. One-shot: the
		// flag is consumed (cleared) by the reply. This is how a ring call reuses the entire
		// existing dispatch (and thus the dar-l8k UAF / dar-6x4 rwlock fixes) -- only the reply
		// SINK changes, not the execution path.
		void beginRingReply(uint32_t seq);

		// perf #18 P8 D3 (dar-1il.3.1.1): the synthetic duplex selftest kickoff. Issues ONE S2C ECHO
		// upcall to THIS thread's ring-parked caller via the GUARDED duplex publish (_s2cTryDuplexLocked
		// -- the same guard + mailbox path a real op will use in Phase E), marking the in-flight upcall
		// as a sentinel parent with the parent request's ring seq. Does NOT park: returns immediately
		// after publishing (the brief's "publish upcall + return"); _drainDuplexReply later harvests the
		// correlated reply + publishes the parent FINAL reply. Returns true if the duplex path was taken
		// (parent reply deferred to the drain), false if the guard declined -- in which case the CALLER
		// must publish the synthetic parent's failure reply now (never a fabricated success). Reached
		// ONLY from the sentinel parent op in ringServiceThread, gated by DARLING_SERVER_DUPLEX_SELFTEST.
		bool duplexSelftestUpcall(uint32_t arg, uint32_t seq);

		// perf #18 P8 D3: drain a correlated duplex reply for this thread if one is in flight + ready.
		// Called from the main-loop ring drain for every ring thread. Returns true if it harvested a
		// reply (woke the parked fiber). No-op (returns false) when nothing is in flight.
		bool drainDuplexReply();

		// perf #18 P8 D4 (dar-1il.3.2.1): mark/unmark this thread's CURRENT ring call as a duplex parent
		// (a real op -- mach_port_deallocate from a duplex-deallocate-capable caller). When set,
		// _s2cPerform routes its munmap S2C through the duplex mailbox (publish + fiber-park) and the
		// main-loop drain resumes the fiber. Set just before dispatching the op on the fiber, cleared
		// after doWork() returns. A no-op gate when the caller isn't duplex-deallocate-capable.
		void setRingDuplexParentActive(bool active);
		// True iff a duplex-deallocate-capable caller's ring is attached to this thread (the routing
		// precondition ringServiceThread checks before treating a deallocate as a duplex parent).
		bool duplexDeallocateCapable() const;
		// perf #18 P8 D5 (dar-1il.3.2.2): True iff a vm_deallocate-capable caller's ring is attached
		// (the per-op routing precondition for treating a vm_deallocate as a duplex parent). Keyed on
		// the specific VM_DEALLOCATE cap bit, distinct from the D4 deallocate bit.
		bool duplexVmDeallocateCapable() const;
		// perf #18 P8 D5 (dar-1il.3.2.2) boot-scoped proof: tag this thread's in-flight duplex parent as a
		// vm_deallocate proof op so _drainDuplexReply bumps the vmdealloc-specific counters (and records
		// whether a real caller-S2C fired, for the auto-disarm). Set/cleared around the proof dispatch.
		void setRingDuplexVmdeallocProof(bool active);
		// One-shot read+clear: did a real caller-S2C (UPCALL_MUNMAP) complete for the last vm_deallocate
		// proof dispatch? Drives the auto-disarm (spend a budget unit only on a proven cure).
		bool takeRingDuplexVmdeallocS2cFired();
#ifdef DSERVER_RING_PHASE_PROF
		// perf #18 P6: read + clear the publish-phase TSC cycles recorded during the last reply.
		uint64_t takeRingPublishCycles();
#endif
#endif

		/**
		 * @note Only to be used by direct XNU traps! (e.g. Mach IPC, psynch, etc.)
		 */
		[[noreturn]]
		static void syscallReturn(int resultCode);

		static std::shared_ptr<Thread> currentThread();

		/**
		 * Returns the Thread that corresponds to the given thread port in the current port space.
		 *
		 * @note This function may only be called from a microthread context.
		 */
		static std::shared_ptr<Thread> threadForPort(uint32_t thread_port);

		/**
		 * Schedules the given function to be called within a duct-taped kernel microthread.
		 */
		static void kernelAsync(std::function<void()> fn);

		/**
		 * Runs the given function on a duct-taped kernel microthread and waits for it to return.
		 */
		static void kernelSync(std::function<void()> fn);

		static void interruptDisable();
		static void interruptEnable();

		void logToStream(Log::Stream& stream) const;
	};
};

#endif // _DARLINGSERVER_THREAD_HPP_
