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

#ifndef _DARLINGSERVER_CALL_HPP_
#define _DARLINGSERVER_CALL_HPP_

#include <darlingserver/rpc.h>
#include <darlingserver/rpc.internal.h>

#include <darlingserver/message.hpp>
#include <darlingserver/registry.hpp>
#include <darlingserver/logging.hpp>
#include <darlingserver/test-diagnostics.hpp>
#include <darlingserver/ring-call-context.hpp>

#include <memory>
#include <utility>
#include <optional>

#include <unistd.h>

namespace DarlingServer {
	class CallWithReply;
	class RingBuffer;

	class Call {
	public:
		enum class Number: unsigned int {
			Invalid = dserver_callnum_invalid,
			DSERVER_ENUM_VALUES
		};

		static inline const char* callNumberToString(Number number) {
			return dserver_callnum_to_string(static_cast<dserver_callnum_t>(number));
		};

	protected:
		std::weak_ptr<Thread> _thread;
		Address _replyAddress;
		dserver_rpc_callhdr_t _header;

		// perf#26 RING-MACH-MSG: the ring transport of a ring-originated call (see ring-call-context.hpp
		// for why this lives on the Call and not on the Thread, and for the lane lifetime rule).
	private:
		std::optional<RingCallContext> _ringContext;

	public:
		void attachRingContext(std::shared_ptr<class RingBuffer> ring, uint32_t seq, uint32_t callnum, bool duplexCapable);
		bool hasRingContext() const { return _ringContext.has_value(); }
		// nullptr when this call did not arrive over the ring (then its reply travels on UDS).
		const RingCallContext* ringContext() const { return _ringContext ? &*_ringContext : nullptr; }

		static DarlingServer::Log rpcReplyLog;

		static void sendReply(Message&& reply);

		// perf#30 DETACHED COMPLETION: the reply's DESTINATION belongs to the Call, not to the Thread
		// that happened to execute it. A terminal call (checkout) deliberately clears its Thread before
		// replying -- there is no thread left to answer through -- and until this existed the reply fell
		// through to the server's socket even when the call had arrived over a lane, so a ring-waiting
		// guest waited forever for an answer that was delivered somewhere else. MEASURED: with the lane
		// route enabled, checkout uds=0 ring=1 and shellspawn never became ready.
		//
		// The Call retains its RingCallContext (shared_ptr<RingBuffer>, request seq, callnum, duplex
		// flags) precisely so this is possible: the lane incarnation a request was published on outlives
		// every participant in it, and the reply must go back to THAT incarnation.
		void publishFinalReply(Message&& reply);
		// perf#30 FD-COURIER (round 49, SERVER->GUEST): hand a descriptor the CALL is returning to the
		// process that owns `pid` over the process-scoped courier, and return the token that names it.
		// Zero means the courier could not take it and the caller must keep the legacy CMSG reply.
		//
		// This lives on Call rather than being a direct Server call in the generated inline reply code
		// because the generated header only sees Call: MEASURED, calling Server::sharedInstance() there
		// fails with "incomplete type 'DarlingServer::Server' named in nested name specifier".
		uint64_t sendFdCourierToGuest(pid_t pid, uint32_t kind, int fd);
		// perf#30 PROCESS-CONTROL PLANE: a call whose caller is NOT waiting on any transport of ours --
		// it published through the management page and reads its answer there. Its reply must not become
		// a datagram (a stray reply desynchronizes the caller's next RPC), so the transport send is
		// skipped and the result code is captured for the page instead.
		void suppressReplyDelivery() { _replySuppressed = true; }
		// perf#30 (doc section 227): THE PLANE'S COMPLETION TARGET. A plane-serviced Call that is DISPATCHED rather
		// than run inline has no caller left to read `suppressedReplyCode()`, so the answer has to be written where
		// the waiter is looking -- the management page -- by whoever finishes the Call. MEASURED need: running the
		// Call inline from the plane pass held the single plane slot while the Call waited for a free guest thread,
		// and the guest's thread creation waits on that same slot (the deadlock of section 218/223).
		struct PlaneCompletion {
			void* page = nullptr;
			uint32_t seq = 0;
			int32_t pid = 0;
			bool armed() const { return page != nullptr; }
		};
		void setPlaneCompletion(void* page, uint32_t seq, int32_t pid) { _planeCompletion = PlaneCompletion{ page, seq, pid }; }
		const PlaneCompletion& planeCompletion() const { return _planeCompletion; }
		bool replySuppressed() const { return _replySuppressed; }
		int suppressedReplyCode() const { return _suppressedReplyCode; }
		void noteSuppressedReply(int32_t code) { _suppressedReplyCode = code; }
		// perf#30 ATTACH_LANE: a suppressed reply still CARRIES a body (reject reason + descriptor token).
		// The page route has no datagram to read them from, so the body is kept here, bounded.
		void noteSuppressedReplyBody(const void* data, size_t len) {
			if (!data) { _suppressedReplyBodyLen = 0; return; }
			if (len > sizeof(_suppressedReplyBody)) { len = sizeof(_suppressedReplyBody); }
			memcpy(_suppressedReplyBody, data, len);
			_suppressedReplyBodyLen = len;
		}
		const uint8_t* suppressedReplyBody(size_t* outLen) const {
			if (outLen) { *outLen = _suppressedReplyBodyLen; }
			return _suppressedReplyBody;
		}
		// perf#30: the descriptor the reply WOULD have carried. On the page route there is no datagram to
		// carry it, so the page's servicing code sends it on the courier itself and reports the token.
		void noteSuppressedReplyWakeFd(int fd) { _suppressedReplyWakeFd = fd; }
		int suppressedReplyWakeFd() const { return _suppressedReplyWakeFd; }
	private:
		bool _replySuppressed = false;
		PlaneCompletion _planeCompletion;
		int _suppressedReplyCode = 0;
		uint8_t _suppressedReplyBody[128] = {};
		size_t _suppressedReplyBodyLen = 0;
		int _suppressedReplyWakeFd = -1;
	public:
		/** Publish a reply to a retained ring context; false means the caller must fall back to UDS. */
		static bool publishReplyToRingContext(const RingCallContext& ctx, Message& reply);

