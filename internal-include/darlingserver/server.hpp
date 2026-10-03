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

#ifndef _DARLINGSERVER_SERVER_HPP_
#define _DARLINGSERVER_SERVER_HPP_

#include <map>
#include <string>
#include <sys/epoll.h>
#include <thread>

#include <darlingserver/message.hpp>
#include <darlingserver/workers.hpp>
#include <darlingserver/call.hpp>
#include <darlingserver/process-identity.hpp>
#include <darlingserver/registry.hpp>
#include <darlingserver/utility.hpp>
#include <darlingserver/monitor.hpp>

namespace DarlingServer {
	class DebugCollector;
	namespace Debug { class DebugConnections; }
	// A0 (perf#25a-hang) per-tid RPC trace sink. Env-gated (DARLING_SERVER_AUXLOG=1), defined in call.cpp,
	// shared with thread.cpp's reply-disposition sites. No-op when the env is unset. NOT default-on.
	__attribute__((format(printf, 1, 2)))
	void __rpctrace(const char* fmt, ...);

	// NOTE: server instances MUST be created with `new` rather than as a normal local/stack variable
	class Server {
		friend class Monitor;
		friend class Process;

	private:
		int _listenerSocket;
		std::string _prefix;
		int _prefixFD;
		pid_t _rootlessInitHostPID;
		std::string _socketPath;
		// perf #0 (dar-dar6x4-perf-5dq.6): dedicated stat socket. A SOCK_STREAM listener in
		// the ABSTRACT namespace (the server is in a private mount namespace, so a pathname
		// socket is unreachable from the host; the network namespace is shared, so an
		// abstract name works). On accept the server writes a one-shot JSON metrics snapshot
		// and closes. Lives on the same epoll loop, so producing the snapshot is single-
		// threaded and reads the atomic counters consistently. -1 if setup failed (the
		// server keeps running regardless -- metrics are best-effort observability).
		int _statListenerSocket = -1;
		// perf#30 FD-COURIER (PRODUCT, dar-gwn.7.7.5): ONE AF_UNIX socket per Linux guest process that
		// carries ONLY descriptor transfer (SCM_RIGHTS + a correlation envelope). It deliberately has no
		// callnum and no RPC body: descriptor delivery is a process-scoped channel, not a property of
		// each thread's RPC socket. The semantic half stays on the Ring/shared path and names the token.
		//
		// SOCK_SEQPACKET, not SOCK_STREAM: message boundaries are what let one persistent connection
		// deliver many descriptors with their ancillaries intact (a stream read can split the fds from
		// the envelope, and SCM_RIGHTS attaches to data, not to a byte count).
		//
		// The connection is NOT registered in epoll. Every lookup drains all connections non-blockingly
		// first, and the guest sends the descriptor BEFORE it publishes the request that names the token,
		// so the descriptor is already enqueued when the server resolves it -- for either arrival order,
		// with no blocking and no polling loop.
		int _fdCourierListenerSocket = -1;
		std::string _fdCourierPath; // abstract name (without the leading NUL)
		/** Accept every pending courier connection (non-blocking). */
		void _handleFdCourierConnection();
		/** Read every complete envelope waiting on every connection (non-blocking). */
		void _drainFdCourierMessages();
		bool _isFdCourierSocket(void* ptr);

		/**
		 * perf#30 FD-COURIER (SERVER->GUEST): hand ONE descriptor to the process that owns `pid`, on the
		 * process-scoped connection, and return the token that names it. Zero means the courier could not
		 * take it (no live connection for that process) and the caller must use its legacy path -- a
		 * token that resolves to nothing is worse than a descriptor on the old transport.
		 */

		struct FdCourierConnection {
			int socket = -1;
			pid_t peerPid = -1;   // SO_PEERCRED: the kernel's own answer for which process this is
			uint64_t received = 0;
			uint64_t sent = 0;    // bundles the SERVER handed to this process (reverse direction)
			// perf#30 (doc section 90): WHICH IMAGE this connection belongs to. A process has several courier
			// connections -- the loader and the guest image each carry their own -- and sending a bundle to
			// "the first connection with this pid" delivered descriptors to the wrong image (measured as a
			// guest-side MISS with stored=0 dropped=0). The discriminator is free and unambiguous: the
			// connection that delivered KIND_PROCESS_CONTROL is the LOADER's (only the loader creates the
			// process-control page), so any other connection for that pid is the guest image's.
			bool isLoader = false;
		};
		std::unordered_map<int, FdCourierConnection> _fdCourierConns;

