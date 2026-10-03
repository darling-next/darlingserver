/**
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling developers
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

#ifndef _DARLINGSERVER_DEBUG_SERIALIZE_HPP_
#define _DARLINGSERVER_DEBUG_SERIALIZE_HPP_

#include <darlingserver/debug-abi.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace DarlingServer::Debug {

	struct SerializeResult {
		std::string json;
		bool truncated = false;
		// Names of sections that did not fit the byte budget, in drop order.
		std::vector<std::string> dropped;
	};

	// Serializes a snapshot to JSON, never exceeding `maxBytes` bytes.
	//
	// The core block (ABI version, sequence, op, the three identity spaces, and
	// the per-section availability map) is always emitted: those are what let a
	// debugger tell "absent state" from "state the server cannot describe".
	// Remaining sections are appended in priority order while the budget allows.
	// Any section that does not fit is named in `dropped` and the result is
	// marked truncated, so truncation is always observable and never silent.
	SerializeResult serializeSnapshotJSON(const TargetSnapshot& snapshot, size_t maxBytes);

	// Minimal always-emitted ABI banner, used by `hello`/version probes and as
	// the last-resort response when even the core block cannot fit a budget.
	std::string serializeAbiBanner(size_t maxBytes);

	// The hard upper bound a server places on a single response, so a debug query
	// can never turn into an unbounded allocation or write.
	static constexpr size_t kDefaultMaxResponseBytes = 64 * 1024;

};

#endif // _DARLINGSERVER_DEBUG_SERIALIZE_HPP_