	public:
		// Send a minimal error reply (header only: {number, code}) to the client that
		// sent `requestMessage`, WITHOUT a constructed Call object. Used at the message
		// ingest paths in callFromMessage() that would otherwise silently drop a message
		// (non-existent process/thread, pending-call-overwrite). A waiting guest thread
		// is parked in recvmsg on its per-thread RPC socket; if its call is dropped with
		// no reply it hangs forever (and under a fork/signal storm degrades into a SEGV).
		// For a call that expects a reply *body*, the guest's RPC wrapper sees a short
		// reply and returns -ECOMM; for a body-less call it returns `code` (e.g. -ESRCH).
		// Either way the guest syscall RETURNS instead of hanging. (dar-gwn.6.2)
		static void sendErrorReplyFromHeader(const dserver_rpc_callhdr_t* header, Address replyAddress, int code);
		Call(std::shared_ptr<Thread> thread, Address replyAddress, dserver_rpc_callhdr_t* callHeader);
		virtual ~Call();

		static std::shared_ptr<Call> callFromMessage(Message&& requestMessage);

		// perf#30 (doc section 219): the registration/arming block of a call's ingest, reachable WITHOUT building a
		// Call -- the management plane services a checkin through this when no guest thread can execute anything, and
		// the datagram path calls the SAME function, so the two routes cannot drift apart.
		static std::pair<std::shared_ptr<Process>, std::shared_ptr<Thread>>
		registerPeerForMessage(Message& requestMessage, dserver_rpc_callhdr_t* header, bool replyErrors,
			std::shared_ptr<Process> reuseProcess /* = nullptr */);

		// perf #18 D15a (dar-1il.10): is this raw call number a member of the simple-ring C2S
		// allowlist (DSERVER_RING_C2S_OPCODES)? Used ONLY by the attach-timeline census to classify a
		// pre-attach UDS call as "would have ridden the ring had it been attached". Defined in call.cpp
		// where the shared macro is already in scope; returns false when the ring transport is compiled
		// out. NOT a dispatch predicate -- the real eligibility gate is in ringServiceThread.
		static bool ringEligibleCallnum(uint32_t callNumber);

		virtual Number number() const = 0;
		std::shared_ptr<Thread> thread() const;

		virtual void processCall() = 0;

		virtual void sendBasicReply(int resultCode);
		// FIXME: this should actually be in a "BSD" subclass with BSD traps inheriting from it
		virtual void sendBSDReply(int resultCode, uint32_t returnValue);

		virtual bool isXNUTrap() const;
		virtual bool isBSDTrap() const;

		DSERVER_CLASS_DECLS;
	};

	DSERVER_CLASS_DEFS;
};

#endif // _DARLINGSERVER_CALL_HPP_
