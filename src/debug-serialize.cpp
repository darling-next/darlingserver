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

#include <darlingserver/debug-serialize.hpp>

#include <cstdio>
#include <type_traits>
#include <cstring>

namespace DarlingServer::Debug {

	namespace {
		// The largest prefix of `length` bytes that ends on a UTF-8 character
		// boundary. A torn sequence is not a fact a debugger should print.
		size_t utf8SafeLength(const char* bytes, size_t length) {
			size_t i = 0;
			while (i < length) {
				const unsigned char lead = static_cast<unsigned char>(bytes[i]);
				size_t expected = 1;
				if ((lead & 0x80) == 0x00) expected = 1;
				else if ((lead & 0xe0) == 0xc0) expected = 2;
				else if ((lead & 0xf0) == 0xe0) expected = 3;
				else if ((lead & 0xf8) == 0xf0) expected = 4;
				else return i; // invalid lead: stop before it
				if (i + expected > length) return i; // incomplete sequence
				for (size_t k = 1; k < expected; ++k) {
					if ((static_cast<unsigned char>(bytes[i + k]) & 0xc0) != 0x80) {
						return i; // malformed continuation
					}
				}
				i += expected;
			}
			return i;
		}
	}

	void setBounded(char* destination, size_t capacity, const char* value) {
		if (capacity == 0) {
			return;
		}
		if (value == nullptr) {
			destination[0] = '\0';
			return;
		}
		size_t length = strnlen(value, capacity - 1);
		length = utf8SafeLength(value, length);
		memcpy(destination, value, length);
		destination[length] = '\0';
	}

	void Text::set(const char* value) {
		setBounded(bytes, sizeof(bytes), value);
	}

	void Path::set(const char* value) {
		setBounded(bytes, sizeof(bytes), value);
	}

	namespace {
		std::string quote(const char* value) {
			std::string out;
			out.reserve(strlen(value) + 2);
			out.push_back('"');
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
			out.push_back('"');
			return out;
		}

		std::string quote(const Text& text) { return quote(text.bytes); }
		std::string quote(const Path& path) { return quote(path.bytes); }

		template <typename T, typename = std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value>>
		std::string number(T value) {
			return std::to_string(value);
		}

		template <typename T, typename = std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value>>
		std::string field(const char* key, T value) {
			return std::string(",\"") + key + "\":" + number(value);
		}

		std::string boolField(const char* key, bool value) {
			return std::string(",\"") + key + "\":" + (value ? "true" : "false");
		}

		std::string textField(const char* key, const char* value) {
			return std::string(",\"") + key + "\":" + quote(value);
		}

		std::string textField(const char* key, const Text& value) {
			return textField(key, value.bytes);
		}

		std::string pathField(const char* key, const Path& value) {
			return std::string(",\"") + key + "\":" + quote(value);
		}

		std::string callBlock(const CallSnapshot& call) {
			std::string out = "{\"present\":";
			out += call.present ? "true" : "false";
			if (call.present) {
				out += field("number", static_cast<unsigned long long>(call.number));
				out += textField("name", call.name);
				out += field("header_pid", static_cast<long long>(call.headerPid));
				out += field("header_tid", static_cast<long long>(call.headerTid));
				out += field("header_arch", static_cast<unsigned long long>(call.headerArchitecture));
			}
			out += "}";
			return out;
		}

		std::string hostBlock(const HostIdentity& host) {
			std::string out = "{\"known\":";
			out += host.known ? "true" : "false";
			out += field("pid", static_cast<long long>(host.pid));
			out += field("tid", static_cast<long long>(host.tid));
			out += field("pidfd", static_cast<long long>(host.pidfd));
			out += "}";
			return out;
		}

		std::string guestBlock(const GuestIdentity& guest) {
			std::string out = "{\"known\":";
			out += guest.known ? "true" : "false";
			out += field("pid", static_cast<long long>(guest.pid));
			out += field("tid", static_cast<long long>(guest.tid));
			out += "}";
			return out;
		}

		std::string processIdentityBlock(const ProcessSnapshot& process) {
			std::string out = "{\"known\":";
			out += process.identity.known ? "true" : "false";
			out += field("server_pid", static_cast<long long>(process.identity.id));
			out += field("guest_pid", static_cast<long long>(process.identity.nsid));
			out += field("eternal_id", process.identity.eternalId);
			out += "}";
			return out;
		}

