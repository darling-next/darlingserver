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

#ifndef _DARLINGSERVER_DEBUG_COLLECT_HPP_
#define _DARLINGSERVER_DEBUG_COLLECT_HPP_

// The live side of the read-only debug introspection ABI: it reads the server's
// own process/thread registries and fills the dependency-free ABI PODs.
//
// It reaches that state exclusively through Thread's and Process's EXISTING
// public accessors - it is not a friend of either class, and it reads no private
// member directly. That is deliberate: the ABI must not become a second, silent
// coupling to internal layout, and a field that has no accessor is reported as
// not exposed rather than read past the class boundary.
//
// Every method here is READ-ONLY. It must never call anything that mutates the
// guest, a transport, or a thread's execution state.

#include <darlingserver/debug-abi.hpp>
#include <darlingserver/debug-introspection.hpp>

namespace DarlingServer {
	class DebugCollector: public Debug::Collector {
	public:
		DebugCollector();

		Debug::TargetSnapshot collect(const Debug::Request& request) override;
		std::vector<std::string> operations() const override;
		uint64_t nextSequence() override;

	private:
		void _fillIdentitiesByThread(Debug::TargetSnapshot& snapshot) const;
		void _fillLists(Debug::TargetSnapshot& snapshot) const;
	};

};

#endif // _DARLINGSERVER_DEBUG_COLLECT_HPP_
