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

#define _GNU_SOURCE 1
#include <darlingserver/call.hpp>
#include <darlingserver/rpc-error-reply.hpp>
#include <darlingserver/push-reply-sync-pipe.hpp>
#include <darlingserver/server.hpp>
#include <darlingserver/process-identity.hpp>
#include <sys/uio.h>
#include <errno.h>
#include <cstring>

#include <darlingserver/logging.hpp>
#include <darlingserver/duct-tape.h>
#include <darlingserver/config.hpp>
#include <darlingserver/metrics.hpp>
#include <darlingserver/test-diagnostics.hpp>
#include <sys/fcntl.h>
#include <sys/syscall.h>
#include <darlingserver/s2c-trace.hpp>
#include <darlingserver/kqchan.hpp>
#include <system_error>
#include <cerrno>
#include <atomic>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdarg>
#include <sys/stat.h>
#ifdef DSERVER_RING_TRANSPORT
	#include <darlingserver/ring.hpp>
	#include <darlingserver/monitor.hpp>
	#include <darlingserver/utility.hpp>
	#include <sys/eventfd.h>
#endif

static DarlingServer::Log callLog("calls");

DarlingServer::Log DarlingServer::Call::rpcReplyLog("replies");

// A0 (perf#25a-hang) PER-TID RPC TRACE -- env-gated (DARLING_SERVER_AUXLOG=1, same gate + same log file
// as the kqchan AUXLOG so both traces interleave chronologically). The guest-side capture proved the
// stall is a lost/never-sent DGRAM RPC reply (server idle, every parked guest thread in recvmsg with
// Recv-Q=0). This trace names, per guest tid, EVERY call the server receives (RECV) and the DISPOSITION
// of its reply -- the four pushCallReply outcomes (SENT-UDS / SENT-RING / STASH-SAVED[interrupt] /
// STASH-DEFERRED[s2c]), each stash's matching FLUSH, the Call::sendReply fallback/error/direct paths.
// A stall then reads directly off the tape: a RECV whose reply STASHes and never FLUSHes, or that never
// produces any reply line at all, names the exact stuck call + drop site. Zero cost when unset (one
// relaxed-load bool). Writes to the prefix log file via O_APPEND (atomic per-line). NOT default-on;
// reverted before any non-instrumented build.
//
// Exposed with external linkage (not static) so thread.cpp's pushCallReply/flush sites -- the actual
// reply dispositions -- share one file-open and one timestamp base with the call.cpp RECV site.
static bool __rpctrace_enabled() {
	static std::atomic<int> cached{-1};
	int v = cached.load(std::memory_order_relaxed);
	if (v < 0) {
		const char* e = getenv("DARLING_SERVER_AUXLOG");
		v = (e && e[0] == '1') ? 1 : 0;
		cached.store(v, std::memory_order_relaxed);
	}
	return v != 0;
}

static int __rpctrace_fd() {
	static int fd = []() -> int {
		if (!__rpctrace_enabled()) {
			return -1;
		}
		std::string path = DarlingServer::Server::sharedInstance().prefix() + "/private/var/log/dserver-auxlog.txt";
		return open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	}();
	return fd;
}