		// A descriptor half that arrived before its semantic request did.
		struct FdCourierBundle {
			pid_t pid = -1;  // the pid the bundle ARRIVED under (the sending connection's), for the generation check
			int fd = -1;
			uint32_t kind = 0;
			uint64_t generation = 0;
		};
		// keyed by (pid, token): the token is the request's own name for this descriptor half
		// perf#30: the pending bundle and its waiter are keyed by TOKEN, not by pid. MEASURED: the guest sends
		// the descriptor with the same token the request names, but the server observes two different pids for
		// the same Linux process -- the page is keyed by the pid that mapped the region, while the bundle
		// arrives under the pid of the SENDING connection (`PARKED pid=672791 token=...` against
		// `bundle pid=672797 token=<same>`). Keying on pid therefore parked every descriptor forever: 11 of 13
		// transactions went to WAIT_FD and only 2 ever completed. The token is unique per send and is the
		// identity both halves already share; the pid stays as a field for the generation check.
		std::map<uint64_t, FdCourierBundle> _fdCourierPending;
		// the semantic half that arrived first: resumed with the descriptor when it lands
		struct FdCourierWaiter {
			pid_t pid = -1;
			std::function<void(int)> resume;
		};
		std::map<uint64_t, FdCourierWaiter> _fdCourierWaiters;
		// the incarnation each pid last declared, so a bundle from a DIFFERENT incarnation is rejected
		// rather than paired with the wrong process's request
		std::map<pid_t, uint64_t> _fdCourierGeneration;
		uint64_t _fdCourierPendingPeak = 0;

		// perf#30 PROCESS-CONTROL PLANE: the process-shared management page, mapped from the descriptor
		// the loader sends over the courier. One page per Linux process, rare operations only, ONE
		// outstanding serialized request -- so the server needs no queue and no second wake path: the
		// main loop already drains its transports, and it services these pages in the same pass.
		struct ProcessControlRegion {
			int fd = -1;
			void* map = nullptr;
			size_t size = 0;
			uint64_t requests = 0;
			// perf#30: the guest's attach-route counters are monotonic PER PROCESS. Publishing them by
			// storing (the first version) made the snapshot a sample of whichever process was serviced
			// last -- which is how a per-process pair read as a run total. The last observed values are
			// kept here so the metric accumulates the DELTA.
			uint32_t lastRouteOk = 0;
			uint32_t lastRouteNotReady = 0;
			uint32_t lastRouteRefused = 0;
			uint32_t lastRouteClaimedNoLane = 0;
			uint32_t lastRouteNoSlot = 0;
		};
		// perf#30 ProcessControlTxn: a SERVER-OWNED logical transaction. The page is only a
		// submission/completion mailbox; once a request is CLAIMED its correctness no longer depends on the
		// submitting image or thread remaining alive. The transaction owns the payload, the descriptor (after
		// the courier accepts it) and the completion target's identity -- never a raw guest pointer, an fd
		// number, or a Thread reference.
		struct ProcessControlTxn {
			pid_t pid = -1;
			uint64_t generation = 0;
			uint32_t seq = 0;
			uint32_t op = 0;
			uint64_t payload[4] = {};
			uint64_t fdToken = 0;
			uint32_t expectedFdKind = 0;
			int fd = -1;
			enum class State { CLAIMED, WAIT_FD, READY } state = State::CLAIMED;
			// Completion target identity. A late answer must never be published into a different page
			// incarnation, a reused mapping, or a later request that happens to use the same slot.
			void* pageMap = nullptr;
			size_t pageSize = 0;
			bool waitedForFd = false;
		};
		std::unordered_map<pid_t, std::shared_ptr<ProcessControlTxn>> _processControlTxns;

		// perf#30 (doc section 227): plane Calls that must run OUTSIDE the plane pass. MEASURED: running a synthesized
		// Call inline from that pass makes the pass wait for a guest thread that is itself waiting on the plane's single
		// slot, which is the deadlock of sections 218/223; arming the Call alone runs nothing, because a pending Call is
		// consumed by `doWork()`. The plane therefore queues the Call, returns (leaving the slot PENDING), and the
		// server's own loop drains the queue once the pass is over -- at which point the waiter is parked and no longer
		// on the critical path of starting that Call.
		std::vector<std::shared_ptr<Call>> _deferredPlaneCalls;