		std::string threadIdentityBlock(const ThreadSnapshot& thread) {
			std::string out = "{\"known\":";
			out += thread.identity.known ? "true" : "false";
			out += field("server_tid", static_cast<long long>(thread.identity.id));
			out += field("guest_tid", static_cast<long long>(thread.identity.nsid));
			out += field("eternal_id", thread.identity.eternalId);
			out += field("process_server_pid", static_cast<long long>(thread.processId));
			out += "}";
			return out;
		}

		std::string sectionsBlock(const Sections& sections) {
			std::string out = "{\"process\":";
			out += quote(availabilityToString(sections.process));
			out += ",\"thread\":";
			out += quote(availabilityToString(sections.thread));
			out += ",\"dtape\":";
			out += quote(availabilityToString(sections.dtape));
			out += ",\"wait\":";
			out += quote(availabilityToString(sections.wait));
			out += ",\"transport\":";
			out += quote(availabilityToString(sections.transport));
			out += ",\"artifacts\":";
			out += quote(availabilityToString(sections.artifacts));
			out += "}";
			return out;
		}

		std::string processBlock(const ProcessSnapshot& process) {
			std::string out = "{\"availability\":";
			out += quote(availabilityToString(process.availability));
			out += textField("detail", process.detail);
			out += ",\"identity\":";
			out += processIdentityBlock(process);
			out += field("parent_server_pid", static_cast<long long>(process.parentId));
			out += field("tracer_server_pid", static_cast<long long>(process.tracerId));
			out += field("architecture", static_cast<unsigned long long>(process.architecture));
			out += boolField("is64bit", process.is64Bit);
			out += boolField("dead", process.dead);
			out += boolField("start_suspended", process.startSuspended);
			out += boolField("pending_replacement", process.pendingReplacement);
			out += pathField("executable_path", process.executablePath);
			out += pathField("vchroot_path", process.vchrootPath);
			out += boolField("memory_known", process.memoryKnown);
			out += field("virtual_size", process.virtualSize);
			out += field("resident_size", process.residentSize);
			out += field("page_size", process.pageSize);
			out += field("region_count", process.regionCount);
			out += field("thread_count", static_cast<unsigned long long>(process.threadCount));
			out += "}";
			return out;
		}

		std::string threadBlock(const ThreadSnapshot& thread) {
			std::string out = "{\"availability\":";
			out += quote(availabilityToString(thread.availability));
			out += textField("detail", thread.detail);
			out += ",\"identity\":";
			out += threadIdentityBlock(thread);
			out += boolField("addressed", thread.addressed);
			out += field("address", thread.address);
			out += field("run_state", static_cast<unsigned long long>(thread.runState));
			out += textField("run_state_name", thread.runStateName);
			out += boolField("dead", thread.dead);
			out += boolField("terminating", thread.terminating);
			out += boolField("suspended", thread.suspended);
			out += boolField("resume_permit", thread.resumePermit);
			out += boolField("has_continuation", thread.hasContinuation);
			out += field("deferral_state", static_cast<unsigned long long>(thread.deferralState));
			out += textField("deferral_state_name", thread.deferralStateName);
			out += boolField("pending_call_override", thread.pendingCallOverride);
			out += boolField("waiting_for_reply", thread.waitingForReply);
			out += field("interrupt_depth", static_cast<unsigned long long>(thread.interruptDepth));
			out += field("pending_interrupts", static_cast<unsigned long long>(thread.pendingInterrupts));
			out += field("pending_signal", static_cast<long long>(thread.pendingSignal));
			out += boolField("interrupted_for_signal", thread.interruptedForSignal);
			out += boolField("did_syscall_return_during_interrupt", thread.didSyscallReturnDuringInterrupt);
			out += boolField("handling_interrupted_call", thread.handlingInterruptedCall);
			out += boolField("defer_reply_for_s2c", thread.deferReplyForS2C);
			out += boolField("has_deferred_reply", thread.hasDeferredReply);
			out += boolField("has_s2c_reply", thread.hasS2cReply);
			out += boolField("impersonating", thread.impersonating);
			out += ",\"active_call\":";
			out += callBlock(thread.activeCall);
			out += ",\"pending_call\":";
			out += callBlock(thread.pendingCall);
			out += "}";
			return out;
		}

