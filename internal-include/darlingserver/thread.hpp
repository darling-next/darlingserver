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
#include <optional>
#include <sys/types.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <stack>
#include <queue>

#include <darlingserver/message.hpp>
#include <darlingserver/ring-call-context.hpp>
#include <darlingserver/duct-tape.h>
#include <darlingserver/logging.hpp>
#include <darlingserver/stack-pool.hpp>
#include <darlingserver/registry.hpp>
#include <darlingserver/microthread-resume.hpp>

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
		// perf#30 (directive section 7): the cancellation state transitions are read-modify-write on three
		// bools shared with duct-tape, and __pthread_markcancel mutates them from another thread while the
		// target observes them at its own cancellation point. Their correctness used to rest on both running on
		// the same microthread fiber -- the serialization the management plane removes when it services a
		// cancellation check directly on a thread that is suspended inside the syscall asking. The lock lives
		// HERE, in server-side state, and not in the shared structure: the latter is linked into several images,
		// and a stale consumer of a changed shared layout is silent memory corruption.
		std::mutex _cancelLock;
		StackPool::Stack _stack;
		// Execution ownership is separate from the saved wait context and its
		// wake permit. Pending dispatch exists only while execution is owned.
		bool _suspended = false;
		bool _resumePermit = false;
		ucontext_t _resumeContext;
		// Validity of `_resumeContext`: true only between a getcontext() that captured it and a
		// setcontext() that consumed it. A Thread that never suspended (measured: a thread created
		// for an already-running post-exec image, and any interrupt stacked on a call that never
		// reached suspend()) has no captured context, and restoring it would fault inside glibc's
		// setcontext(). See darlingserver/interrupt-resume.hpp.
		bool _resumeContextValid = false;
		dtape_thread_t* _dtapeThread;
		std::function<void()> _continuationCallback = nullptr;
		MicrothreadExecution _execution;
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
		// site. It must SURVIVE past pushCallReply (which clears the call's ring context before recordCall
		// runs), so it is tracked separately here. _heatmapCallWasRing latches at dispatch (the generic
		// ring path sets when it attaches the Call's ring context) and is reset when a fresh call becomes active.
		// Pure diagnostic: only READ when the heatmap is armed, but the bool write is unconditional and
		// trivial (a single store), so no hot-path branch is added when disarmed. perf #18 D14
		// (dar-1il.9): the former companion _heatmapCallDidS2c sticky bool was REMOVED -- caller-S2C is
		// now attributed PER-CALL at _s2cPerform via Metrics::recordCallerS2cFor(_activeCall), which
		// eliminates the cross-op leak the sticky latch caused on exec/teardown threads (D13 finding).
	public:
		// perf#18 D9 accounting fix: the heatmap's transport tag must be set by whoever KNOWS the
		// transport. The generic ring service path takes a call off a lane, so it tags here; setting it
		// only in beginRingReply (a duplex-reply step) left ordinary ring-served calls counted as UDS.
		void noteServicedFromRing() { _heatmapCallWasRing = true; }
		// perf#30 PHASE-0 census fix: a call the management page synthesized did NOT arrive on any
		// datagram. MEASURED: without this it was counted in the UDS column (process_control_requests = 20
		// and exactly the 20 set_dyld_info/set_executable_path rows appeared as `uds`), which inflates
		// "legacy semantic UDS" by the plane's own traffic -- a measurement defect, not a transport one.
		void noteServicedFromProcessControl() { _heatmapCallWasProcessControl = true; }
	private:
		bool _heatmapCallWasRing = false;
		bool _heatmapCallWasProcessControl = false;

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
		// perf#26 RING-MACH-MSG: publish a complete reply onto the lane named by a CALL's transport
		// context, or return false so the caller sends it over UDS. MUST be called with _rwlock held.
		// There is deliberately no Thread-global reply sink: a blocking call suspends inside doWork()
		// (which then returns to the main loop), so only state that travels with the Call can still name
		// the lane when the reply is finally produced. See Call::RingContext.
		bool _publishReplyToRingContextLocked(const RingCallContext& ctx, Message& reply);

		// perf#27 M6 (completion replay safety). THE INVARIANT, stated without reference to any history
		// structure: the duplex reply slot is valid ONLY for the CURRENT in-flight upcall.
		//
		//   case A  slot ready and NO upcall in flight  -> unsolicited/stale. Reject, clear, and it must
		//           never block publication of the next upcall.
		//   case B  upcall in flight but the (parent, upcall) pair is someone else's -> mismatch. Reject,
		//           clear, keep waiting for the correct completion. A wrong reply must never be left sitting
		//           in the single slot.
		//   case C  pair matches the in-flight upcall -> exactly once: validate, consume, clear, close.
		//
		// The bounded history below is DIAGNOSTIC ONLY -- it classifies a case-A stale slot as a "known
		// duplicate of a recently closed completion" for the trace. Correctness must not depend on it:
		// case A is rejected whether or not the pair is in the history. (The M6b mutation completes more
		// than the window's worth of upcalls before replaying an old one precisely to prove that.)
		static constexpr unsigned kClosedHistory = 8;
		uint32_t _duplexClosedParent[kClosedHistory] = {0,0,0,0,0,0,0,0};
		uint32_t _duplexClosedUpcall[kClosedHistory] = {0,0,0,0,0,0,0,0};
		unsigned _duplexClosedNext = 0;
		void _duplexRecordClosed(uint32_t parentId, uint32_t upcallId);
		// Scrub an invalid reply slot per the invariant above. Returns true iff a slot was cleared.
		bool _duplexScrubReplySlot(dserver_ring_shm_t* cb, bool upcallInFlight);
		// The ring context of a reply stashed by the deferral branch of pushCallReply(), so its later
		// flush in _s2cPerform() still knows which lane (and seq) it belongs to.
		std::optional<RingCallContext> _deferredRingContext;

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
		uint64_t _duplexReplyValue = 0;         // ABI v6: the guest's 64-bit upcall result (mmap address)
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
		// perf#26: duplex-parent eligibility is NOT stored here any more. It is read from the ACTIVE
		// CALL's RingContext (Call::RingContext::duplexCapable), because a blocking parent suspends
		// inside doWork() -- a Thread flag cleared when doWork() returned would be gone exactly when the
		// caller-S2C arrives. The drain still resumes the fiber through _s2cReplySempahore as before.
		bool     _s2cUdsFallbackThisCall = false; // perf#26: the current mach_msg parent's S2C went over UDS
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
		// ABI v6: the anonymous-mmap sibling (fd < 0, offset == 0). Same mailbox, same parking.
		bool _s2cTryDuplexMmapLocked(uint32_t s2cNumber, uint64_t address, uint64_t length,
		                             uint32_t protection, uint32_t flags,
		                             std::unique_lock<std::shared_mutex>& lock);
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

		/**
		 * The semantic core of XNU's __pthread_canceled(action): reads and mutates THIS thread's cancellation
		 * state and returns the guest-facing code.
		 *
		 * It exists so that there is exactly ONE implementation of the semantics, reached by two transports:
		 * the ordinary PthreadCanceled Call (call.cpp) and the process management plane, which services this
		 * operation DIRECTLY on the target Thread. The management route must not synthesize a Call on the
		 * target, because for this operation the target IS the caller and it is suspended inside the syscall
		 * that is asking -- MEASURED as "Thread has both a pending call and a pending continuation
		 * (pending_call=31 ... suspended=1)". Deferral cannot help there; only not executing on that
		 * microthread can.
		 */
		int pthreadCanceled(int action);

		/**
		 * The other half of the same state machine: XNU's __pthread_markcancel(thread_port), which ARMS the
		 * pending bit from ANOTHER thread. It takes the same lock as pthreadCanceled, because the two are
		 * genuinely concurrent and the shared duct-tape structure holds three plain bools.
		 */
		int pthreadMarkcancel();

		/**
		 * A consistent read of the same three bits, under the same lock, for diagnostics (call.cpp traces a
		 * before/after pair around each of the two operations).
		 */
		void cancelStateSnapshot(bool* disabled, bool* pending, bool* canceled);

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


		// perf #2b (dar-dar6x4-perf-5dq.8): the main event loop runs cheap, non-blocking
		// RPCs inline via doWork() instead of paying a worker-thread wakeup. After an inline
		// doWork() returns, this reports whether the microthread is still suspended (i.e. the
		// call blocked and will be resumed on the worker pool) vs ran to completion.
		// Cheap, lock-protected read of the same _suspended flag doWork() itself consults.
		bool isCurrentlySuspended() const;
		// perf#30 (doc section 234): the OTHER half of the "pending call and pending continuation" invariant, which
		// the stall dump must report -- a thread parked on a continuation is invisible in every counter the server had.
		bool hasContinuation() const { return _continuationCallback != nullptr; };
		// perf#30 (doc section 236): the duct-tape thread identity, so a stall dump can be JOINED with the duct-tape
		// trace (`dtape.msgq`, `dtape.wait_timer`), which print only this pointer. Without it the two instruments
		// answer about different names for the same thread and neither can be read against the other.
		dtape_thread_t* dtapeThread() const { return _dtapeThread; };

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

		// perf#26 diagnostic: is a duplex upcall in flight for this thread? Read-only, for the SIGUSR1
		// state dump; takes the same _rwlock the reply paths do, so it cannot observe a torn tuple.
		bool duplexInFlight() const;

		// (No setRingDuplexParentActive: duplex eligibility is read from the active Call's
		// RingCallContext, so it survives a blocking suspend.)
		// True iff a duplex-deallocate-capable caller's ring is attached to this thread (the routing
		// precondition ringServiceThread checks before treating a deallocate as a duplex parent).
		bool duplexDeallocateCapable() const;
		// perf #18 P8 D5 (dar-1il.3.2.2): True iff a vm_deallocate-capable caller's ring is attached
		// (the per-op routing precondition for treating a vm_deallocate as a duplex parent). Keyed on
		// the specific VM_DEALLOCATE cap bit, distinct from the D4 deallocate bit.
		bool duplexVmDeallocateCapable() const;
		bool duplexMachMsgCapable() const;
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