		// perf#30 (doc section 234): STALL DUMP. MEASURED motivation: every stall this cycle was read from counters
		// ("no reply for lane N"), never from the question the stall actually asks -- which thread is waiting on
		// which operation, and is it parked on a continuation or holding a pending call. The server could not answer
		// that at all: it had no way to enumerate live threads. This prints, bounded and on a timer, every live
		// process and thread with the state that decides a deadlock. Behind DARLING_SERVER_STALL_DUMP=1 so an
		// acceptance run is unchanged, and the loop's blocking epoll is made finite only under that hatch.
		bool _stallDumpEnabled = false;
		bool _stallDumpInitialized = false;
		uint64_t _servicedTotal = 0;
		uint64_t _stallDumpServicedTotal = 0;
		uint64_t _stallDumpLastProgressMs = 0;
		uint64_t _stallDumpLastPrintMs = 0;
		void _stallDumpNoteProgress(uint64_t servicedNow);
		void _maybeStallDump(uint64_t nowMs);
	public:
		void deferPlaneCall(std::shared_ptr<Call> call) { _deferredPlaneCalls.push_back(std::move(call)); }
		void runDeferredPlaneCalls() {
			if (_deferredPlaneCalls.empty()) { return; }
			std::vector<std::shared_ptr<Call>> batch;
			batch.swap(_deferredPlaneCalls);
			for (auto& call : batch) {
				if (!call) { continue; }
				if (auto t = call->thread()) {
					try {
						call->suppressReplyDelivery();
						t->noteServicedFromProcessControl();
						t->setPendingCall(call);
						t->doWork();   // consumes the pending Call and writes the plane completion through pushCallReply
					} catch (const std::exception& e) {
						// MEASURED: this was gated on a hatch that was OFF in the run that mattered, so the drain failed
						// silently and the only visible symptom was "Failed to checkin with darlingserver" -- an
						// instrument that hides its own failure is the class this work keeps recording. Unconditional
						// but BOUNDED (the first few failures, then a count), so a storm cannot flood a boot log.
						static int defFailures = 0;
						++defFailures;
						if (defFailures <= 4) {
							static DarlingServer::Log defLog("plane-deferred-exception");
							defLog.error() << "plane-deferred-exception n=" << defFailures
								<< " what=" << e.what() << defLog.endLog;
						}
					}
				}
			}
		}
	private:
		void _cancelProcessControlTxn(pid_t pid);
		std::unordered_map<pid_t, ProcessControlRegion> _processControl;
		void _serviceProcessControl();

		std::string _statSocketPath; // abstract name (without the leading NUL)
		// Darling native debugger epic: the read-only debug introspection ABI.
		// SOCK_STREAM listener in the ABSTRACT namespace for the same reason as the
		// stat socket (the server is in a private mount namespace, so a pathname
		// socket is host-unreachable). Same event loop, so a snapshot is produced
		// single-threaded and reads live state consistently. -1 if setup failed; the
		// server runs normally without it.
		int _debugListenerSocket = -1;
		std::string _debugSocketPath; // abstract name (without the leading NUL)
		std::unique_ptr<DebugCollector> _debugCollector;
		std::unique_ptr<Debug::DebugConnections> _debugConnections;
		int _epollFD;
		MessageQueue _inbox;
		MessageQueue _outbox;
		WorkQueue<std::shared_ptr<Thread>> _workQueue;
		bool _canRead = false;
		bool _canWrite = true;
		int _wakeupFD;
		int _timerFD;
		uint64_t _currentTimerDeadline = 0;
		std::mutex _timerLock;
		std::vector<std::shared_ptr<Monitor>> _monitors;
		std::vector<std::shared_ptr<Monitor>> _monitorsWaitingToDie;
		std::mutex _monitorsLock;

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 P4 (dar-dar6x4-perf-5dq.33): threads that own an attached ring. The main-loop
		// pre-epoll spin phase walks these to drain requests without an eventfd doorbell (the
		// hot path). weak_ptr so a dead thread's entry self-prunes. Guarded by its own mutex --
		// register/unregister happen on the main loop (RingAttach) and on thread death.
		std::vector<std::weak_ptr<Thread>> _ringThreads;
		std::mutex _ringThreadsLock;