		std::string dtapeBlock(const DtapeThreadSnapshot& dtape) {
			std::string out = "{\"availability\":";
			out += quote(availabilityToString(dtape.availability));
			out += textField("detail", dtape.detail);
			out += boolField("known", dtape.known);
			out += field("task_address", dtape.taskAddress);
			out += field("thread_address", dtape.threadAddress);
			out += field("xnu_thread_id", dtape.xnuThreadId);
			out += field("suspend_count", static_cast<unsigned long long>(dtape.suspendCount));
			out += boolField("waiting_suspended", dtape.waitingSuspended);
			out += field("wait_channel", dtape.waitChannel);
			out += textField("wait_message", dtape.waitMessage);
			out += field("continuation", dtape.continuation);
			out += boolField("cancel_disabled", dtape.cancelDisabled);
			out += boolField("cancel_pending", dtape.cancelPending);
			out += boolField("cancel_canceled", dtape.cancelCanceled);
			out += boolField("processing_signal", dtape.processingSignal);
			out += boolField("fatal_exception_delivery", dtape.fatalExceptionDelivery);
			out += boolField("timer_armed", dtape.timerArmed);
			out += field("timer_deadline_ns", dtape.timerDeadlineNanos);
			out += "}";
			return out;
		}

		std::string waitBlock(const WaitSnapshot& wait) {
			std::string out = "{\"availability\":";
			out += quote(availabilityToString(wait.availability));
			out += textField("detail", wait.detail);
			out += field("run_state", static_cast<unsigned long long>(wait.runState));
			out += textField("run_state_name", wait.runStateName);
			out += boolField("waiting_for_reply", wait.waitingForReply);
			out += boolField("suspended", wait.suspended);
			out += boolField("has_continuation", wait.hasContinuation);
			out += field("wait_channel", wait.waitChannel);
			out += textField("wait_message", wait.waitMessage);
			out += boolField("timer_armed", wait.timerArmed);
			out += field("timer_deadline_ns", wait.timerDeadlineNanos);
			out += ",\"expected_waker\":";
			out += quote(wait.expectedWaker);
			out += "}";
			return out;
		}

		std::string transportBlock(const TransportSnapshot& transport) {
			std::string out = "{\"availability\":";
			out += quote(availabilityToString(transport.availability));
			out += textField("detail", transport.detail);
			out += textField("class", transport.transportClass);
			out += field("lane_id", transport.laneId);
			out += field("sequence", transport.sequence);
			out += textField("state", transport.state);
			out += ",\"expected_waker\":";
			out += quote(transport.expectedWaker);
			out += "}";
			return out;
		}

		std::string artifactsBlock(const ArtifactSnapshot& artifacts) {
			std::string out = "{\"availability\":";
			out += quote(availabilityToString(artifacts.availability));
			out += textField("detail", artifacts.detail);
			out += pathField("server_executable", artifacts.serverExecutable);
			out += pathField("guest_executable", artifacts.guestExecutable);
			out += boolField("dyld_info_known", artifacts.dyldInfoKnown);
			out += field("dyld_info_address", artifacts.dyldInfoAddress);
			out += field("dyld_info_length", artifacts.dyldInfoLength);
			out += "}";
			return out;
		}

		std::string opsBlock(const TargetSnapshot& snapshot) {
			std::string out = "{\"count\":";
			out += number(snapshot.availableOpCount);
			out += ",\"names\":[";
			for (uint32_t i = 0; i < snapshot.availableOpCount && i < kMaxOps; ++i) {
				if (i != 0) out += ",";
				out += quote(snapshot.availableOps[i]);
			}
			out += "]}";
			return out;
		}

		std::string listBlock(const IdentityList& list) {
			std::string out = "{\"count\":";
			out += number(list.count);
			out += boolField("truncated", list.truncated);
			out += ",\"entries\":[";
			const uint32_t count = list.count > kMaxListEntries ? static_cast<uint32_t>(kMaxListEntries) : list.count;
			for (uint32_t i = 0; i < count; ++i) {
				const IdentityEntry& entry = list.entries[i];
				if (i != 0) out += ",";
				out += "{\"server_pid\":";
				out += number(entry.process.id);
				out += ",\"guest_pid\":";
				out += number(entry.process.nsid);
				out += ",\"process_eternal_id\":";
				out += number(entry.process.eternalId);
				out += ",\"server_tid\":";
				out += number(entry.thread.id);
				out += ",\"guest_tid\":";
				out += number(entry.thread.nsid);
				out += ",\"thread_eternal_id\":";
				out += number(entry.thread.eternalId);
				out += ",\"host_pid\":";
				out += number(entry.host.pid);
				out += ",\"host_tid\":";
				out += number(entry.host.tid);
				out += "}";
			}
			out += "]}";
			return out;
		}

		// A fragment is appended whole or not at all: a half-written section would
		// be indistinguishable from a server that lacks the state.
		struct Fragment {
			const char* name;
			std::string body;
		};
	}

