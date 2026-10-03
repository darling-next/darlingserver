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

#include <darlingserver/debug-introspection.hpp>
#include <darlingserver/debug-serialize.hpp>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace DarlingServer::Debug {

	namespace {
		enum class ParseError {
			None = 0,
			BadSignature,
			BadAbiVersion,
			BadPair,
			UnknownKey,
			BadValue,
			TrailingData,
		};

		bool splitPair(const char* token, size_t length, char* key, size_t keyCapacity, char* value, size_t valueCapacity) {
			const char* equals = static_cast<const char*>(memchr(token, '=', length));
			if (equals == nullptr) {
				return false;
			}
			const size_t keyLength = static_cast<size_t>(equals - token);
			const size_t valueLength = length - keyLength - 1;
			if (keyLength >= keyCapacity || valueLength >= valueCapacity) {
				return false;
			}
			memcpy(key, token, keyLength);
			key[keyLength] = '\0';
			memcpy(value, equals + 1, valueLength);
			value[valueLength] = '\0';
			return true;
		}

		bool parseInt32(const char* value, int32_t& out) {
			if (value[0] == '\0') return false;
			errno = 0;
			char* end = nullptr;
			const long parsed = strtol(value, &end, 10);
			if (errno != 0 || end == value || *end != '\0') return false;
			if (parsed < INT32_MIN || parsed > INT32_MAX) return false;
			out = static_cast<int32_t>(parsed);
			return true;
		}

		bool parseUint32(const char* value, uint32_t& out) {
			if (value[0] == '\0') return false;
			errno = 0;
			char* end = nullptr;
			const unsigned long parsed = strtoul(value, &end, 10);
			if (errno != 0 || end == value || *end != '\0') return false;
			if (parsed > UINT32_MAX) return false;
			out = static_cast<uint32_t>(parsed);
			return true;
		}

		bool parseUint64(const char* value, uint64_t& out) {
			if (value[0] == '\0') return false;
			errno = 0;
			char* end = nullptr;
			const unsigned long long parsed = strtoull(value, &end, 10);
			if (errno != 0 || end == value || *end != '\0') return false;
			out = static_cast<uint64_t>(parsed);
			return true;
		}
	}

	bool debugAbiEnabledFor(const char* setting) {
		return setting != nullptr && strcmp(setting, "1") == 0;
	}

	bool peerIsAuthorized(uid_t peerUid, uid_t serverUid) {
		// Same-UID only. A different uid is refused outright; there is no
		// privileged override, and no alternative transport to reach the same state.
		return peerUid == serverUid;
	}

	FrameStatus frameRequest(const char* data, size_t length, bool eof, size_t& outRequestLength) {
		outRequestLength = 0;
		const void* newline = memchr(data, '\n', length);
		if (newline != nullptr) {
			outRequestLength = static_cast<size_t>(static_cast<const char*>(newline) - data);
			return FrameStatus::Ready;
		}
		// No terminator yet. Exceeding the bound is rejected even before a newline
		// arrives, so an unterminated flood cannot grow the buffer.
		if (length > kMaxRequestBytes) {
			return FrameStatus::TooLong;
		}
		// At EOF the buffered bytes are all the caller will get: hand them to the
		// parser, which decides whether they are a valid request. An unterminated
		// but well-formed request is accepted; anything else becomes an explicit
		// error document rather than silence.
		if (eof) {
			outRequestLength = length;
			return FrameStatus::Ready;
		}
		return FrameStatus::NeedMore;
	}

	ParseResult parseRequest(const char* data, size_t length, Request& out) {
		// Strip exactly one trailing newline (and an optional carriage return).
		while (length > 0 && (data[length - 1] == '\n' || data[length - 1] == '\r')) {
			--length;
		}
		if (length == 0) {
			return {ParseStatus::Empty, "empty request"};
		}
		if (length > kMaxRequestBytes) {
			return {ParseStatus::TooLong, "request exceeds the ABI request bound"};
		}

		std::string text(data, length);
		size_t position = 0;
		int tokenIndex = 0;
		char key[32];
		char value[64];

		while (position < text.size()) {
			while (position < text.size() && text[position] == ' ') {
				++position;
			}
			if (position >= text.size()) {
				break;
			}
			size_t tokenEnd = text.find(' ', position);
			if (tokenEnd == std::string::npos) {
				tokenEnd = text.size();
			}
			const std::string token = text.substr(position, tokenEnd - position);
			position = tokenEnd;

			switch (tokenIndex) {
				case 0:
					if (token != kRequestSignature) {
						return {ParseStatus::BadSignature, "request must start with the ABI signature"};
					}
					break;
				case 1:
					if (!parseUint32(token.c_str(), out.callerAbiVersion)) {
						return {ParseStatus::BadAbiVersion, "ABI version must be an unsigned integer"};
					}
					break;
				case 2:
					out.op.set(token.c_str());
					if (out.op.bytes[0] == '\0') {
						return {ParseStatus::BadValue, "operation name is empty or too long"};
					}
					break;
				default:
					if (!splitPair(token.c_str(), token.size(), key, sizeof(key), value, sizeof(value))) {
						return {ParseStatus::BadPair, "arguments must be key=value"};
					}
					if (strcmp(key, "host_pid") == 0) {
						if (!parseInt32(value, out.hostPid)) return {ParseStatus::BadValue, "host_pid must be an int32"};
						out.hasHostPid = true;
					} else if (strcmp(key, "host_tid") == 0) {
						if (!parseInt32(value, out.hostTid)) return {ParseStatus::BadValue, "host_tid must be an int32"};
						out.hasHostTid = true;
					} else if (strcmp(key, "server_pid") == 0) {
						if (!parseInt32(value, out.serverPid)) return {ParseStatus::BadValue, "server_pid must be an int32"};
						out.hasServerPid = true;
					} else if (strcmp(key, "server_tid") == 0) {
						if (!parseInt32(value, out.serverTid)) return {ParseStatus::BadValue, "server_tid must be an int32"};
						out.hasServerTid = true;
					} else if (strcmp(key, "nsid") == 0) {
						if (!parseInt32(value, out.nsid)) return {ParseStatus::BadValue, "nsid must be an int32"};
						out.hasNsid = true;
					} else if (strcmp(key, "eternal_id") == 0) {
						if (!parseUint64(value, out.eternalId)) return {ParseStatus::BadValue, "eternal_id must be a uint64"};
						out.hasEternalId = true;
					} else {
						return {ParseStatus::UnknownKey, "unknown argument key"};
					}
					break;
			}
			++tokenIndex;
		}

		if (tokenIndex < 3) {
			return {ParseStatus::TrailingData, "request must name an operation"};
		}
		return {ParseStatus::Ok, ""};
	}

	namespace {
		std::string jsonQuote(const char* value) {
			std::string out = "\"";
			for (const char* it = value; *it != '\0'; ++it) {
				const unsigned char c = static_cast<unsigned char>(*it);
				switch (c) {
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if (c < 0x20) {
							char escaped[8];
							snprintf(escaped, sizeof(escaped), "\\u%04x", c);
							out += escaped;
						} else {
							out.push_back(static_cast<char>(c));
						}
						break;
				}
			}
			out += "\"";
			return out;
		}

		const char* statusToCode(ParseStatus status) {
			switch (status) {
				case ParseStatus::Empty: return "empty_request";
				case ParseStatus::TooLong: return "request_too_long";
				case ParseStatus::MissingSignature: return "missing_signature";
				case ParseStatus::BadSignature: return "bad_signature";
				case ParseStatus::BadAbiVersion: return "bad_abi_version";
				case ParseStatus::BadPair: return "bad_argument";
				case ParseStatus::UnknownKey: return "unknown_argument";
				case ParseStatus::BadValue: return "bad_argument_value";
				case ParseStatus::TrailingData: return "missing_operation";
				case ParseStatus::Ok: default: return "ok";
			}
		}
	}

	std::string errorDocument(const char* code, const char* reason, const std::vector<std::string>& knownOps, size_t maxBytes) {
		std::string out = "{\"abi\":" + jsonQuote(kAbiName);
		out += ",\"abi_version\":";
		out += std::to_string(static_cast<unsigned long long>(kAbiVersion));
		out += ",\"error\":";
		out += jsonQuote(code);
		out += ",\"reason\":";
		out += jsonQuote(reason);
		out += ",\"truncated\":false";
		if (!knownOps.empty()) {
			out += ",\"known_ops\":[";
			for (size_t i = 0; i < knownOps.size(); ++i) {
				if (i != 0) out += ",";
				out += jsonQuote(knownOps[i].c_str());
			}
			out += "]";
		}
		out += "}";
		if (out.size() > maxBytes) {
			// The caller's budget cannot hold the error; report the least that is
			// still valid JSON, or nothing at all.
			const std::string banner = serializeAbiBanner(maxBytes);
			return banner;
		}
		return out;
	}

	std::string handleRequestLine(Collector& collector, const char* data, size_t length, size_t maxBytes) {
		Request request;
		const ParseResult parsed = parseRequest(data, length, request);
		if (!parsed.ok()) {
			return errorDocument(statusToCode(parsed.status), parsed.reason, {}, maxBytes);
		}

		// Version handshake: a mismatch is reported explicitly with both versions
		// rather than being guessed at, because the response schema is versioned.
		if (request.callerAbiVersion != kAbiVersion) {
			std::string out = "{\"abi\":" + jsonQuote(kAbiName);
			out += ",\"abi_version\":";
			out += std::to_string(static_cast<unsigned long long>(kAbiVersion));
			out += ",\"error\":\"abi_version_mismatch\",\"caller_abi_version\":";
			out += std::to_string(static_cast<unsigned long long>(request.callerAbiVersion));
			out += ",\"truncated\":false}";
			if (out.size() > maxBytes) {
				return serializeAbiBanner(maxBytes);
			}
			return out;
		}

		const std::vector<std::string> knownOps = collector.operations();
		bool known = false;
		for (const auto& op: knownOps) {
			if (op == request.op.bytes) {
				known = true;
				break;
			}
		}
		if (!known) {
			return errorDocument("unknown_operation", "this server build does not implement the requested operation", knownOps, maxBytes);
		}

		// A broken target must produce an error document, not a dead server: the
		// collector reads live, concurrently-mutating state, so any read can fail
		// while the rest of the server keeps serving real guests.
		TargetSnapshot snapshot;
		try {
			snapshot = collector.collect(request);
		} catch (const std::exception& error) {
			std::string out = "{\"abi\":" + jsonQuote(kAbiName);
			out += ",\"abi_version\":";
			out += std::to_string(static_cast<unsigned long long>(kAbiVersion));
			out += ",\"error\":\"collector_exception\",\"reason\":";
			out += jsonQuote(error.what());
			out += ",\"truncated\":false}";
			if (out.size() > maxBytes) {
				return serializeAbiBanner(maxBytes);
			}
			return out;
		} catch (...) {
			return errorDocument("collector_exception", "non-std exception while collecting the snapshot", {}, maxBytes);
		}

		snapshot.abiVersion = kAbiVersion;
		// The caller-visible op name is echoed so a debugger can attribute a
		// response without keeping its own correlation table.
		snapshot.op = request.op;
		if (snapshot.availableOpCount == 0) {
			for (size_t i = 0; i < knownOps.size() && i < kMaxOps; ++i) {
				snapshot.availableOps[i].set(knownOps[i].c_str());
				snapshot.availableOpCount = static_cast<uint32_t>(i) + 1;
			}
		}

		const SerializeResult result = serializeSnapshotJSON(snapshot, maxBytes);
		return result.json;
	}

};