		// perf#28c (STABLE per-process doorbell): the Ring doorbell belongs to ONE Linux guest
		// process INCARNATION, and its lifetime is anchored to that incarnation -- not to the
		// `DarlingServer::Process` object, which can be disposed while the guest still holds the
		// duplicate (measured: three distinct guest pids shared ONE kernel eventfd object via the
		// server-wide doorbell, and tying the object to Process::_dispose broke boot because the next
		// attach created a SECOND object whose dup the guest's loader discarded).
		//
		// The anchor is a pidfd for the guest's Linux pid: it names that incarnation exactly, it is
		// immune to PID reuse (a recycled pid gets a DIFFERENT pidfd, and a pidfd for a dead process
		// is readable), and it notifies the server when the incarnation exits.
		struct RingProcessWake {
			pid_t pid = -1;
			FD pidfd;                                  // anchors this incarnation; death is EPOLLIN
			FD doorbell;                               // the ONE eventfd object this process writes
			std::shared_ptr<Monitor> doorbellMonitor;  // readable -> drain
			std::shared_ptr<Monitor> deathMonitor;     // pidfd readable -> retire the entry
		};
		std::unordered_map<pid_t, std::shared_ptr<RingProcessWake>> _ringWakeRegistry;
		std::mutex _ringWakeRegistryLock;

		/** Look up this incarnation's wake object, creating it on first use (or after its death). */
		std::shared_ptr<RingProcessWake> _ringWakeFor(pid_t pid);
		/** Erase the entry and unregister both monitors. Safe from the death callback. */
		void _ringWakeRetire(pid_t pid);
		/** The wake scan; global by design (see the comment at the definition). */
		uint32_t _ringDrainAll();

		// perf#28 (ONE doorbell): the Ring wake channel is a SINGLE server-wide eventfd with a SINGLE
		// Monitor, replacing the per-lane eventfd + per-lane Monitor. Every guest Linux process receives
		// a dup of this one eventfd at each lane attach and its shared loader adopts the FIRST and closes
		// the rest, so the guest holds exactly ONE Ring wake fd per process no matter how many lanes or
		// images it opens, and the server holds exactly two fds for the whole transport.
		//
		// Why process-INDEPENDENT rather than one Monitor per Process: the two things the wake path
		// does -- the service scan (_drainRings) and the sleep-state publication (_setAllRingStates) --
		// were already process-independent before this change, so a per-process Monitor would add N
		// server fds without changing a single decision the doorbell makes. The guest remains one
		// doorbell per process, which is the property being bought.
		FD _ringDoorbellFD;
		std::shared_ptr<Monitor> _ringDoorbellMonitor;
		std::mutex _ringDoorbellLock;

		/** The Monitor body: consume the counter, then service every lane that has work. */
		void _ringDoorbellWake();
		/**
		 * PW4 (sleep-transition race gate): widen ONE window of the arm/sleep handshake so a test can
		 * inject a publish into it deterministically. `where` is "armed" (after ACTIVE_POLLING ends,
		 * before the SLEEP_ARMED store), "presleep" (after SLEEP_ARMED is visible, before the final
		 * rescan), "sleeping" (after SLEEPING_EPOLL is visible, before epoll_wait) or "prewait".
		 * Gated by DSERVER_RING_SLEEP_PROBE_US + DSERVER_RING_SLEEP_PROBE_AT; inert by default.
		 */
		void _ringSleepProbe(const char* where);
		/** EPOLLHUP on the doorbell == every guest that held a dup is gone. */
		void _ringDoorbellHangup(std::shared_ptr<Monitor> monitor);

		// Pre-epoll adaptive spin: drain every attached ring, publishing ACTIVE_POLLING while we
		// do. Returns the total number of requests serviced this pass. Bounded by the caller's
		// budget; never blocks. Defined in server.cpp.
		uint32_t _drainRings();
		// Publish a sleep state (DSERVER_RING_SRV_*) into every attached ring's control block.
		void _setAllRingStates(uint32_t state);

		// Spin-budget configuration, resolved once from DARLING_SERVER_SPIN_US / DARLING_SERVER_MODE.
		uint64_t _ringSpinNs = 0;     // how long to busy-poll rings before arming epoll (0 == low-power)
		bool _ringSpinResolved = false;
		void _resolveRingSpinBudget();
#endif

		void _worker(std::shared_ptr<Thread> thread);

		// perf #0: accept one stat client and write the JSON snapshot.
		void _handleStatConnection();

		friend struct ::DTapeHooks;