	namespace {
		std::string emitTruncatedBanner(const std::vector<Fragment>& fragments, size_t maxBytes, bool* outEmitted) {
			std::string out = "{\"abi\":" + quote(kAbiName) + field("abi_version", static_cast<unsigned long long>(kAbiVersion)) + ",\"truncated\":true,\"dropped\":[";
			for (size_t i = 0; i < fragments.size(); ++i) {
				if (i != 0) out += ",";
				out += quote(fragments[i].name);
			}
			out += "]}";
			if (out.size() > maxBytes) {
				*outEmitted = false;
				return std::string();
			}
			*outEmitted = true;
			return out;
		}
	}

	std::string serializeAbiBanner(size_t maxBytes) {
		std::string out = "{\"abi\":" + quote(kAbiName) + field("abi_version", static_cast<unsigned long long>(kAbiVersion)) + "}";
		if (out.size() > maxBytes) {
			// Never emit an over-budget payload; a caller that gave us no room
			// gets nothing rather than something invalid.
			return std::string();
		}
		return out;
	}

	SerializeResult serializeSnapshotJSON(const TargetSnapshot& snapshot, size_t maxBytes) {
		SerializeResult result;

		std::string core = "{\"abi\":" + quote(kAbiName);
		core += field("abi_version", static_cast<unsigned long long>(snapshot.abiVersion));
		core += field("sequence", snapshot.sequence);
		core += textField("op", snapshot.op);
		core += ",\"host\":";
		core += hostBlock(snapshot.host);
		core += ",\"guest\":";
		core += guestBlock(snapshot.guest);
		core += ",\"process_identity\":";
		core += processIdentityBlock(snapshot.process);
		core += ",\"thread_identity\":";
		core += threadIdentityBlock(snapshot.thread);
		core += ",\"ops\":";
		core += opsBlock(snapshot);
		core += ",\"list\":";
		core += listBlock(snapshot.list);
		core += ",\"sections\":";
		core += sectionsBlock(snapshot.sections);

		std::vector<Fragment> fragments;
		fragments.push_back({"process", processBlock(snapshot.process)});
		fragments.push_back({"thread", threadBlock(snapshot.thread)});
		fragments.push_back({"wait", waitBlock(snapshot.wait)});
		fragments.push_back({"dtape", dtapeBlock(snapshot.dtape)});
		fragments.push_back({"transport", transportBlock(snapshot.transport)});
		fragments.push_back({"artifacts", artifactsBlock(snapshot.artifacts)});

		// Pass 1: does everything fit with the minimal (non-truncated) footer?
		// Reporting truncation when nothing had to be dropped would be a false
		// alarm, so the honest answer is checked first.
		{
			std::string body = core;
			for (const auto& fragment: fragments) {
				body += std::string(",\"") + fragment.name + "\":" + fragment.body;
			}
			body += ",\"truncated\":false}";
			if (body.size() <= maxBytes) {
				result.json = std::move(body);
				result.truncated = false;
				return result;
			}
		}

		// Pass 2: drop whole sections, in priority order, and report which ones.
		std::string body = core;
		std::vector<std::string> dropped;
		// The footer that reports truncation must itself fit, so reserve
		// worst-case room for the longest possible footer before admitting any
		// section. The worst case is a footer naming every fragment.
		size_t footerReserve = strlen(",\"truncated\":true,\"dropped\":[]");
		for (const auto& fragment: fragments) {
			footerReserve += strlen(fragment.name) + 3; // two quotes and a separator
		}

		if (body.size() + footerReserve > maxBytes) {
			// Even the identity core cannot fit: say exactly that, and nothing else.
			bool emitted = false;
			std::string banner = emitTruncatedBanner(fragments, maxBytes, &emitted);
			if (emitted) {
				result.json = std::move(banner);
				result.truncated = true;
				for (const auto& fragment: fragments) {
					result.dropped.push_back(fragment.name);
				}
				return result;
			}
			result.json = serializeAbiBanner(maxBytes);
			result.truncated = !result.json.empty();
			for (const auto& fragment: fragments) {
				result.dropped.push_back(fragment.name);
			}
			return result;
		}

		for (const auto& fragment: fragments) {
			const std::string appended = std::string(",\"") + fragment.name + "\":" + fragment.body;
			if (body.size() + appended.size() + footerReserve > maxBytes) {
				dropped.push_back(fragment.name);
				continue;
			}
			body += appended;
		}

		body += ",\"truncated\":true,\"dropped\":[";
		for (size_t i = 0; i < dropped.size(); ++i) {
			if (i != 0) {
				body += ",";
			}
			body += quote(dropped[i].c_str());
		}
		body += "]}";
		result.truncated = true;
		result.dropped = dropped;
		result.json = std::move(body);
		return result;
	}

};
