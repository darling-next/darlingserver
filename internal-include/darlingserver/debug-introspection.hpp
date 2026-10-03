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

#ifndef _DARLINGSERVER_DEBUG_INTROSPECTION_HPP_
#define _DARLINGSERVER_DEBUG_INTROSPECTION_HPP_

// The protocol layer of the read-only debug introspection ABI.
//
// This header deliberately depends on nothing but the ABI schema, so the whole
// request/response contract - parsing, version handshake, error reporting, and
// bounds - is executable and testable on the host, with a test-supplied
// Collector, without a Darling runtime. The live implementation of Collector
// (which reads real server objects) is the only part that needs the server.

#include <darlingserver/debug-abi.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace DarlingServer::Debug {

	static constexpr const char* kRequestSignature = "darling-debug";
	static constexpr size_t kMaxRequestBytes = 512;

	// A parsed request. Every target selector is optional and independent: the
	// debugger may know only a host tid, only an NSID, or only a server id, and
	// the collector is responsible for resolving whichever one it got.
	struct Request {
		uint32_t callerAbiVersion = 0;
		Text op {};
		int32_t hostPid = -1;
		int32_t hostTid = -1;
		int32_t serverPid = -1;
		int32_t serverTid = -1;
		int32_t nsid = -1;
		uint64_t eternalId = 0;
		bool hasHostPid = false;
		bool hasHostTid = false;
		bool hasServerPid = false;
		bool hasServerTid = false;
		bool hasNsid = false;
		bool hasEternalId = false;
	};

	enum class ParseStatus {
		Ok = 0,
		Empty,
		TooLong,
		MissingSignature,
		BadSignature,
		BadAbiVersion,
		BadPair,
		UnknownKey,
		BadValue,
		TrailingData,
	};

	struct ParseResult {
		ParseStatus status = ParseStatus::Ok;
		const char* reason = "";

		bool ok() const { return status == ParseStatus::Ok; }
	};

	// Parses one request line. `data` need not be NUL-terminated; a single
	// trailing '\n' or '\r' is ignored. The op name is not validated here: it is
	// validated at dispatch against the collector, so an older server can name
	// the operations it does implement.
	ParseResult parseRequest(const char* data, size_t length, Request& out);

	// The seam between the protocol layer and live server state.
	class Collector {
	public:
		virtual ~Collector() = default;

		// Fills the requested snapshot. MUST NOT throw (the caller still contains
		// throws defensively) and MUST NOT mutate guest, server, or transport
		// state. An unresolvable target is reported as !known identities plus an
		// explanatory `detail`, never as a zero/false value.
		virtual TargetSnapshot collect(const Request& request) = 0;

		// Operations this server build implements, in documentation order.
		virtual std::vector<std::string> operations() const = 0;

		// The server's own identity, for the artifact section and for ordering.
		virtual uint64_t nextSequence() { return 0; }
	};

	// Whether the debug ABI should be served at all, given the value of
	// DSERVER_DEBUG_ABI. The surface is OPT-IN: exactly "1" enables it, and every
	// other value - unset, empty, "0", "off", "no", or anything unrecognised -
	// leaves it disabled. Kept here, pure, so the policy is testable without a
	// server and cannot drift from the listener that consults it.
	bool debugAbiEnabledFor(const char* setting);

	// Peer authorization for the abstract-namespace socket.
	//
	// The endpoint lives in the Linux abstract AF_UNIX namespace, where filesystem
	// mode bits do not apply: any local process that knows the name can connect.
	// The authorization policy is therefore the peer credential, not a path. For
	// the first developer interface, same-UID is sufficient and is checked on
	// EVERY accepted connection; no password or token is involved, and there is no
	// guest RPC fallback to reach the state another way.
	bool peerIsAuthorized(uid_t peerUid, uid_t serverUid);

	// Framing policy: ONE request per accepted connection, one response, then
	// close. Requests are newline-terminated and bounded; bytes after the first
	// newline belong to no request and are ignored, which is why a second request
	// on the same connection is never answered.
	enum class FrameStatus {
		/// No newline yet and the bound is not exceeded: read more.
		NeedMore = 0,
		/// A complete request is present; `outRequestLength` covers it (no newline).
		Ready,
		/// The bound was exceeded before a newline arrived: reject and close.
		TooLong,
	};

	FrameStatus frameRequest(const char* data, size_t length, bool eof, size_t& outRequestLength);

	// Handles one complete request and returns the response text (JSON, no
	// trailing newline). Never throws, and never returns more than `maxBytes`
	// bytes: a failure of any kind becomes an error document, not a truncated
	// stack trace or a dead server.
	std::string handleRequestLine(Collector& collector, const char* data, size_t length, size_t maxBytes);

	// Renders an error document within a byte budget. Exposed for the tests and
	// for callers that must report a failure before a Collector exists.
	std::string errorDocument(const char* code, const char* reason, const std::vector<std::string>& knownOps, size_t maxBytes);

};

#endif // _DARLINGSERVER_DEBUG_INTROSPECTION_HPP_