	public:
		// perf#30 ATTRIBUTION: does this process have a process-control page mapped, and is its transport
		// published? Used by the datagram-checkin attribution log: a checkin that arrived on a datagram for a
		// process that HAS a ready page is a guest that did not take the route; one with no page is a guest
		// that never established it. Those are different defects and this is what tells them apart.
		void* processControlPageFor(pid_t pid) const {
			auto it = _processControl.find(pid);
			if (it == _processControl.end() || it->second.map == nullptr) { return nullptr; }
			return it->second.map;
		}
		// perf#30 FD-COURIER (round 49, SERVER->GUEST): Call hands a returned descriptor back through this.
		uint64_t sendFdCourierBundleToGuest(pid_t pid, uint32_t kind, int fd, bool preferLoader = false, bool drainFirst = true);
		// perf#30 FD-COURIER (PRODUCT): the process-scoped descriptor channel's operational surface.
		// Public because a Call resolves its own descriptor half and a dying Process releases its own --
		// the STATE stays private, so there is still exactly one place it can be mutated.
		enum class FdCourierResult {
			Resolved,
			Missing,          // no descriptor half yet: the caller parks a continuation
			KindMismatch,     // the descriptor is for a different operation
			StaleGeneration,  // from another incarnation of this pid
			Malformed,        // wrong fd count / unknown kind / no usable descriptor
		};
		/** Locate one descriptor half, draining first; registers `onArrival` when it has not arrived. */
		FdCourierResult resolveFdCourierBundle(pid_t pid, uint64_t token, uint32_t kind, int* outFd,
		                                       std::function<void(int)> onArrival);
		/** Drop this incarnation's pending descriptors and waiters (process exit). */
		void _closeFdCourierForPid(pid_t pid);
		/** Live courier connection count: ONE per Linux process by construction. */
		size_t fdCourierConnectionCount() const { return _fdCourierConns.size(); }

		Server(
			std::string prefix,
			int prefixFD,
			pid_t rootlessInitHostPID = 0);
		~Server();

		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;
		Server(Server&&) = delete;
		Server& operator=(Server&&) = delete;

		void start();

		void monitorProcess(std::shared_ptr<Process> process);

		std::string prefix() const;
		int prefixFD() const;
		pid_t namespaceIDForPeer(pid_t peerHostPID, pid_t reportedNamespaceID) const;

		static Server& sharedInstance();

		void scheduleThread(std::shared_ptr<Thread> thread);

		void addMonitor(std::shared_ptr<Monitor> monitor);
		void removeMonitor(std::shared_ptr<Monitor> monitor);

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 P4: (un)register a ring-owning thread for the main-loop spin phase.
		void registerRingThread(std::shared_ptr<Thread> thread);
		void unregisterRingThread(std::shared_ptr<Thread> thread);

		// perf#28: get a NEW dup of the one process doorbell to send to a guest at lane attach,
		// creating + registering the doorbell and its Monitor on the first call. -1 on failure.
		int ringDoorbellDupForGuest();
		// perf#28d (round 49): the process doorbell is delivered to the guest ONCE per process
		// INCARNATION, not on every ring_attach reply. The guest's loader keeps the first descriptor and
		// answers every later ask from its own slot, so a re-attach needs no descriptor at all. Keyed by
		// (pid, generation): an exec keeps the pid but starts a fresh incarnation whose loader has no
		// slot yet, so that incarnation gets exactly one delivery.
		// perf#28d: ONE doorbell descriptor per process lifetime. Keyed by pid, NOT by the courier
		// generation: MEASURED (4th attempt), the generation MOVES during a boot (the plane's PING teaches
		// it after the first attaches), so a generation-keyed rule re-delivered on every change and sent 36
		// bundles. Cleared when the process's courier state is closed (Process::_dispose), which is exactly
		// when a new incarnation -- exec -- legitimately needs its own delivery.
		std::unordered_set<pid_t> _ringDoorbellDelivered;
		// perf#30 (violation A): a SECOND, independent guard. MEASURED: reusing _ringDoorbellDelivered for the early
		// page-registration delivery cancelled the LANE ATTACH's own delivery (`[mldr-seed] attach-rc ... wake=-1`),
		// so the runtime was left without a doorbell and shellspawn never became ready -- the boot that "broke" was
		// my guard, not the drain. The early delivery and the attach delivery are different consumers and each needs
		// its own once-per-process record.
		std::unordered_set<pid_t> _planeDoorbellDelivered;
		uint64_t _ringDoorbellGeneration(pid_t pid);

		// perf#28c: a NEW dup of the ONE doorbell of the guest Linux process INCARNATION `pid`
		// (created + registered on that incarnation's first lane attach). -1 on failure.
		int ringDoorbellDupFor(pid_t pid);
#endif

		void sendMessage(Message&& message);
	};
};

#endif // _DARLINGSERVER_SERVER_HPP_