__attribute__((format(printf, 1, 2)))
void DarlingServer::__rpctrace(const char* fmt, ...) {
	if (!__rpctrace_enabled()) {
		return;
	}
	int fd = __rpctrace_fd();
	if (fd < 0) {
		return;
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	char line[512];
	int n = snprintf(line, sizeof(line), "[RPCTRACE %ld.%06ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000);
	va_list ap;
	va_start(ap, fmt);
	if (n >= 0 && (size_t)n < sizeof(line)) {
		int m = vsnprintf(line + n, sizeof(line) - n, fmt, ap);
		if (m >= 0) {
			n += m;
		}
	}
	va_end(ap);
	if (n < 0 || (size_t)n >= (int)sizeof(line)) {
		n = sizeof(line) - 1;
	}
	line[n++] = '\n';
	(void)!write(fd, line, n);
}

// perf #18 D15a (dar-1il.10): static ring-eligibility classifier for the attach-timeline census.
// Mirrors the dispatch allowlist (DSERVER_RING_C2S_OPCODES, rpc-supplement.h) -- the SAME macro the
// ringServiceThread dispatch keys off -- so "eligible" here means exactly "this op would have ridden
// the ring had the process attached". Recon only; never gates real dispatch.
bool DarlingServer::Call::ringEligibleCallnum(uint32_t callNumber) {
#ifdef DSERVER_RING_TRANSPORT
#define DSERVER_RING_C2S_ELIGIBLE_CENSUS(op) || (callNumber == (uint32_t)dserver_callnum_##op)
	return (false DSERVER_RING_C2S_OPCODES(DSERVER_RING_C2S_ELIGIBLE_CENSUS));
#undef DSERVER_RING_C2S_ELIGIBLE_CENSUS
#else
	(void)callNumber;
	return false;
#endif
}


// perf#30 (doc section 219): THE REGISTRATION AND ARMING BLOCK, reachable WITHOUT building a Call.
//
// MEASURED (section 218/219): the management plane must be able to service a checkin when no guest thread can execute
// anything -- that is the plane's whole purpose -- and a handler that instead built a Call and dispatched its work
// onto a guest thread held the plane's single slot while waiting for a thread that was itself waiting on the plane.
// Substituting only `Checkin::processCall`'s tail for `doWork()` was tried and MEASURED WRONG: the failure moved from
// the dyld-info write to the executable-path write, because the ingest block below does much more than the Call's own
// body -- it takes the lifetime descriptor out of the message, builds and arms the Process and the Thread, registers
// the thread with the process, and records the message address. So the block itself is what both routes must share,
// which is what this function is.
std::pair<std::shared_ptr<DarlingServer::Process>, std::shared_ptr<DarlingServer::Thread>>
DarlingServer::Call::registerPeerForMessage(Message& requestMessage, dserver_rpc_callhdr_t* header, bool replyErrors,
	std::shared_ptr<Process> reuseProcess) {
	std::shared_ptr<Process> process = nullptr;
	std::shared_ptr<Thread> thread = nullptr;

		const auto namespaceID = Server::sharedInstance().namespaceIDForPeer(requestMessage.pid(), header->pid);

		// Now look up (and possibly create) the process and thread making this call.
		// perf#30 PROCESS-CONTROL PLANE: the side-by-side trace, so the RPC checkin and the plane's
		// synthesized one can be compared action for action.
		const bool traceCheckin = (getenv("DARLING_SERVER_COURIER_LOG") != NULL);

		// perf#30 (doc section 222): THE PLANE HAS NO PEER SOCKET, so `namespaceIDForPeer` is not its identity -- and MEASURED
	// the consequence: with it, the plane's registration created a Mach object that `ipc_port_destroy + 0x3c` could not
	// tear down. A caller that already knows the process (the plane's region belongs to it) passes it here, and then the
	// process is REUSED rather than reconstructed under a guessed namespace. The datagram path passes nothing and keeps
	// its own derivation exactly as before.
	process = reuseProcess ? reuseProcess : processRegistry().registerIfAbsent(namespaceID, [&]() {
			std::shared_ptr<Process> tmp = nullptr;

			if (TestDiagnostics::consumeFault("ingest.process_register_fail")) {
				return tmp;
			}

			int lifetimePipe = -1;


			if (header->number == dserver_callnum_checkin) {
				auto checkinCall = reinterpret_cast<dserver_rpc_call_checkin_t*>(header);
				if (checkinCall->body.lifetime_listener_pipe != -1) {
					lifetimePipe = requestMessage.extractDescriptorAtIndex(checkinCall->body.lifetime_listener_pipe);
					checkinCall->body.lifetime_listener_pipe = -1;
				}
			}

			if (traceCheckin) {
				static DarlingServer::Log traceLog("checkin-trace");
				traceLog.error() << "rpc-register-process pid=" << requestMessage.pid()
					<< " nsid=" << namespaceID << " lifetime_pipe=" << lifetimePipe
					<< " header_pid=" << header->pid << traceLog.endLog;
			}
			try {
				/* PROCESS-REGISTRATION RECORD, UNCONDITIONAL. The checkin and lane-attach records showed that the early-hang
				 * runs never register a THREAD; this record one step earlier says whether the PROCESS itself was ever
				 * created for that pid -- 'registered, then nothing' and 'never registered' are different failures with
				 * different owners (guest startup vs server ingress). Plain write(2,...), bounded. */
				{
					static unsigned g_srv_proc_records = 0;
					unsigned n = __atomic_fetch_add(&g_srv_proc_records, 1, __ATOMIC_RELAXED);
					if (n < 32) {
						char b[160];
						int l = snprintf(b, sizeof(b), "[srv-process-register #%u pid=%d callnum=%u nsid=%d]\n",
							n, (int)requestMessage.pid(), (unsigned)header->number, (int)namespaceID);
						if (l > 0) { (void)!write(2, b, (size_t)l); }
					}
				}
				tmp = std::make_shared<Process>(requestMessage.pid(), namespaceID, static_cast<Process::Architecture>(header->architecture), lifetimePipe);
			} catch (std::system_error e) {
				return tmp;
			}

			Server::sharedInstance().monitorProcess(tmp);
			return tmp;
		});

		if (TestDiagnostics::consumeFault("ingest.force_missing_process")) {
			TestDiagnostics::traceLine(
				"call.ingest_missing_process number=" + std::to_string(header->number) +
				" pid=" + std::to_string(header->pid) +
				" tid=" + std::to_string(header->tid) +
				" forced=1 code=" + std::to_string(-ESRCH)
			);
			process = nullptr;
		}

		if (!process) {
			callLog.error() << "Received call from non-existent process (number "
				<< header->number << "); replying -ESRCH instead of dropping" << callLog.endLog;

			// Don't drop silently: the guest thread is parked in recvmsg waiting for a
			// reply, and dropping leaves it hung forever (-> SEGV under a fork/signal
			// storm). Reply -ESRCH so the guest syscall returns. (dar-gwn.6.2)
			if (replyErrors) { sendErrorReplyFromHeader(header, requestMessage.address(), -ESRCH); }
			return { nullptr, nullptr };
		}

		thread = threadRegistry().registerIfAbsent(header->tid, [&]() {
			std::shared_ptr<Thread> tmp = nullptr;

			if (TestDiagnostics::consumeFault("ingest.thread_register_fail")) {
				return tmp;
			}

			void* stackHint = nullptr;

			if (traceCheckin) {
				static DarlingServer::Log traceLog("checkin-trace");
				traceLog.error() << "rpc-register-thread tid=" << header->tid
					<< " number=" << header->number << traceLog.endLog;
			}

			if (header->number == dserver_callnum_checkin) {
				auto checkinCall = reinterpret_cast<dserver_rpc_call_checkin_t*>(header);
				stackHint = reinterpret_cast<void*>(checkinCall->body.stack_hint);
			}

			try {
				tmp = std::make_shared<Thread>(process, header->tid, stackHint);
			} catch (std::system_error e) {
				return tmp;
			}

			tmp->setAddress(requestMessage.address());
			tmp->registerWithProcess();
			return tmp;
		});

		/* REGISTRATION RECORD, OUTSIDE THE 'if absent' LAMBDA. MEASURED: placed inside it, this never fired --

		 * a thread the loader created for a guest pthread is ALREADY in the registry by the time the checkin

		 * arrives, so the lambda is skipped. The point of the record is the identity link between the guest's own

		 * host tid (what the fault witness prints) and the server's view of that thread, so it belongs where every

		 * checkin passes, and it is filtered to the checkin callnum to stay quiet. */

		if (header->number == dserver_callnum_checkin) {

			static unsigned g_srv_reg_records = 0;

			unsigned n = __atomic_fetch_add(&g_srv_reg_records, 1, __ATOMIC_RELAXED);

			if (n < 64) {

				char b[160];

				int l = snprintf(b, sizeof(b),

					"[srv-checkin pid=%d tid=%d present_before=%d]\n",

					n, (int)requestMessage.pid(), (int)header->tid, (int)(thread != nullptr));

				if (l > 0) { (void)!write(2, b, (size_t)l); }

			}

		}


		if (!thread) {
			callLog.error() << "Received call from non-existent thread (number "
				<< header->number << "); replying -ESRCH instead of dropping" << callLog.endLog;

			// Same as the non-existent-process case: reply -ESRCH so the parked guest
			// thread's recvmsg returns instead of hanging forever. (dar-gwn.6.2)
			if (replyErrors) { sendErrorReplyFromHeader(header, requestMessage.address(), -ESRCH); }
			return { nullptr, nullptr };
		}

		thread->setAddress(requestMessage.address());

		if (process->id() != requestMessage.pid()) {
			throw std::runtime_error("System-reported message PID != darlingserver-recorded PID");
		}
	return { process, thread };
}

std::shared_ptr<DarlingServer::Call> DarlingServer::Call::callFromMessage(Message&& requestMessage) {
	if (requestMessage.data().size() < sizeof(dserver_rpc_callhdr_t)) {
		throw std::invalid_argument("Message buffer was too small for call header");
	}

	dserver_rpc_callhdr_t* header = reinterpret_cast<dserver_rpc_callhdr_t*>(requestMessage.data().data());
	std::shared_ptr<Call> result = nullptr;
	std::shared_ptr<Process> process = nullptr;
	std::shared_ptr<Thread> thread = nullptr;

	// first, make sure we know this call number
	switch (header->number) {
		case dserver_callnum_s2c:
		case dserver_callnum_push_reply:
		DSERVER_VALID_CALLNUM_CASES
			break;

		default:
			throw std::invalid_argument("Invalid call number");
	}

	if ((header->number & DSERVER_CALL_UNMANAGED_FLAG) == 0) {
		// perf#30 (doc section 219): the SAME block the plane calls, so the two routes cannot drift apart.
		auto registered = registerPeerForMessage(requestMessage, header, true, nullptr);
		process = registered.first;
		thread = registered.second;
		if (!process || !thread) {
			return nullptr;
		}
	}


	auto pidString = (process) ? (std::to_string(process->id()) + " (" + std::to_string(process->nsid()) + ")") : (std::to_string(header->pid) + " (-1)");
	auto tidString = (thread) ? (std::to_string(thread->id()) + " (" + std::to_string(thread->nsid()) + ")") : (std::to_string(header->tid) + " (-1)");
	callLog.debug() << "Received call #" << header->number << " (" << dserver_callnum_to_string(header->number) << ") from PID " << pidString << ", TID " << tidString << callLog.endLog;

	// A0 RPC TRACE: every call the server accepts, keyed by host tid (matches /proc/<tid> in the guest
	// capture) + guest nsid. The nsid pins WHICH guest thread; the host tid ties it to the recvmsg the
	// stall capture saw parked. s2c/push_reply are logged here too (they return early below) so the tape
	// shows the interrupt/S2C protocol traffic interleaved with the call it belongs to.
	DarlingServer::__rpctrace("RECV call=%u(%s) hpid=%d htid=%d nspid=%lld nstid=%lld",
		(unsigned)header->number, dserver_callnum_to_string(header->number),
		header->pid, header->tid,
		(long long)(process ? process->nsid() : -1),
		(long long)(thread ? thread->nsid() : -1));

	if (header->number == dserver_callnum_s2c) {
		// this is an S2C reply
		{
			std::unique_lock lock(thread->_rwlock);

			if (thread->_s2cReply) {
				throw std::runtime_error("Received S2C reply but thread already had one pending");
			}

			thread->_s2cReply = std::move(requestMessage);
		}

		dtape_semaphore_up(thread->_s2cReplySempahore);

		return nullptr;
	} else if (header->number == dserver_callnum_push_reply) {
		// this is a reply push
		// (used to send interrupted replies back to the server)

		auto pushReplyCall = reinterpret_cast<const dserver_rpc_call_push_reply_t*>(requestMessage.data().data());
		Message replyToSave(pushReplyCall->reply_size, 0);

		// Extract the reply-push synchronization pipe and take RAII ownership of it
		// IMMEDIATELY. The client's push-reply hook
		// (dserver-rpc-defs.c:__dserver_rpc_hooks_push_reply) blocks in a
		// `read()` on the read end of this pipe until we either write a byte to
		// our write end or close it (EOF). If ANY code below throws before we
		// resolve the pipe, the raw fd would leak (never written, never closed):
		// the client's read() would then block forever, the interrupted call's
		// reply would never be re-delivered, and the guest's recvmsg would hang
		// indefinitely (semaphore_timedwait -111 on siblings) while darlingserver
		// stays alive but idle. That is exactly the dar-gwn.1.7 flavor-C hang,
		// and the Server::start() throw-containment guard (which drops the
		// message on an uncaught throw) makes it a silent leak rather than a
		// crash. Binding the fd to an FD here guarantees it is always closed on
		// every path -- so the client's read() always returns (a byte on success,
		// EOF on failure) and never strands the guest. (dar-gwn.1.7)
		PushReplySyncPipe pipeDesc(requestMessage.extractDescriptorAtIndex(requestMessage.descriptors().size() - 1));

		if (!pipeDesc) {
			throw std::runtime_error("Failed to extract reply-push synchronization pipe");
		}

		if (!process->readMemory(pushReplyCall->reply, replyToSave.data().data(), pushReplyCall->reply_size)) {
			// pipeDesc's FD dtor closes the pipe -> client read() sees EOF and unblocks.
			throw std::runtime_error("Failed to read client-pushed reply body");
		}

		replyToSave.replaceDescriptors(requestMessage.descriptors());
		requestMessage.replaceDescriptors({});

		replyToSave.setAddress(requestMessage.address());

		{
			std::unique_lock lock(thread->_rwlock);
			if (thread->_pendingCall && thread->_pendingCall->number() == Call::Number::InterruptEnter) {
				// this means the client got interrupted after we had already sent a reply for the interrupted call,
				// the client saw this unexpected reply while waiting for interrupt_enter to respond and sent it back to us,
				// and we received both calls (interrupt_enter and push_reply) at the same time
				thread->_pendingSavedReply = std::move(replyToSave);
				// A0 RPC TRACE: pushed-back reply parked in _pendingSavedReply (flushed at InterruptEnter).
				DarlingServer::__rpctrace("PUSHREPLY-STASH htid=%d nstid=%lld slot=pendingSaved", thread->id(), (long long)thread->nsid());
			} else if (thread->_interrupts.empty()) {
				// The push_reply races the interrupt's lifetime: the interrupt was
				// already torn down (interrupt_exit popped it) by the time this
				// pushed-back reply arrived, so there is no saved-reply slot to
				// stash it in. The pushed reply IS the reply to the interrupted
				// call, and the client is still waiting for it -- so DON'T drop it
				// (that would hang the call forever). Send it straight back to the
				// client now. (dar-gwn.1.7)
				lock.unlock();
				callLog.debug() << *thread << ": push_reply arrived with no live interrupt; re-sending pushed reply directly" << callLog.endLog;
				// A0 RPC TRACE: pushed-back reply re-sent directly (no live interrupt to stash it in).
				DarlingServer::__rpctrace("PUSHREPLY-DIRECT htid=%d nstid=%lld dead=%d", thread->id(), (long long)thread->nsid(), (int)thread->isDead());
				if (!thread->isDead()) {
					Server::sharedInstance().sendMessage(std::move(replyToSave));
				}
				// signal the client's push hook so it can continue
				pipeDesc.acknowledge();
				return nullptr;
			} else {
				if (thread->_interrupts.top().savedReply) {
					throw std::runtime_error("Client-pushed reply overwriting existing saved reply");
				}
				thread->_interrupts.top().savedReply = std::move(replyToSave);
				// A0 RPC TRACE: pushed-back reply parked in _interrupts.top().savedReply (flushed at InterruptExit).
				DarlingServer::__rpctrace("PUSHREPLY-STASH htid=%d nstid=%lld slot=interruptTop", thread->id(), (long long)thread->nsid());
			}
		}

		callLog.debug() << *thread << ": Saved client-pushed reply (" << ((thread->_pendingSavedReply) ? "pending" : "normal") << ")" << callLog.endLog;

		// write a byte to the pipe so the caller can continue
		// (pipeDesc's FD dtor closes the fd when this scope exits)
		pipeDesc.acknowledge();

		return nullptr;
	}

	// perf #18 P8 D8 (dar-1il.3.2.x): mach_msg_overwrite SHAPE CENSUS. Pure measurement, no behavior
	// change: when armed (DARLING_SERVER_MSG_CENSUS=1, set once on a warm server), classify each
	// mach_msg_overwrite by send/receive semantics (from the inline RPC args, free) and -- for the
	// send path -- by the message body's complex/descriptor shape (one cheap readMemory of the header,
	// skipped entirely when the census is off so the hot path is byte-identical). The goal is to size
	// the reclaimable fraction of the ~19% msg_overwrite hotness BEFORE designing any ring migration.
	if (header->number == dserver_callnum_mach_msg_overwrite &&
	    Metrics::shared().msgCensusOn.load(std::memory_order_relaxed) &&
	    requestMessage.data().size() >= sizeof(dserver_rpc_call_mach_msg_overwrite_t)) {
		auto* mc = reinterpret_cast<const dserver_rpc_call_mach_msg_overwrite_t*>(header);
		const int32_t option = mc->body.option;
		const bool send = (option & 0x1) != 0; // MACH_SEND_MSG
		Metrics::MsgComplexClass cclass = Metrics::MsgComplexClass::Unknown;
		// mach_msg_header_t is 24 bytes in the user ABI (msgh_bits + size + remote/local/voucher port
		// NAMES [4B each] + id), msgh_bits at offset 0. We use explicit sizes here so this stays free of
		// the XNU message.h type (not reliably in scope in a darlingserver TU).
		static const uint32_t kMsgHeaderSize = 24u;
		static const uint32_t kMachMsghBitsComplex = 0x80000000u;
		if (send && mc->body.send_size >= kMsgHeaderSize && process) {
			// Read msgh_bits to tell simple-vs-complex; for a complex message classify by the first
			// descriptor's type. No mutation, one or two small reads, skipped entirely when census off.
			uint32_t msghBits = 0;
			int rc = 0;
			if (process->readMemory((uintptr_t)mc->body.msg, &msghBits, sizeof(msghBits), &rc)) {
				if ((msghBits & kMachMsghBitsComplex) == 0) {
					cclass = Metrics::MsgComplexClass::Simple;
				} else {
					// complex: classify by the FIRST descriptor's type (the dominant shape signal).
					// layout: header(24) | mach_msg_body_t{ uint32 descriptor_count } | desc[0]...
					// The `type` field is a :8 bitfield that, in every user descriptor variant (port /
					// ool32 / ool64 / ool_ports / guarded_port), is the HIGH byte of the 4-byte word at
					// byte offset 8 of the descriptor (after the 4/8-byte address-or-name + a 4-byte
					// size-or-pad word). Holds for both the 32- and 64-bit user ABIs, so it needs no
					// architecture branch. We read 12 bytes of desc[0] to reach it.
					uint8_t dbuf[12];
					const uintptr_t descStart = (uintptr_t)mc->body.msg + kMsgHeaderSize + sizeof(uint32_t);
					if (mc->body.send_size >= kMsgHeaderSize + sizeof(uint32_t) + sizeof(dbuf) &&
					    process->readMemory(descStart, dbuf, sizeof(dbuf), &rc)) {
						uint32_t typeWord;
						__builtin_memcpy(&typeWord, dbuf + 8, sizeof(typeWord));
						uint32_t dtype = (typeWord >> 24) & 0xffu;
						// MACH_MSG_PORT_DESCRIPTOR=0, OOL=1, OOL_PORTS=2, OOL_VOLATILE=3, GUARDED_PORT=4
						if (dtype == 1 || dtype == 3) {
							cclass = Metrics::MsgComplexClass::ComplexOol;       // OOL / OOL_VOLATILE memory
						} else if (dtype == 0 || dtype == 2 || dtype == 4) {
							cclass = Metrics::MsgComplexClass::ComplexPort;      // port / ool-ports / guarded-port
						} else {
							cclass = Metrics::MsgComplexClass::ComplexOther;
						}
					} else {
						cclass = Metrics::MsgComplexClass::ComplexOther;
					}
				}
			} // else: Unknown (header read failed) -> counted as msg_census_hdr_read_fail
		}
		Metrics::shared().recordMsgOverwriteCensus(option, mc->body.send_size, mc->body.rcv_size, mc->body.timeout, cclass);
	}

	// finally, let's construct the call class

	#define CALL_CASE(_callName, _className) \
		case dserver_callnum_ ## _callName: { \
			if (requestMessage.data().size() < sizeof(dserver_rpc_call_ ## _callName ## _t)) { \
				throw std::invalid_argument("Message buffer was too small for dserver_call_" #_callName "_t"); \
			} \
			result = std::make_shared<_className>(thread, reinterpret_cast<dserver_rpc_call_ ## _callName ## _t*>(header), std::move(requestMessage)); \
		} break;

	switch (header->number) {
		DSERVER_CONSTRUCT_CASES

		default:
			throw std::invalid_argument("Invalid call number");
	}

	#undef CALL_CASE

	if (thread) {
		try {
			thread->setPendingCall(result);
		} catch (const std::exception& ex) {
			// setPendingCall throws "pending call overwritten while active" when a
			// non-interrupt call races a still-pending call on the same thread (seen
			// under the Homebrew fork/signal storm). Previously this unwound to the
			// Server::start() guard, which dropped the message silently -> the guest
			// thread that sent THIS call stayed parked in recvmsg forever (-> SEGV under
			// the storm). Reply -EAGAIN here instead, so that guest syscall returns and
			// the guest can retry, while leaving the genuinely-pending call untouched.
			// (dar-gwn.6.2)
			callLog.error() << "setPendingCall rejected call (number " << header->number
				<< "): " << ex.what() << "; replying -EAGAIN instead of dropping" << callLog.endLog;
			result->sendBasicReply(-EAGAIN);
			return nullptr;
		}
		return result;
	} else {
		Thread::kernelAsync([result]() {
			// Contain a throwing processCall so it cannot terminate the whole server
			// (see the matching guard in Thread::microthreadWorker). This path has no
			// client thread to reply to, so just log and drop.
			try {
				result->processCall();
			} catch (const std::system_error& err) {
				callLog.error() << "Uncaught std::system_error from kernel-async processCall (call "
					<< DarlingServer::Call::callNumberToString(result->number()) << "): " << err.what()
					<< " (code " << err.code().value() << ")" << callLog.endLog;
				TestDiagnostics::traceLine(
					"call.kernel_async_exception kind=system number=" +
					std::to_string(static_cast<int>(result->number())) +
					" code=" + std::to_string(err.code().value())
				);
			} catch (const std::exception& ex) {
				callLog.error() << "Uncaught exception from kernel-async processCall (call "
					<< DarlingServer::Call::callNumberToString(result->number()) << "): " << ex.what() << callLog.endLog;
				TestDiagnostics::traceLine(
					"call.kernel_async_exception kind=std number=" +
					std::to_string(static_cast<int>(result->number()))
				);
			} catch (...) {
				callLog.error() << "Uncaught non-std exception from kernel-async processCall (call "
					<< DarlingServer::Call::callNumberToString(result->number()) << ")" << callLog.endLog;
				TestDiagnostics::traceLine(
					"call.kernel_async_exception kind=unknown number=" +
					std::to_string(static_cast<int>(result->number()))
				);
			}
		});
		return nullptr;
	}
};

DarlingServer::Call::Call(std::shared_ptr<Thread> thread, Address replyAddress, dserver_rpc_callhdr_t* callHeader):
	_thread(thread),
	_replyAddress(replyAddress),
	_header(*callHeader)
	{};

DarlingServer::Call::~Call() {};

std::shared_ptr<DarlingServer::Thread> DarlingServer::Call::thread() const {
	return _thread.lock();
};

void DarlingServer::Call::sendBasicReply(int resultCode) {
	Message reply(sizeof(dserver_rpc_replyhdr_t), 0);
	reply.setAddress(_replyAddress);
	auto replyStruct = reinterpret_cast<dserver_rpc_replyhdr_t*>(reply.data().data());
	replyStruct->number = _header.number;
	replyStruct->code = resultCode;
	sendReply(std::move(reply));
};

void DarlingServer::Call::sendBSDReply(int resultCode, uint32_t returnValue) {
	throw std::runtime_error("This call cannot send a BSD reply");
};

bool DarlingServer::Call::isXNUTrap() const {
	return false;
};

bool DarlingServer::Call::isBSDTrap() const {
	return false;
};

bool DarlingServer::Call::publishReplyToRingContext(const RingCallContext& ctx, Message& reply) {
#ifdef DSERVER_RING_TRANSPORT
	if (!ctx.ring) {
		return false;
	}
	const auto& bytes = reply.data();
	if (bytes.size() < sizeof(dserver_rpc_replyhdr_t)) {
		return false; // malformed (too short) -> caller UDS-falls-back
	}
	const dserver_rpc_replyhdr_t* rhdr = reinterpret_cast<const dserver_rpc_replyhdr_t*>(bytes.data());
	const uint8_t* body = bytes.data() + sizeof(dserver_rpc_replyhdr_t);
	uint32_t bodyLen = static_cast<uint32_t>(bytes.size() - sizeof(dserver_rpc_replyhdr_t));
	// The context's OWN reference: the lane incarnation that carried the request outlives the call.
	auto ring = ctx.ring;
	if (!ring->publishReply(ctx.seq, static_cast<uint32_t>(rhdr->number), rhdr->code, body, bodyLen)) {
		// s2c full: fall back to a UDS reply so the guest still gets its answer.
		Metrics::shared().ringS2cFull.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	ring->wakeGuest();
	return true;
#else
	(void)ctx;
	(void)reply;
	return false;
#endif
};

uint64_t DarlingServer::Call::sendFdCourierToGuest(pid_t pid, uint32_t kind, int fd) {
	return Server::sharedInstance().sendFdCourierBundleToGuest(pid, kind, fd);
}

void DarlingServer::Call::publishFinalReply(Message&& reply) {
#ifdef DSERVER_RING_TRANSPORT
	// The Call's own route first: a detached call still owns where its answer goes.
	if (const auto* ctx = ringContext()) {
		if (publishReplyToRingContext(*ctx, reply)) {
			DarlingServer::__rpctrace("SENT-RING-DETACHED call=%u", (unsigned)number());
			return;
		}
	}
#endif
	DarlingServer::__rpctrace("SENT-FALLBACK-DETACHED call=%u", (unsigned)number());
	Server::sharedInstance().sendMessage(std::move(reply));
};

void DarlingServer::Call::sendReply(Message&& reply) {
	// A0 RPC TRACE: the non-pushCallReply reply funnel -- thread-expired fallback (generated _sendReply
	// when _thread.lock() fails), error replies (sendErrorReplyFromHeader), and the push_reply
	// no-live-interrupt direct re-send. No thread handle here; key by the reply's own header number so
	// the tape still names which call this reply belongs to.
	if (reply.data().size() >= sizeof(dserver_rpc_replyhdr_t)) {
		auto* rh = reinterpret_cast<const dserver_rpc_replyhdr_t*>(reply.data().data());
		DarlingServer::__rpctrace("SENT-FALLBACK call=%u code=%d", (unsigned)rh->number, (int)rh->code);
	} else {
		DarlingServer::__rpctrace("SENT-FALLBACK (undersized reply)");
	}
	Server::sharedInstance().sendMessage(std::move(reply));
};

void DarlingServer::Call::sendErrorReplyFromHeader(const dserver_rpc_callhdr_t* header, Address replyAddress, int code) {
	// Build a minimal reply (just the reply header) from the raw call header. We don't
	// have a Call object here, so we can't size the reply to the specific call's reply
	// struct -- but the guest's RPC wrapper will still return (with -ECOMM for a call
	// that expected a larger reply body, or with `code` for a body-less call) rather
	// than blocking forever in recvmsg. The point is to UNBLOCK the parked guest thread.
	Message reply(sizeof(dserver_rpc_replyhdr_t), 0);
	reply.setAddress(replyAddress);
	auto replyStruct = reinterpret_cast<dserver_rpc_replyhdr_t*>(reply.data().data());
	*replyStruct = rpcErrorReplyHeaderFromCall<dserver_rpc_replyhdr_t>(header, code);
	sendReply(std::move(reply));
};

//
// call processing
//

/*
 *
 * A note about RPC wrappers:
 *
 * The auto-generated RPC wrappers provide both client-side wrappers as well as server-side wrappers.
 * The server-side wrappers automatically handle a few things like replies and descriptors.
 *
 * Replies:
 * The RPC wrappers provide a custom `_sendReply` method specific to each call class.
 * This method takes the result/status code as its first parameter followed by the return parameters
 * specified in the call interface. When a call is done processing, it simply calls `_sendReply` with the necessary
 * parameters and the RPC wrappers will take care of setting up the message and loading it onto the reply queue
 * for the server to send it out.
 *
 * Descriptors:
 * The RPC wrappers automatically handle ownership of descriptors, both incoming and outgoing.
 *
 * Incoming descriptors are extracted from the message and ownership is moved into the call instance.
 * The call processing code can use the descriptor however it likes while the call instance is still alive.
 * If it would like to move ownership out of the call instance, it can set the descriptor in the `_body` to `-1`.
 * Descriptors still left in the `_body` when the call instance is destroyed are automatically closed.
 *
 * Ownership of outgoing descriptors is passed into the reply message. In other words, when a descriptor
 * is given to `_sendReply`, the call instance loses ownership of that descriptor. If the call instance
 * would like to retain ownership, it should `dup()` the descriptor and pass the `dup()`ed descriptor to `_sendReply` instead.
 *
 */

void DarlingServer::Call::Checkin::processCall() {
	/* UNCONDITIONAL CHECKIN RECORD. MEASURED NEED: the server's own checkin attribution exists but is gated by
	 * DARLING_SERVER_COURIER_LOG, and this stage measured that a server-side environment variable is not reliably
	 * present (the gated S2C trace produced nothing where an unconditional write did). The question this answers is
	 * whether the workload's guest threads check in by this path at all, and with which tid -- the same tid the
	 * loader's fault witness prints. Plain write(2,...), bounded. */
	{
		static unsigned g_srv_checkin_records = 0;
		unsigned n = __atomic_fetch_add(&g_srv_checkin_records, 1, __ATOMIC_RELAXED);
		if (n < 64) {
			void* pcPage = DarlingServer::Server::sharedInstance().processControlPageFor(_header.pid);
			uint32_t pcReady = 0;
			if (pcPage != nullptr) {
				pcReady = __atomic_load_n(&static_cast<dserver_process_control*>(pcPage)->transport_ready, __ATOMIC_ACQUIRE);
			}
			char b[176];
			int l = snprintf(b, sizeof(b), "[srv-checkin #%u pid=%d tid=%d fork=%d page=%d ready=%u]\n",
				n, (int)_header.pid, (int)_header.tid, (int)(_body.is_fork ? 1 : 0),
				(int)(pcPage != nullptr ? 1 : 0), (unsigned)pcReady);
			if (l > 0) { (void)!write(2, b, (size_t)l); }
		}
	}

	// the Call instance creation already took care of registering the process and thread.

	// perf#30 ATTRIBUTION: which checkins still arrive on a DATAGRAM, and from whom. MEASURED need: 167 of 332
	// checkins were still on the datagram after three guest sites were converted, and the site counters tried
	// for this never incremented. The server sees every datagram checkin regardless of the guest site that
	// sent it, so this is the instrument that can actually attribute them: it names the pid, the tid, whether
	// it is a fork child, and whether the caller had a lifetime descriptor.
	if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
		static DarlingServer::Log ciLog("checkin-attrib");
		void* pcPage = DarlingServer::Server::sharedInstance().processControlPageFor(_header.pid);
		uint32_t pcReady = 0;
		if (pcPage != nullptr) {
			pcReady = __atomic_load_n(&static_cast<dserver_process_control*>(pcPage)->transport_ready, __ATOMIC_ACQUIRE);
		}
		ciLog.error() << "uds-checkin pid=" << _header.pid << " tid=" << _header.tid
			<< " fork=" << (_body.is_fork ? 1 : 0)
			<< " lifetime=" << (int)_body.lifetime_listener_pipe
			<< " page=" << (pcPage != nullptr ? 1 : 0) << " page_ready=" << pcReady
			<< ciLog.endLog;
	}

	// perf #0 (dar-dar6x4-perf-5dq.6): count checkins, and fork-checkins specifically.
	// The fork checkin is the per-fork synchronous round-trip whose latency dominates
	// fork-heavy builds (the dar-l3a slowness); fork_latency_pXX is the headline number
	// each perf fix must drive down.
	auto& metrics = Metrics::shared();
	metrics.checkins.fetch_add(1, std::memory_order_relaxed);
	if (_body.is_fork) {
		metrics.forks.fetch_add(1, std::memory_order_relaxed);
	}
	uint64_t startUs = Metrics::nowMonoUs();

	int code = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			// Main-thread re-checkin classifies exec independently of pipe
			// notification delivery; fork checkin alone notifies the parent.
			process->notifyCheckin(static_cast<Process::Architecture>(_header.architecture),
				ProcessIdentity::isMainThread(thread->nsid(), thread->id(), process->nsid(), process->id()),
				_body.is_fork);
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	// perf #0: record fork-checkin latency (the handler may block coordinating with the
	// parent, so this captures the real per-fork cost).
	if (_body.is_fork) {
		uint64_t now = Metrics::nowMonoUs();
		metrics.forkLatency.record((now >= startUs) ? (now - startUs) : 0);
	}

	_sendReply(code);
};

void DarlingServer::Call::Checkout::processCall() {
	int code = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			// perf#30 FD-COURIER: the descriptor half of this call travels on the process-scoped
			// SCM_RIGHTS courier, so the request itself carries no ancillary data and can ride the lane.
			// Resolve the token here, before any side effect, so that a request whose descriptor half is
			// missing cannot silently look like the no-descriptor form of checkout (which means "this
			// thread is going away" -- a completely different operation).
			if (_body.exec_listener_pipe < 0 && _body.fd_token != 0) {
				int courierFd = -1;
				std::weak_ptr<Thread> weakThread = thread;
				// Shared with the continuation: the descriptor and the token both have to survive the
				// suspension, and neither may live on the stack of the frame that parks the call.
				auto fdBox = std::make_shared<int>(-1);
				auto result = Server::sharedInstance().resolveFdCourierBundle(process->id(), _body.fd_token,
					DSERVER_FD_COURIER_KIND_CHECKOUT_FD, &courierFd,
					[fdBox, weakThread](int arrivedFd) {
						// The semantic half arrived first. The descriptor's arrival is what completes it,
						// so wake the thread whose continuation owns the rest of this call -- never inline
						// on the event loop, and never by re-entering the handler from here.
						*fdBox = arrivedFd;
						if (auto strongThread = weakThread.lock()) {
							strongThread->resume();
						}
					});

				switch (result) {
					case Server::FdCourierResult::Resolved:
						_body.exec_listener_pipe = courierFd;
						break;
					case Server::FdCourierResult::Missing:
						// No descriptor half yet. Park a continuation and give the event loop back: no
						// reply is sent, so the guest keeps waiting for the real answer and no success is
						// faked. The continuation re-enters this handler with the descriptor in place --
						// safe because nothing has been done yet on this call.
						thread->suspend([this, fdBox]() {
							if (*fdBox >= 0) {
								_body.exec_listener_pipe = *fdBox;
							}
							processCall();
						});
						// Reaching here means suspend() consumed a resume that arrived before it could
						// install the continuation, so the completion belongs to this frame.
						if (*fdBox >= 0) {
							_body.exec_listener_pipe = *fdBox;
							processCall();
						} else {
							callLog.error() << "checkout: parked without a descriptor and without a resume "
								<< "(token " << _body.fd_token << ")" << callLog.endLog;
							code = -EBADF;
						}
						return;
					default:
						callLog.error() << "checkout: courier descriptor rejected (result "
							<< static_cast<int>(result) << ", token " << _body.fd_token
							<< ") -- committed failure, never a UDS retry" << callLog.endLog;
						code = -EBADF;
						break;
				}
			}

			if (_body.exec_listener_pipe >= 0) {
				// this is actually an execve;
				// let's monitor the FD we got

				// make it non-blocking
				int flags = fcntl(_body.exec_listener_pipe, F_GETFL);
				if (flags < 0) {
					code = -errno;
				} else {
					flags |= O_NONBLOCK;
					if (fcntl(_body.exec_listener_pipe, F_SETFL, flags) < 0) {
						code = -errno;
					} else {
						// now monitor it
						auto fd = std::make_shared<FD>(_body.exec_listener_pipe);
						_body.exec_listener_pipe = -1; // the FD instance now owns the descriptor

						process->beginExec(fd, _body.executing_macho);

						std::weak_ptr<Process> weakProcess = process;
						Server::sharedInstance().addMonitor(std::make_shared<Monitor>(fd, Monitor::Event::HangUp, false, true, [fd, weakProcess](std::shared_ptr<Monitor> monitor, Monitor::Event events) {
							Server::sharedInstance().removeMonitor(monitor);

							auto process = weakProcess.lock();

							if (!process) {
								// the process died...
								return;
							}

							process->notifyExecCompletion(fd);
						}));
					}
				}
			} else {
				thread->notifyDead();

				// if this was the last thread in the process, it'll be automatically unregistered
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	// clear the thread pointer so that the reply will be sent directly through the server
	// (otherwise, we would attempt to send it through the thread, which is now dead)
	_thread.reset();

	_sendReply(code);
};

void DarlingServer::Call::VchrootPath::processCall() {
	int code = 0;
	size_t fullLength = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			if (_body.buffer_size > 0) {
				auto tmpstr = process->vchrootPath().substr(0, _body.buffer_size - 1);
				auto len = std::min(tmpstr.length() + 1, _body.buffer_size);

				fullLength = process->vchrootPath().length();

				if (!process->writeMemory(_body.buffer, tmpstr.c_str(), len, &code)) {
					// writeMemory returns a positive error code, but we want a negative one
					code = -code;
				}
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, fullLength);
};

void DarlingServer::Call::TaskSelfTrap::processCall() {
	const auto taskSelfPort = dtape_task_self_trap();
	_sendReply(0, taskSelfPort);
};

void DarlingServer::Call::HostSelfTrap::processCall() {
	const auto hostSelfPort = dtape_host_self_trap();
	_sendReply(0, hostSelfPort);
};

void DarlingServer::Call::ThreadSelfTrap::processCall() {
	const auto threadSelfPort = dtape_thread_self_trap();
	_sendReply(0, threadSelfPort);
};

void DarlingServer::Call::MachReplyPort::processCall() {
	const auto machReplyPort = dtape_mach_reply_port();
	_sendReply(0, machReplyPort);
};

void DarlingServer::Call::Kprintf::processCall() {
	static auto kprintfLog = Log("kprintf");
	int code = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			char* tmp = (char*)malloc(_body.string_length + 1);

			if (tmp) {
				if (process->readMemory(_body.string, tmp, _body.string_length, &code)) {
					size_t len = _body.string_length;

					// strip trailing whitespace
					while (len > 0 && isspace(tmp[len - 1])) {
						--len;
					}
					tmp[len] = '\0';

					kprintfLog.info() << tmp << kprintfLog.endLog;
				} else {
					// readMemory returns a positive error code, but we want a negative one
					code = -code;
				}

				free(tmp);
			} else {
				code = -ENOMEM;
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::StartedSuspended::processCall() {
	int code = 0;
	bool suspended = false;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			suspended = process->startSuspended();
			process->setStartSuspended(false);
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, suspended);
};

void DarlingServer::Call::GetTracer::processCall() {
	int code = 0;
	int32_t tracer = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			if (auto tracerProcess = process->tracerProcess()) {
				tracer = tracerProcess->nsid();
			} else {
				// leave `tracer` as 0
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, tracer);
};

void DarlingServer::Call::Uidgid::processCall() {
	int code = 0;
	int uid = -1;
	int gid = -1;

	if (TestDiagnostics::consumeFault("processcall.uidgid_throw")) {
		TestDiagnostics::traceLine("call.processcall_throw name=uidgid");
		throw std::runtime_error("injected uidgid processCall failure");
	}

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			// HACK
			// we shouldn't need to access _dtapeTask; Process should provide a method for this (but it doesn't yet because i'm not sure how to make that API feel at-home in C++)
			dtape_task_uidgid(process->_dtapeTask, _body.new_uid, _body.new_gid, &uid, &gid);
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, uid, gid);
};

void DarlingServer::Call::SetThreadHandles::processCall() {
	int code = 0;

	if (auto thread = _thread.lock()) {
		thread->setThreadHandles(_body.pthread_handle, _body.dispatch_qaddr);
	} else {
		code = -ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::Vchroot::processCall() {
	int code = 0;
	static constexpr uint64_t kMaxVchrootPath = 4096;

	// TODO: wrap all `processCall` calls in try-catch like this
	try {
		if (auto thread = _thread.lock()) {
			if (auto process = thread->process()) {
				// `path` is a guest pointer. Copy the complete pathname snapshot once,
				// bounded by the ABI limit, and reject short/faulting reads. Do not parse
				// any bytes directly from guest memory after this copy.
				if (_body.path_size == 0 || _body.path_size > kMaxVchrootPath) {
					code = -EINVAL;
				} else {
					char path[kMaxVchrootPath];

					if (process->readMemory(static_cast<uintptr_t>(_body.path), path, static_cast<size_t>(_body.path_size), &code)) {
						if (path[_body.path_size - 1] != '\0') {
							code = -EINVAL;
						} else if (std::memchr(path, '\0', static_cast<size_t>(_body.path_size - 1)) != nullptr) {
							// Embedded NUL would make the server and guest interpret different
							// pathname byte strings.
							code = -EINVAL;
						} else {
							process->setVchrootPath(std::string(path, static_cast<size_t>(_body.path_size - 1)));
						}
					} else {
						// readMemory returns a positive error code, but we want a negative one
						code = -code;
					}
				}
			} else {
				code = -ESRCH;
			}
		} else {
			code = -ESRCH;
		}
	} catch (std::system_error err) {
		code = -err.code().value();
	} catch (...) {
		code = std::numeric_limits<int>::min();
	}

	_sendReply(code);
};

void DarlingServer::Call::MldrPath::processCall() {
	int code = 0;
	uint64_t fullLength = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			auto tmpstr = std::string(Config::defaultMldrPath).substr(0, _body.buffer_size - 1);
			auto len = std::min(tmpstr.length() + 1, _body.buffer_size);

			fullLength = process->vchrootPath().length();

			if (!process->writeMemory(_body.buffer, tmpstr.c_str(), len, &code)) {
				// writeMemory returns a positive error code, but we want a negative one
				code = -code;
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, fullLength);
};

void DarlingServer::Call::ThreadGetSpecialReplyPort::processCall() {
	_sendReply(0, dtape_thread_get_special_reply_port());
};

void DarlingServer::Call::MkTimerCreate::processCall() {
	_sendReply(0, dtape_mk_timer_create());
};

void DarlingServer::Call::PthreadKill::processCall() {
	int code = 0;

	if (auto targetThread = Thread::threadForPort(_body.thread_port)) {
		try {
			targetThread->sendSignal(_body.signal);
		} catch (std::system_error e) {
			code = -e.code().value();
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::PthreadCanceled::processCall() {
	// Implements XNU __pthread_canceled(action) on the calling thread's
	// duct-tape cancellation bits (dar-gwn.6.3). dtape_thread_canceled returns
	// the XNU-style code (0 / EINVAL); we negate to the guest's BSD-errno
	// convention. The old TODO stub replied -ENOSYS for every action, which
	// broke libpthread's cancellation handshake and made cancelable syscalls
	// (brew's portable-ruby) livelock re-issuing this call ~670x/sec.
	int code = -ESRCH;
	dtape_thread_cancel_state_snapshot_t before = {};
	dtape_thread_cancel_state_snapshot_t after = {};
	bool stateKnown = false;
	const bool trace = TestDiagnostics::enabled();

	if (auto thread = _thread.lock()) {
		stateKnown = true;
		// perf#30 ATTRIBUTION (doc section 64): this callnum is 87% on the lane and 13% on the datagram, and
		// the guest's reason histogram is silent for it. The server can answer the question directly: did the
		// thread HAVE a lane when this datagram arrived? If yes, the guest chose the datagram despite having
		// one; if no, the datagram is the only transport that existed for that thread.
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log pcLog("pthread-canceled-attrib");
			// The transport, not just the lane's existence: this line logged EVERY invocation when it was
			// written (193 lines for a run whose heatmap shows far fewer datagrams), which is the
			// "instrument that cannot answer" class again. A call taken off the lane carries a ring context,
			// so its presence separates the two transports in the same line.
			pcLog.error() << "pthread_canceled pid=" << _header.pid << " tid=" << _header.tid
				<< " has_lane=" << (thread->ring() ? 1 : 0)
				<< " lane=" << (ringContext() != nullptr ? 1 : 0)
				<< " action=" << _body.action << pcLog.endLog;
		}
		if (trace) {
			thread->cancelStateSnapshot(&before.disabled, &before.pending, &before.canceled);
		}
		// The one semantic core, also reached by the management plane (direct servicing):
		// Thread::pthreadCanceled owns the state transitions and their synchronization.
		code = thread->pthreadCanceled(_body.action);
		if (trace) {
			thread->cancelStateSnapshot(&after.disabled, &after.pending, &after.canceled);
		}
	}

	if (trace) {
		TestDiagnostics::tracePthreadCanceled(
			_header.pid,
			_header.tid,
			_body.action,
			stateKnown,
			before.disabled,
			before.pending,
			before.canceled,
			after.disabled,
			after.pending,
			after.canceled,
			code
		);
	}
	_sendReply(code);
};

void DarlingServer::Call::PthreadMarkcancel::processCall() {
	// Implements XNU __pthread_markcancel(thread_port): arm the cancel-pending
	// bit on the target thread (the kernel side of pthread_cancel). dar-gwn.6.3.
	int code = 0;
	dtape_thread_cancel_state_snapshot_t before = {};
	dtape_thread_cancel_state_snapshot_t after = {};
	bool targetKnown = false;
	const bool trace = TestDiagnostics::enabled();

	if (auto targetThread = Thread::threadForPort(_body.thread_port)) {
		targetKnown = true;
		if (trace) {
			targetThread->cancelStateSnapshot(&before.disabled, &before.pending, &before.canceled);
		}
		code = targetThread->pthreadMarkcancel();
		if (trace) {
			targetThread->cancelStateSnapshot(&after.disabled, &after.pending, &after.canceled);
		}
	} else {
		code = -ESRCH;
	}

	if (trace) {
		TestDiagnostics::tracePthreadMarkcancel(
			_header.pid,
			_header.tid,
			_body.thread_port,
			targetKnown,
			before.disabled,
			before.pending,
			before.canceled,
			after.disabled,
			after.pending,
			after.canceled,
			code
		);
	}
	_sendReply(code);
};

void DarlingServer::Call::KqchanMachPortOpen::processCall() {
	int code = 0;
	int socket = -1;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			auto kqchan = std::make_shared<Kqchan::MachPort>(process, _body.port_name, _body.receive_buffer, _body.receive_buffer_size, _body.saved_filter_flags);

			try {
				socket = kqchan->setup();
			} catch (std::system_error e) {
				code = -e.code().value();
			} catch (...) {
				// just report that we couldn't find the port
				code = -ESRCH;
			}

			process->registerKqchan(kqchan);
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, socket);
};

void DarlingServer::Call::KqchanProcOpen::processCall() {
	int code = 0;
	int socket = -1;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			auto kqchan = std::make_shared<Kqchan::Process>(process, _body.pid, _body.flags);

			try {
				socket = kqchan->setup();
			} catch (std::system_error e) {
				code = -e.code().value();
			} catch (...) {
				// just report that we couldn't find the process
				code = -ESRCH;
			}

			process->registerKqchan(kqchan);
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, socket);
};

void DarlingServer::Call::ForkWaitForChild::processCall() {
	int code = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			if (!process->waitForChildAfterFork()) {
				code = -ETIMEDOUT;
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::Sigprocess::processCall() {
	int code = 0;
	int newBSDSignal = 0;

	if (auto thread = _thread.lock()) {
		try {
			thread->processSignal(_body.bsd_signal_number, _body.linux_signal_number, _body.code, _body.signal_address, _body.thread_state, _body.float_state);
			newBSDSignal = thread->pendingSignal();
		} catch (std::system_error e) {
			code = -e.code().value();
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, newBSDSignal);
};

void DarlingServer::Call::TaskIs64Bit::processCall() {
	int code = 0;
	bool is64Bit = false;

	if (auto maybeTargetProcess = processRegistry().lookupEntryByNSID(_body.id)) {
		auto targetProcess = *maybeTargetProcess;
		is64Bit = targetProcess->is64Bit();
	} else {
		code = -ESRCH;
	}

	_sendReply(code, is64Bit);
};

void DarlingServer::Call::InterruptEnter::processCall() {
	Thread::_handleInterruptEnterForCurrentThread();

	_sendReply(0);
};

void DarlingServer::Call::InterruptExit::processCall() {
	auto thread = _thread.lock();

	dtape_thread_sigexc_exit(thread->_dtapeThread);

	_sendReply(0);

	{
		std::unique_lock lock(thread->_rwlock);

		auto tmp = std::move(thread->_interrupts.top());

		thread->_interrupts.pop();

		if (tmp.savedReply) {
			callLog.debug() << *thread << ": Going to send saved reply" << callLog.endLog;
			Server::sharedInstance().sendMessage(std::move(*tmp.savedReply));
			tmp.savedReply = std::nullopt;
			// A0 RPC TRACE: the STASH-SAVED[interrupt] reply is now flushed at interrupt_exit. Its absence
			// for a tid that logged STASH-SAVED is the stall signature (interrupted reply never re-sent).
			DarlingServer::__rpctrace("FLUSH-SAVED htid=%d nstid=%lld", thread->id(), (long long)thread->nsid());
		}
	}
};

void DarlingServer::Call::ConsoleOpen::processCall() {
	static Log consoleLog("console");

	int code = 0;
	int sockets[2] = { -1, -1 };

	// we don't really need bidirectional communication, so a pipe would suffice,
	// except that when you set O_NONBLOCK on one side of a pipe, it is set for both.

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
		int err = errno;
		callLog.warning() << __PRETTY_FUNCTION__ << ": socketpair failed with " << err << callLog.endLog;

		// just report EMFILE for the peer
		code = EMFILE;
	} else {
		// make our side non-blocking
		int flags = fcntl(sockets[0], F_GETFL);
		if (flags < 0) {
			code = -errno;
		} else {
			flags |= O_NONBLOCK;
			if (fcntl(sockets[0], F_SETFL, flags) < 0) {
				code = -errno;
			} else {
				// now monitor it
				auto fd = std::make_shared<FD>(sockets[0]);
				std::weak_ptr<Process> weakProcess;

				if (auto thread = _thread.lock()) {
					if (auto process = thread->process()) {
						weakProcess = process;
					}
				}

				Server::sharedInstance().addMonitor(std::make_shared<Monitor>(fd, Monitor::Event::Readable | Monitor::Event::HangUp, false, false, [fd, weakProcess](std::shared_ptr<Monitor> monitor, Monitor::Event events) {
					auto proc = weakProcess.lock();

					if (!proc || static_cast<uint64_t>(events & Monitor::Event::HangUp) != 0) {
						Server::sharedInstance().removeMonitor(monitor);
						return;
					}

					if (static_cast<uint64_t>(events & Monitor::Event::Readable) != 0) {
						std::stringstream data;
						while (true) {
							char buf[128];
							auto count = read(fd->fd(), buf, sizeof(buf) - 1);
							if (count <= 0) {
								break;
							}
							buf[count] = '\0';
							data << buf;
						}
						consoleLog.info() << *proc << ": " << data.rdbuf();
					}
				}));
			}
		}
	}

	if (code != 0) {
		if (sockets[0] >= 0) {
			close(sockets[0]);
			sockets[0] = -1;
		}
		if (sockets[1] >= 0) {
			close(sockets[1]);
			sockets[1] = -1;
		}
	}
	_sendReply(code, sockets[1]);
};

void DarlingServer::Call::SetDyldInfo::processCall() {
	int code = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			dtape_task_set_dyld_info(process->_dtapeTask, _body.address, _body.length);
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::StopAfterExec::processCall() {
	int code = 0;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			process->setStartSuspended(true);
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::SetTracer::processCall() {
	int code = 0;
	std::shared_ptr<Process> targetProcess = nullptr;
	std::shared_ptr<Process> tracerProcess = nullptr;

	if (_body.target == 0) {
		if (auto thread = _thread.lock()) {
			targetProcess = thread->process();
		}
	} else {
		if (auto maybeTargetProcess = processRegistry().lookupEntryByNSID(_body.target)) {
			targetProcess = *maybeTargetProcess;
		}
	}

	if (targetProcess) {
		if (_body.tracer == 0) {
			// leave tracer process as nullptr
		} else {
			if (auto maybeTracerProcess = processRegistry().lookupEntryByNSID(_body.tracer)) {
				tracerProcess = *maybeTracerProcess;
			} else {
				// intentionally not negated because this is not an internal error;
				// this is a perfectly valid case
				code = ESRCH;
			}
		}

		if (code == 0) {
			if (!targetProcess->setTracerProcess(tracerProcess)) {
				// again, not negated because this isn't an internal error;
				// simply indicates there was already a tracer set for the target
				code = EPERM;
			}
		}
	} else {
		// ditto from before
		code = ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::TidForThread::processCall() {
	int code = 0;
	int32_t tid = 0;

	if (auto thread = Thread::threadForPort(_body.thread)) {
		tid = thread->nsid();
	} else {
		// might be user error (e.g. invalid port number or dead thread), so don't negate it
		code = ESRCH;
	}

	_sendReply(code, tid);
};

void DarlingServer::Call::PtraceSigexc::processCall() {
	int code = 0;

	if (auto maybeProcess = processRegistry().lookupEntryByNSID(_body.target)) {
		auto process = *maybeProcess;

		dtape_task_set_sigexc_enabled(process->_dtapeTask, _body.enabled);
		dtape_task_try_resume(process->_dtapeTask);
	} else {
		// not negated because this isn't an internal error
		code = ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::PtraceThupdate::processCall() {
	int code = 0;

	if (auto maybeThread = threadRegistry().lookupEntryByNSID(_body.target)) {
		auto thread = *maybeThread;

		thread->setPendingSignal(_body.signum);
	} else {
		// not negated because this isn't an internal error
		code = ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::ThreadSuspended::processCall() {
	int code = 0;

	if (auto thread = _thread.lock()) {
		thread->waitWhileUserSuspended(_body.thread_state, _body.float_state);
	} else {
		code = -ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::S2CPerform::processCall() {
	int code = 0;

	if (auto thread = _thread.lock()) {
		dtape_semaphore_up(thread->_s2cInterruptEnterSemaphore);
		dtape_semaphore_down_simple(thread->_s2cInterruptExitSemaphore);
	} else {
		code = -ESRCH;
	}

	_sendReply(code);
};

void DarlingServer::Call::SetExecutablePath::processCall() {
	int code = 0;
	std::string path;
	bool pathSet = false;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			std::string tmpstr;
			tmpstr.resize(_body.buffer_size);
			if (!process->readMemory((uintptr_t)_body.buffer, tmpstr.data(), _body.buffer_size, &code)) {
				code = -code;
			} else {
				path = tmpstr.c_str();
				process->setExecutablePath(path);
				pathSet = true;
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	if (TestDiagnostics::enabled()) {
		TestDiagnostics::traceExecutablePath(_header.pid, _header.tid, pathSet ? path : std::string(), code);
	}

	_sendReply(code);
}

void DarlingServer::Call::GetExecutablePath::processCall() {
	int code = 0;
	uint64_t fullLength;

	if (auto callingThread = _thread.lock()) {
		if (auto callingProcess = callingThread->process()) {
			if (auto maybeTargetProcess = processRegistry().lookupEntryByNSID(_body.pid)) {
				auto targetProcess = *maybeTargetProcess;
				auto path = targetProcess->executablePath();
				auto len = std::min(path.length() + 1, _body.buffer_size);
				if (!callingProcess->writeMemory((uintptr_t)_body.buffer, path.c_str(), len, &code)) {
					code = -code;
				}
				fullLength = path.length();
			} else {
				// not negated because this is an acceptable case.
				// e.g. the target process may have died before the call was processed.
				code = ESRCH;
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, fullLength);
}

void DarlingServer::Call::Groups::processCall() {
	int code = 0;
	std::vector<uint32_t> oldGroups;

	if (auto thread = _thread.lock()) {
		if (auto process = thread->process()) {
			oldGroups = process->groups();

			if (_body.new_groups != 0 && _body.new_group_count > 0) {
				std::vector<uint32_t> newGroups;
				newGroups.resize(_body.new_group_count);

				if (!process->readMemory((uintptr_t)_body.new_groups, newGroups.data(), newGroups.size() * sizeof(uint32_t), &code)) {
					code = -code;
				} else {
					process->setGroups(newGroups);
				}
			}

			if (code == 0 && _body.old_groups != 0 && _body.old_group_space > 0) {
				auto len = std::min(oldGroups.size(), _body.old_group_space) * sizeof(uint32_t);
				if (!process->writeMemory((uintptr_t)_body.old_groups, oldGroups.data(), len, &code)) {
					code = -code;
				}
			}
		} else {
			code = -ESRCH;
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, oldGroups.size());
};

void DarlingServer::Call::DebugListProcesses::processCall() {
	int code = 0;
	auto processes = processRegistry().copyEntries();
	int pipes[2] = {-1, -1};

	code = pipe(pipes);
	if (code == 0) {
		for (const auto& process: processes) {
			dserver_debug_process_t debugProcess;
			debugProcess.pid = process->nsid();
			debugProcess.port_count = dtape_debug_task_port_count(process->_dtapeTask);
			write(pipes[1], &debugProcess, sizeof(debugProcess));
		}

		close(pipes[1]);
	}

	_sendReply(code, processes.size(), pipes[0]);
};

void DarlingServer::Call::DebugListPorts::processCall() {
	int code = 0;
	uint64_t portCount = 0;
	int pipes[2] = {-1, -1};

	if (auto maybeProcess = processRegistry().lookupEntryByNSID(_body.process)) {
		auto process = *maybeProcess;

		code = pipe(pipes);
		if (code == 0) {
			portCount = dtape_debug_task_list_ports(process->_dtapeTask, [](void* context, const dtape_debug_port_t* port) {
				int& writeFD = *(int*)context;
				dserver_debug_port_t debugPort;

				debugPort.port_name = port->name;
				debugPort.rights = port->rights;
				debugPort.refs = port->refs;
				debugPort.messages = port->messages;

				write(writeFD, &debugPort, sizeof(debugPort));

				return true;
			}, &pipes[1]);
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, portCount, pipes[0]);
};

void DarlingServer::Call::DebugListMembers::processCall() {
	int code = 0;
	uint64_t portCount = 0;
	int pipes[2] = {-1, -1};

	if (auto maybeProcess = processRegistry().lookupEntryByNSID(_body.process)) {
		auto process = *maybeProcess;

		code = pipe(pipes);
		if (code == 0) {
			portCount = dtape_debug_portset_list_members(process->_dtapeTask, _body.portset, [](void* context, const dtape_debug_port_t* port) {
				int& writeFD = *(int*)context;
				dserver_debug_port_t debugPort;

				debugPort.port_name = port->name;
				debugPort.rights = port->rights;
				debugPort.refs = port->refs;
				debugPort.messages = port->messages;

				write(writeFD, &debugPort, sizeof(debugPort));

				return true;
			}, &pipes[1]);
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, portCount, pipes[0]);
};

void DarlingServer::Call::DebugListMessages::processCall() {
	int code = 0;
	uint64_t portCount = 0;
	int pipes[2] = {-1, -1};

	if (auto maybeProcess = processRegistry().lookupEntryByNSID(_body.process)) {
		auto process = *maybeProcess;

		code = pipe(pipes);
		if (code == 0) {
			portCount = dtape_debug_port_list_messages(process->_dtapeTask, _body.port, [](void* context, const dtape_debug_message_t* port) {
				int& writeFD = *(int*)context;
				dserver_debug_message_t debugMessage;

				debugMessage.sender = port->sender;
				debugMessage.size = port->size;

				write(writeFD, &debugMessage, sizeof(debugMessage));

				return true;
			}, &pipes[1]);
		}
	} else {
		code = -ESRCH;
	}

	_sendReply(code, portCount, pipes[0]);
};

// perf #18 (dar-dar6x4-perf-5dq.30): shared-memory ring transport negotiation handler.
//
// The guest sends a memfd (via @fd, dup'd into _body.ring_fd) holding a dserver_ring_shm
// control block + rings + arena, plus the size it claims the mapping is. We treat all of it
// as untrusted: dserver_ring_attach_check() fstats the fd for its REAL size, maps it
// read-only, copies the control block out, and runs the pure validator -- dereferencing no
// guest pointer and trusting no guest-supplied length. reject_reason is 0 (dserver_ring_ok)
// on accept or a dserver_ring_reject_t code; on any reject (or with the feature compiled
// off) the guest stays on UDS forever, no error -- the ring is a fast path, never the only
// path. On accept we build a RingBuffer (maps RW + creates the wake eventfd), register that
// eventfd as a Readable Monitor on the epoll loop, and hand both to the Thread (it releases
// them on death). The Monitor callback currently just drains the wake eventfd -- no call is
// migrated onto the ring yet (that's P3), so there is nothing to dispatch; this proves the
// attach/teardown lifecycle end-to-end.
#ifdef DSERVER_RING_TRANSPORT
// perf #18 P3: service all C2S requests published on a thread's ring. Runs in the ring's
// Monitor callback on the MAIN event loop (the wake eventfd fired). For each request slot we
// rebuild the exact UDS-format request bytes the guest would have sent over the socket
// ({callhdr, body}), construct the SAME Call object via callFromMessage(), arm the thread's
// one-shot ring-reply sink (beginRingReply), and run it INLINE via doWork() -- identical to
// the perf#2b main-loop fast path (server.cpp). The reply is redirected onto the s2c ring by
// Thread::pushCallReply(). Reusing the whole Call path is deliberate: every exec-path fix
// (dar-l8k UAF, dar-6x4 rwlock-across-suspend, the dar-gwn reply-on-drop guards) applies
// unchanged -- only the transport in and the reply sink out differ.
//
// SECURITY: the slot's callnum/length are attacker-controlled. dserver_ring_consumer_begin()
// already refuses a corrupt c2s tail; here we additionally (a) bound the inline body to the
// slot, (b) only accept a small allowlist of ring-eligible call numbers (P3 = task_self_trap),
// dropping anything else so the guest UDS-falls-back, and (c) never deref a guest pointer.
// perf #18 P8 D3 (dar-1il.3.1.1): the DUPLEX SELFTEST hatch. Default OFF (must be set to "1"). The
// sentinel parent op (DSERVER_RING_DUPLEX_SELFTEST_CALLNUM) is recognized in the ring service loop ONLY
// when this is on, so the duplex lane never activates on any real op or on a normal boot -- it is the
// outer kill-switch on top of the per-thread conjunction guard. (Membership-wise the sentinel callnum
// is OUTSIDE the RPC range + not in DSERVER_RING_C2S_OPCODES, so with the hatch off it falls through to
// the eligible check, is not allowlisted, and is dropped exactly like any unknown callnum.)
static bool ringDuplexSelftestEnabled() {
	static const bool v = []() {
		const char* e = getenv("DARLING_SERVER_DUPLEX_SELFTEST");
		return e && e[0] == '1' && e[1] == '\0';
	}();
	return v;
}

// perf #18 P8 D5 (dar-1il.3.2.2): the BOOT-SCOPED launchd vm_deallocate-via-duplex proof harness (option 1,
// gist 3e928115). The caller-S2C munmap deadlock the duplex lane exists to cure is reachable in practice
// ONLY in launchd/early-init (guest pid 1) on a normal boot; a warm leaf command never drives it. So the
// ONLY way to get a LIVE caller-S2C cure proof is to let launchd route a vm_deallocate over the duplex lane
// AT BOOT -- but globally enabling that (or an inherited env hatch) is the forbidden init-wedge hazard. This
// harness threads that needle: it is a ONE-SHOT, BUDGET-LIMITED, AUTO-DISARMING proof, armed ONLY by an env
// var ON THE SERVER PROCESS ITSELF (DARLING_SERVER_D5_VMDEALLOC_PROOF=<budget>, set when LAUNCHING the
// server -- never a guest daemon's inherited env, so shellspawn/leaf processes are untouched). The routing
// CONJUNCTION (below, in ringServiceThread) additionally requires guest pid==1 + the vm_deallocate callnum +
// the duplex caps + a clean mailbox, and AUTO-DISARMS (budget->0) after the first successful caller-S2C, so
// at most <budget> launchd vm_deallocates ever ride the lane and every park is bounded fail-closed.
//
// proofBudget(): the remaining number of launchd vm_deallocates allowed onto the duplex lane. Initialized
// once from the env (0 = disarmed/off, the default). decremented to 0 on the first proven caller-S2C.
static std::atomic<int>& d5VmDeallocProofBudget() {
	static std::atomic<int> budget{[]() {
		const char* e = getenv("DARLING_SERVER_D5_VMDEALLOC_PROOF");
		if (!e) return 0;
		int v = atoi(e);
		if (v < 0) v = 0;
		if (v > 3) v = 3; // gist: ideally 1-3 events; hard cap the blast radius.
		return v;
	}()};
	return budget;
}
static bool d5VmDeallocProofArmed() {
	return d5VmDeallocProofBudget().load(std::memory_order_relaxed) > 0;
}

void DarlingServer::Call::attachRingContext(std::shared_ptr<class RingBuffer> ring, uint32_t seq, uint32_t callnum, bool duplexCapable) {
	_ringContext = RingCallContext{ std::move(ring), seq, callnum, duplexCapable };
};

uint32_t DarlingServer::ringServiceThread(const std::shared_ptr<DarlingServer::Thread>& thread) {
	using namespace DarlingServer;
	uint32_t serviced = 0;
	auto ring = thread->ring();
	if (!ring) {
		return 0;
	}
	const auto& cb = ring->controlBlock();
	dserver_ring_t* c2s = ring->c2sRing();
	uint32_t slotSize = cb.slot_size;
	uint32_t slotCount = cb.slot_count;
	uint32_t inlineCap = slotSize - static_cast<uint32_t>(sizeof(dserver_ring_slot_t));

	auto process = thread->process();
	if (!process) {
		return 0;
	}

	// bounded drain: never loop more than slotCount times even if a buggy/hostile peer keeps
	// the tail ahead (consumer_advance bounds us anyway, but be explicit).
	for (uint32_t guard = 0; guard < slotCount; ++guard) {
		dserver_ring_slot_t* req = dserver_ring_consumer_begin(c2s, slotSize, slotCount);
		if (!req) {
			break; // empty or corrupt tail
		}
		// perf#18 D9: this call was taken off a RING LANE. Tag it here, in the generic service path, so the
		// heatmap reports ring-served calls as Ring for EVERY eligible op -- not only for the duplex-reply
		// step that used to be the sole place the tag was set.
		thread->noteServicedFromRing();
#ifdef DSERVER_RING_PHASE_PROF
		uint64_t _phaseT0 = Metrics::rdtscCycles(); // perf#18 P6: drain phase start
#endif

		// Copy the transport header out before trusting it (guest can mutate concurrently).
		uint32_t callnum = req->callnum;
		uint32_t reqlen = req->length;
		uint32_t seq = req->seq;
		uint32_t arenalen = req->arena_len;

		// perf #18 P8 D3 (dar-1il.3.1.1): the synthetic DUPLEX SELFTEST parent op. Recognized ONLY
		// behind the env hatch + ONLY for the out-of-RPC-range sentinel callnum, so no real op and no
		// normal boot ever reaches it. Shape: an empty-or-one-uint32 body carrying the echo arg. We free
		// the slot, then kick off ONE guarded duplex S2C upcall (publish + return -- it does NOT park;
		// the main-loop drain completes the parent by publishing its final reply when the correlated
		// reply arrives). If the conjunction guard declines (no v4 ring / no cap / busy / stale mailbox),
		// publish the parent's FAILURE reply immediately so the guest selftest never wedges.
		if (ringDuplexSelftestEnabled() && callnum == DSERVER_RING_DUPLEX_SELFTEST_CALLNUM) {
			uint32_t arg = 0;
			if (reqlen >= sizeof(uint32_t) && reqlen <= inlineCap) {
				memcpy(&arg, reinterpret_cast<const char*>(req) + sizeof(dserver_ring_slot_t), sizeof(arg));
			}
			dserver_ring_consumer_advance(c2s); // free the slot before kicking off the upcall
			if (!thread->duplexSelftestUpcall(arg, seq)) {
				// guard declined -> the duplex path was NOT taken; fail the parent reply now (code != 0,
				// no fabricated success). The guest selftest sees the error code + UDS-equivalent miss.
				thread->ring()->publishReply(seq, DSERVER_RING_DUPLEX_SELFTEST_CALLNUM, -1, nullptr, 0);
				thread->ring()->wakeGuest();
			}
			++serviced;
			continue;
		}

		// perf #18 P8 D4 (dar-1il.3.2.1): mach_port_deallocate as a DUPLEX PARENT. deallocate is NOT on
		// the simple ring (it is destroy-capable / caller-S2C); a duplex-deallocate-capable guest routes
		// it here. We run it on the GENERIC fiber path (exactly like a simple-ring body op) but with
		// _ringDuplexParentActive set, so its munmap S2C (if any) rides the duplex mailbox instead of the
		// UDS S2C that a ring-parked caller can't service. The ROUTING decline (the pre-mutation safety
		// boundary) is HERE: if the caller did not advertise DUPLEX_CAP_DEALLOCATE we must NOT dispatch
		// the op (we can't safely service its possible S2C) -- publish a DECLINE reply so the guest
		// UDS-falls-back, BEFORE any mutation (no double-effect). The decline is decidable purely from the
		// negotiated cap, before the op runs.
		if (callnum == (uint32_t)dserver_callnum_mach_port_deallocate) {
			if (!thread->duplexDeallocateCapable()) {
				// caller is not duplex-deallocate-capable: decline pre-dispatch -> guest UDS-falls-back.
				dserver_ring_consumer_advance(c2s);
				thread->ring()->publishReply(seq, callnum, DSERVER_RING_DUPLEX_DECLINE, nullptr, 0);
				thread->ring()->wakeGuest();
				Metrics::shared().ringDuplexDecline.fetch_add(1, std::memory_order_relaxed);
				++serviced;
				continue;
			}
			if (reqlen != sizeof(dserver_call_mach_port_deallocate_t)) {
				// unexpected shape -> decline pre-dispatch (no mutation), guest UDS-falls-back.
				// (dserver_call_mach_port_deallocate_t is the BODY only: {uint32 target; uint32 name} = 8B.)
				dserver_ring_consumer_advance(c2s);
				thread->ring()->publishReply(seq, callnum, DSERVER_RING_DUPLEX_DECLINE, nullptr, 0);
				thread->ring()->wakeGuest();
				Metrics::shared().ringDuplexDecline.fetch_add(1, std::memory_order_relaxed);
				++serviced;
				continue;
			}
			// the op is being dispatched onto the duplex lane (proof it RODE the lane, S2C or not).
			Metrics::shared().ringDuplexParent.fetch_add(1, std::memory_order_relaxed);
			// rebuild {callhdr, body} exactly as the simple-ring generic path, then dispatch on the fiber
			// with the duplex-parent flag set so _s2cPerform routes the munmap S2C through the mailbox.
			size_t totalSize = sizeof(dserver_rpc_callhdr_t) + reqlen;
			Message reqMsg(totalSize, 0);
			reqMsg.data().resize(totalSize);
			auto* hdr = reinterpret_cast<dserver_rpc_callhdr_t*>(reqMsg.data().data());
			hdr->number = static_cast<dserver_callnum_t>(callnum);
			hdr->pid = process->nsid();
			hdr->tid = thread->nsid();
			hdr->architecture = static_cast<dserver_rpc_architecture_t>(process->architecture());
			memcpy(reqMsg.data().data() + sizeof(dserver_rpc_callhdr_t),
			       reinterpret_cast<const char*>(req) + sizeof(dserver_ring_slot_t), reqlen);
			reqMsg.setAddress(thread->address());
			reqMsg.setPID(process->id());
			dserver_ring_consumer_advance(c2s); // free the slot before running the op
			try {
				auto call = Call::callFromMessage(std::move(reqMsg));
				if (call) {
					// duplex parent (mach_port_deallocate): this op may raise a caller-S2C, and this call's own lane
					// mailbox is the only way a ring-parked caller can service it. The context lives on the
					// Call, so it survives the dispatch and any suspend/resume -- see RingCallContext.
					call->attachRingContext(thread->ring(), seq, callnum, true);
					call->thread()->doWork();
					++serviced;
				}
			} catch (const std::exception& ex) {
				callLog.error() << "ring duplex deallocate dispatch threw: " << ex.what() << callLog.endLog;
			}
			continue;
		}

		// perf #18 P8 D5 (dar-1il.3.2.2): mach_vm_deallocate as a DUPLEX PARENT. Unlike D4's
		// mach_port_deallocate (whose munmap S2C is unreachable in Darling because make_memory_entry is a
		// stub), vm_deallocate DOES drive a real caller munmap S2C (vm_map_remove -> dtape_hook_task_free_
		// pages -> _munmap -> _s2cPerform) -- it is the op that genuinely exercises (and proves) the duplex
		// lane's caller-S2C cure. Same machinery as D4: run on the GENERIC fiber path with
		// _ringDuplexParentActive set so the munmap S2C rides the duplex mailbox instead of the UDS S2C a
		// ring-parked caller can't service. The PRE-MUTATION routing decline is HERE, keyed on the SPECIFIC
		// VM_DEALLOCATE cap (a D4-only-capable caller must NOT have a vm_deallocate routed onto the lane).
		if (callnum == (uint32_t)dserver_callnum_mach_vm_deallocate) {
			// perf #18 P8 D6 (caller-S2C sideband) PROOF CONJUNCTION (gist 3e928115 answer #2). Route a
			// vm_deallocate onto the duplex lane ONLY if EVERY condition holds; else DECLINE pre-dispatch (no
			// mutation -> guest UDS-falls-back, the old behavior). The SYNTHETIC WARM real-munmap proof: a
			// test guest allocates a real page locally then sends vm_deallocate of it with
			// target==mach_task_self() OVER the duplex ring (bypassing the trap's local-munmap gate). The
			// server _kernelrpc_mach_vm_deallocate_trap resolves target to the CURRENT task and frees the
			// caller's REAL pages -> vm_map_remove -> task_free_pages -> a REAL caller-S2C munmap which, since
			// the caller is a ring-parked duplex parent, rides the duplex mailbox = ring_duplex_s2c>0 LIVE on
			// the real transport (real _s2cPerform munmap, real guest munmap pump, real fiber resume, real
			// final reply -- NOT a fake echo, NOT fabricated success). PARENT-CENTRIC guard: armed proof +
			// duplex-vm cap + shape (NOT pid -- the active pump-capable ring parent IS the carrier, set by
			// dispatching here with _ringDuplexParentActive). Budget-bounded + auto-disarm caps blast radius.
			// A decline here is ALWAYS pre-mutation (no double-effect).
			bool armed = d5VmDeallocProofArmed();
			bool capable = thread->duplexVmDeallocateCapable();
			bool goodShape = (reqlen == sizeof(dserver_call_mach_vm_deallocate_t));
			if (!armed || !capable || !goodShape) {
				dserver_ring_consumer_advance(c2s);
				thread->ring()->publishReply(seq, callnum, DSERVER_RING_DUPLEX_DECLINE, nullptr, 0);
				thread->ring()->wakeGuest();
				Metrics::shared().ringDuplexDecline.fetch_add(1, std::memory_order_relaxed);
				Metrics::shared().ringDuplexVmdeallocDecline.fetch_add(1, std::memory_order_relaxed);
				++serviced;
				continue;
			}
			// ARMED + pid-1 + capable + good shape: dispatch onto the duplex lane. Its munmap S2C (if the
			// freed range is server-managed -- which launchd's early-init vm_deallocates are) rides the
			// duplex mailbox and increments ring_duplex_vmdealloc_s2c -- the LIVE caller-S2C cure proof.
			Metrics::shared().ringDuplexParent.fetch_add(1, std::memory_order_relaxed);
			Metrics::shared().ringDuplexVmdeallocParent.fetch_add(1, std::memory_order_relaxed);
			size_t totalSize = sizeof(dserver_rpc_callhdr_t) + reqlen;
			Message reqMsg(totalSize, 0);
			reqMsg.data().resize(totalSize);
			auto* hdr = reinterpret_cast<dserver_rpc_callhdr_t*>(reqMsg.data().data());
			hdr->number = static_cast<dserver_callnum_t>(callnum);
			hdr->pid = process->nsid();
			hdr->tid = thread->nsid();
			hdr->architecture = static_cast<dserver_rpc_architecture_t>(process->architecture());
			memcpy(reqMsg.data().data() + sizeof(dserver_rpc_callhdr_t),
			       reinterpret_cast<const char*>(req) + sizeof(dserver_ring_slot_t), reqlen);
			reqMsg.setAddress(thread->address());
			reqMsg.setPID(process->id());
			dserver_ring_consumer_advance(c2s); // free the slot before running the op
			thread->setRingDuplexVmdeallocProof(true); // tag so _drainDuplexReply bumps the vmdealloc counters
			try {
				auto call = Call::callFromMessage(std::move(reqMsg));
				if (call) {
					// duplex parent (mach_vm_deallocate): see the deallocate block above -- the context
					// rides the Call so a caller-S2C raised later still finds this lane's mailbox.
					call->attachRingContext(thread->ring(), seq, callnum, true);
					call->thread()->doWork();
					Metrics::shared().ringDuplexVmdeallocFinal.fetch_add(1, std::memory_order_relaxed);
					++serviced;
				}
			} catch (const std::exception& ex) {
				callLog.error() << "ring duplex vm_deallocate dispatch threw: " << ex.what() << callLog.endLog;
			}
			thread->setRingDuplexVmdeallocProof(false);
			// AUTO-DISARM: if this dispatch produced a real caller-S2C (the proof goal), spend one budget
			// unit; once it hits 0 the proof is disarmed and no further launchd vm_deallocate rides the lane.
			if (thread->takeRingDuplexVmdeallocS2cFired()) {
				int prev = d5VmDeallocProofBudget().fetch_sub(1, std::memory_order_relaxed);
				if (prev <= 1) {
					Metrics::shared().ringDuplexVmdeallocDisarmed.fetch_add(1, std::memory_order_relaxed);
					callLog.error() << "[D5PROOF] launchd vm_deallocate caller-S2C cured over duplex lane; "
					                << "proof auto-disarmed (budget exhausted)" << callLog.endLog;
				}
			}
			continue;
		}


		// C2S allowlist: which call numbers may ride the ring. task_self_trap was the first
		// migration (correctness-first; cached per-process so it doesn't move latency);
		// mach_reply_port is the UNCACHED high-frequency port trap (empty body, {replyhdr.code,
		// uint32 port} reply). perf #18 P5 (dar-1il): mach_port_mod_refs joins -- it carries a
		// 4-arg request BODY (target,name,right,delta) and a HEADER-ONLY reply (the result is the
		// kern_return_t code, no port), so it exercises the generic body-copy datapath below
		// (NOT the empty-body surgical path). The Message is rebuilt as {callhdr, body} exactly as
		// over UDS and dispatched via callFromMessage -> the same MachPortModRefs::processCall ->
		// the same dtape primitive, so behavior is byte-identical to UDS. Anything not allowlisted
		// -> drop (consume so we don't spin); the guest will UDS-fall-back for it.
		// NOTE: this allowlist must NOT be gated by a per-op env hatch. A request that the guest
		// published here is parked waiting for a ring reply; silently dropping it (consume + no
		// reply) strands the guest on its bounded reply-wait every call (slow UDS re-fall-back per
		// op = effectively wedged). These ops ride the safe GENERIC fiber path, so they need no
		// fast-path kill-switch -- the whole-transport switch (DSERVER_RING_TRANSPORT / ABI
		// auto-fallback) is their safety valve, exactly like task_self_trap.
		//
		// perf #18 P5-bulk (dar-1il.1): the allowlist is GENERATED from DSERVER_RING_C2S_OPCODES
		// (rpc-supplement.h) -- the SAME macro the guest's ring dispatch consumes -- so the guest
		// "may publish" set and the server "will service" set can never drift (the no-silent-drop
		// invariant; ring_drift_gate_test.c pins it). To add an op: edit the macro in ONE place.
		// perf#26 RING-MACH-MSG: mach_msg_overwrite as a DUPLEX PARENT on the lane. It is deliberately
		// NOT in DSERVER_RING_C2S_OPCODES: it is a blocking, caller-S2C-capable op, and a ring-parked
		// caller can only service the caller-local munmap it can raise through the duplex mailbox. The
		// guest routes it here only after advertising CAP_MACH_MSG; we then dispatch it on the same
		// generic fiber path as every other eligible op, with _ringDuplexParentActive set for the whole
		// dispatch so the S2C takes the mailbox instead of the UDS send.
		//
		// Two declines, both pre-dispatch and therefore pre-mutation: a caller that never advertised the
		// cap, and an unexpected body shape. Both get an explicit DECLINE reply rather than a silent
		// drop, because the guest is parked on a ring reply for this slot.
		bool machMsgDuplex = false;
		if (callnum == (uint32_t)dserver_callnum_mach_msg_overwrite) {
			if (!thread->duplexMachMsgCapable() || reqlen != sizeof(dserver_call_mach_msg_overwrite_t)) {
				dserver_ring_consumer_advance(c2s);
				thread->ring()->publishReply(seq, callnum, DSERVER_RING_DUPLEX_DECLINE, nullptr, 0);
				thread->ring()->wakeGuest();
				Metrics::shared().ringMachMsgDecline.fetch_add(1, std::memory_order_relaxed);
				++serviced;
				continue;
			}
			machMsgDuplex = true;
		}

		bool eligible = false;
#define DSERVER_RING_C2S_ELIGIBLE(op) || (callnum == (uint32_t)dserver_callnum_##op)
		eligible = (false DSERVER_RING_C2S_OPCODES(DSERVER_RING_C2S_ELIGIBLE)) || machMsgDuplex;
#undef DSERVER_RING_C2S_ELIGIBLE
		if (!eligible || reqlen > inlineCap) {
			dserver_ring_consumer_advance(c2s);
			continue;
		}

		// Rebuild the UDS-format request: {callhdr, body}. For task_self_trap there is no body.
		// Copy the (bounded) inline body out of the slot into a server-owned Message buffer so
		// the guest can't race-mutate it after we validate.
		size_t totalSize = sizeof(dserver_rpc_callhdr_t) + reqlen;
		Message reqMsg(totalSize, 0);
		reqMsg.data().resize(totalSize);
		auto* hdr = reinterpret_cast<dserver_rpc_callhdr_t*>(reqMsg.data().data());
		hdr->number = static_cast<dserver_callnum_t>(callnum);
		// The call header carries the GUEST-NAMESPACE ids (nsid): callFromMessage() looks the
		// thread/process up in the registry keyed on nsid. Using the server-internal id() here
		// makes the lookup miss -> "non-existent thread" -> ESRCH -> the guest FUTEX_WAITs on a
		// reply that never comes (boot wedge). Mirror the real UDS path: header = nsid, and set
		// the Message's SCM-pid to the LINUX id() so callFromMessage's pid-consistency check
		// (process->id() == requestMessage.pid()) holds.
		hdr->pid = process->nsid();
		hdr->tid = thread->nsid();
		hdr->architecture = static_cast<dserver_rpc_architecture_t>(process->architecture());
		if (reqlen > 0) {
			memcpy(reqMsg.data().data() + sizeof(dserver_rpc_callhdr_t),
			       reinterpret_cast<const char*>(req) + sizeof(dserver_ring_slot_t),
			       reqlen);
		}
		reqMsg.setAddress(thread->address());
		reqMsg.setPID(process->id());

		// done reading the request slot; free it before running the call
		dserver_ring_consumer_advance(c2s);

		if (machMsgDuplex) {
			Metrics::shared().ringMachMsgParent.fetch_add(1, std::memory_order_relaxed);
			S2CTrace::line("RING_MACHMSG_CONSUME lane=%llu seq=%u tid=%lld", (unsigned long long)thread->ring()->debugLaneId(), (unsigned)seq, (long long)thread->nsid());
		}

		// NOTE: there is deliberately no dispatch-scoped reply sink any more. The reply destination (and
		// the duplex eligibility) is the CALL's RingContext, attached below just before doWork(); see
		// Call::RingContext for why dispatch-scoped Thread state cannot express either one.
#ifdef DSERVER_RING_PHASE_PROF
		uint64_t _phaseT1 = Metrics::rdtscCycles(); // drain end / dispatch start
#endif
		try {
			auto call = Call::callFromMessage(std::move(reqMsg));
#ifdef DSERVER_RING_PHASE_PROF
			uint64_t _phaseT2 = Metrics::rdtscCycles(); // dispatch end / body start
#endif
			if (call) {
				// Attach the call's ring transport BEFORE dispatching: the context must be there for
				// the reply (which may happen after a suspend/resume) and for a caller-S2C raised
				// while this call is parked.
				call->attachRingContext(thread->ring(), seq, callnum, machMsgDuplex);
				// Port minting and copyout take duct-tape locks, which can suspend
				// under contention. Ring transport never removes the fiber requirement.
				call->thread()->doWork();
				++serviced;
#ifdef DSERVER_RING_PHASE_PROF
				uint64_t _phaseT3 = Metrics::rdtscCycles(); // body end
				auto& m = Metrics::shared();
				// body = doWork window MINUS the publish cycles recorded inside publishReply for
				// this very call (publishReply ran during doWork, via pushCallReply). We read the
				// just-added publish delta back out of the thread's one-shot scratch.
				uint64_t pub = thread->takeRingPublishCycles();
				uint64_t bodyTotal = _phaseT3 - _phaseT2;
				uint64_t body = (bodyTotal > pub) ? (bodyTotal - pub) : 0;
				m.phaseDrainCycles.fetch_add(_phaseT1 - _phaseT0, std::memory_order_relaxed);
				m.phaseDispatchCycles.fetch_add(_phaseT2 - _phaseT1, std::memory_order_relaxed);
				m.phaseBodyCycles.fetch_add(body, std::memory_order_relaxed);
				m.phasePublishCycles.fetch_add(pub, std::memory_order_relaxed);
				m.phaseSamples.fetch_add(1, std::memory_order_relaxed);
#endif
			}
		} catch (const std::exception& ex) {
			callLog.error() << "ring C2S dispatch threw: " << ex.what() << callLog.endLog;
			// leave the ring-reply armed flag to be cleared on the next reply; the guest will
			// time out on this op and UDS-fall-back. Keep serving the rest.
		}
	}
	return serviced;
}
#endif

void DarlingServer::Call::RingAttach::processCall() {
#ifdef DSERVER_RING_TRANSPORT
	uint32_t rejectReason = dserver_ring_reject_total_size; // default-deny
	int code = 0;
	int guestWakeFd = -1; // dup of the wake eventfd handed back to the guest (-1 on reject)

	// perf#30 TRACE (temporary): the page route's third attach does not return from doWork(), so the path is
	// named step by step under the courier log rather than guessed at.
	auto traceAttach = [](const char* what, long v) {
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log tl("attach-trace");
			tl.error() << what << " " << v << tl.endLog;
		}
	};
	traceAttach("enter ring_fd=", (long)_body.ring_fd);
	traceAttach("enter fd_token=", (long)_body.fd_token);
	if (auto thread = _thread.lock()) {
		// perf#30 FD-COURIER (RA1): the lane's backing descriptor arrives on the process-scoped
		// SCM_RIGHTS courier and only its token is named here, so this request has no ancillary data at
		// all. Same two-half pairing the lifecycle ops use: resolve first (draining the courier), park
		// the continuation if the descriptor is still in flight, and never fall back to the RPC socket
		// after publication.
		if (_body.ring_fd < 0 && _body.fd_token != 0) {
			int courierFd = -1;
			std::weak_ptr<Thread> weakThread = thread;
			auto fdBox = std::make_shared<int>(-1);
			auto result = Server::sharedInstance().resolveFdCourierBundle(thread->process() ? thread->process()->id() : -1,
				_body.fd_token, DSERVER_FD_COURIER_KIND_LANE_BACKING, &courierFd,
				[fdBox, weakThread](int arrivedFd) {
					*fdBox = arrivedFd;
					if (auto strongThread = weakThread.lock()) {
						strongThread->resume();
					}
				});
			switch (result) {
				case Server::FdCourierResult::Resolved:
					_body.ring_fd = courierFd;
					break;
				case Server::FdCourierResult::Missing:
					thread->suspend([this, fdBox]() {
						if (*fdBox >= 0) {
							_body.ring_fd = *fdBox;
						}
						processCall();
					});
					if (*fdBox >= 0) {
						_body.ring_fd = *fdBox;
						processCall();
					} else {
						rejectReason = dserver_ring_reject_total_size;
						code = -EBADF;
					}
					return;
				default:
					rejectReason = dserver_ring_reject_total_size;
					code = -EBADF;
					break;
			}
		}

		traceAttach("pre-attach ring_fd=", (long)_body.ring_fd);
		if (_body.ring_fd < 0) {
			// no fd arrived -- can't be a valid attach
			rejectReason = dserver_ring_reject_total_size;
		} else {
			dserver_ring_reject_t reject = dserver_ring_reject_total_size;
			auto ring = RingBuffer::attach(
				_body.ring_fd,
				_body.mapping_size,
				static_cast<int32_t>(thread->nsid()),
				&reject
			);
			rejectReason = static_cast<uint32_t>(reject);
			traceAttach("post-RingBuffer::attach reject=", (long)reject);

			if (ring) {
				const auto& cb = ring->controlBlock();
				/* LANE-ATTACH RECORD, SERVER SIDE, AND THE IDENTITY LINK. The guest logs [dring-attach] per new
				 * thread; this is the same event as the SERVER sees it, carrying the thread's nsid (the guest's own host
				 * tid, the value the loader's fault witness prints) and the lane it was given. Plain write(2,...) because
				 * MEASURED: the server's stderr reaches the run log unconditionally, while the project's gated S2C trace
				 * does not appear there at all. Bounded. */
				{
					static unsigned g_srv_lane_records = 0;
					unsigned n = __atomic_fetch_add(&g_srv_lane_records, 1, __ATOMIC_RELAXED);
					if (n < 64) {
						char b[176];
						int l = snprintf(b, sizeof(b),
							"[srv-lane-attach #%u pid=%d tid=%d slots=%u slot_size=%u]\n",
							n, (int)thread->process()->id(), (int)thread->nsid(),
							(unsigned)cb.slot_count, (unsigned)cb.slot_size);
						if (l > 0) { (void)!write(2, b, (size_t)l); }
					}
				}
				callLog.debug() << "ring_attach accepted for TID " << thread->nsid()
					<< " (" << cb.slot_count << " slots of " << cb.slot_size << "B)"
					<< callLog.endLog;
				// perf#26 RING-MACH-MSG: the attach lifecycle, named. Pairs with the guest's
				// LANE_ATTACH line (which carries the guest lane index + generation) so a re-attach is
				// visible and a seq restart can never be mistaken for a duplicate request.
				S2CTrace::line("LANE_ATTACH tid=%lld lane=%llu", (long long)thread->nsid(),
				               (unsigned long long)ring->debugLaneId());

				// perf#28 (ONE doorbell): the guest gets a dup of the ONE process doorbell, not a
				// per-lane eventfd. The server registers that eventfd + its Monitor exactly once
				// (first attach, in Server::ringDoorbellDupForGuest) and every lane of every process
				// shares it: the wake scan (_drainRings) and the sleep-state publication are already
				// process-independent, so one channel serves them all. The guest side closes every
				// dup it receives after the first, which is what makes its wake-fd count O(1).
				// perf#28c: the doorbell belongs to the guest process INCARNATION (pidfd-anchored), so
				// every lane of this process -- in both images, across Process-object churn -- receives
				// a dup of the SAME kernel eventfd object, and no other process can reach it.
				auto process = thread->process();
				guestWakeFd = process ? Server::sharedInstance().ringDoorbellDupFor(process->id()) : -1;
				traceAttach("post-doorbell wake_fd=", (long)guestWakeFd);
				if (guestWakeFd == -2) {
					// perf#28d: the ONE delivery already happened for this process incarnation; the
					// guest resolves the doorbell from its own slot. This is SUCCESS with no descriptor
					// in the reply, so the ring is still attached and the thread still registered --
					// treating it as a failure is what sent the whole workload back to UDS.
					guestWakeFd = -1;
					Metrics::shared().ringDoorbellSuppressed.fetch_add(1, std::memory_order_relaxed);
					thread->attachRing(ring, nullptr);
					Server::sharedInstance().registerRingThread(thread);
					traceAttach("post-register tid=", (long)thread->nsid());
				} else if (guestWakeFd < 0) {
					rejectReason = static_cast<uint32_t>(dserver_ring_reject_total_size);
				} else {
					// The doorbell Monitor is server-owned (never per-Thread), so the Thread carries
					// no Monitor for it: a Thread death must not deregister the shared channel.
					thread->attachRing(ring, nullptr);
					// perf #18 P4: register the ring-owning thread with the server so the main
					// loop's pre-epoll spin phase can drain it directly (the doorbell is only the
					// COLD-path wake; on the hot path the guest skips it and the spin phase finds
					// the request by polling the c2s ring).
					Server::sharedInstance().registerRingThread(thread);
				}
			} else {
				callLog.info() << "ring_attach rejected for TID " << thread->nsid()
					<< " reason " << rejectReason << " -- thread stays on UDS" << callLog.endLog;
			}

			// perf #18 D15a (dar-1il.10): attach-timeline census. Record the outcome and, on success,
			// latch the ordinal at which the ring attached (= how many UDS calls this process ran
			// before the ring existed -- the size of the pre-attach window). guestWakeFd>=0 means the
			// ring was mapped + the thread registered (true success). No-op unless the census is armed.
			if (Metrics::shared().attachCensusOn.load(std::memory_order_relaxed)) {
				// perf#28d: success is "the ring is attached", which is now true with or without a
				// descriptor half in the reply; guestWakeFd >= 0 alone under-counted the steady state.
				bool success = (rejectReason == static_cast<uint32_t>(dserver_ring_ok));
				uint64_t ordinalAtAttach = 0;
				if (auto p = thread->process()) {
					ordinalAtAttach = p->currentUdsCallOrdinal();
					if (success) {
						p->markRingAttachedAtOrdinal(ordinalAtAttach == 0 ? 1 : ordinalAtAttach);
					}
				}
				Metrics::shared().recordAttachOutcome(success, rejectReason, ordinalAtAttach);
			}
		}
	} else {
		code = -ESRCH;
	}

	// perf#30: the descriptor half, named for the page route. A page-serviced attach has no datagram to
	// receive the reply CMSG, so `Server::_serviceProcessControl` sends this on the courier and passes the
	// token through the page -- which is what lets the ONE doorbell delivery happen without AF_UNIX.
	noteSuppressedReplyWakeFd(guestWakeFd);
	_sendReply(code, rejectReason, guestWakeFd);
#else
	// feature compiled out: a ring-capable guest gets a clean "unsupported" and falls back
	// to UDS. reject_reason is non-zero so the guest never believes the ring was accepted;
	// wake_fd is -1 (no ring).
	_sendReply(0, static_cast<uint32_t>(1) /* any non-ok */, -1);
#endif
};

DSERVER_CLASS_SOURCE_DEFS;
