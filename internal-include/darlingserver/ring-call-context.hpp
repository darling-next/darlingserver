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

#ifndef _DARLINGSERVER_RING_CALL_CONTEXT_HPP_
#define _DARLINGSERVER_RING_CALL_CONTEXT_HPP_

#include <cstdint>
#include <memory>

namespace DarlingServer {
	class RingBuffer;

	// perf#26 RING-MACH-MSG: the ring transport of a call that arrived over a lane.
	//
	// This belongs to the CALL, not to the Thread, and that is the whole point. A blocking op suspends
	// inside Thread::doWork() -- which then RETURNS to the main loop -- and may produce its reply, or
	// raise its caller-S2C, minutes later. Any state stored on the Thread and scoped to the dispatch is
	// therefore already gone when it is needed, which is exactly how a ring-parked caller used to lose
	// both its reply routing and its duplex eligibility.
	//
	// Lifetime rule (deliberate, not incidental): the shared_ptr keeps the RingBuffer object alive until
	// every Call that referenced it has been destroyed. A lane retire or re-attach constructs a NEW
	// RingBuffer, so a stale Call publishes into the OLD lane -- where nobody is waiting -- instead of
	// writing into a new lane's reply stream. No raw pointer to a lane is ever held by a Call.
	struct RingCallContext {
		std::shared_ptr<RingBuffer> ring; // holds the lane alive for as long as this call can reply to it
		uint32_t seq = 0;                 // the request's lane-local sequence number
		uint32_t callnum = 0;             // the request's call number (diagnostic + validation)
		// true iff this call's owner advertised that it services the caller-S2C shapes this lane may
		// carry; only then may the server route a caller-S2C into the lane's duplex mailbox.
		bool duplexCapable = false;
	};
};

#endif // _DARLINGSERVER_RING_CALL_CONTEXT_HPP_
