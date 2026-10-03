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

#include <darlingserver/debug-collect.hpp>
#include <darlingserver/debug-serialize.hpp>
#include <darlingserver/process.hpp>
#include <darlingserver/thread.hpp>
#include <darlingserver/call.hpp>
#include <darlingserver/registry.hpp>

#include <unistd.h>

#include <atomic>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace DarlingServer {

	namespace {
		std::atomic<uint64_t> g_debugSequence{0};

		const char* runStateName(Thread::RunState state) {
			switch (state) {
				case Thread::RunState::Dead: return "dead";
				case Thread::RunState::Running: return "running";
				case Thread::RunState::Stopped: return "stopped";
				case Thread::RunState::Interruptible: return "interruptible";
				case Thread::RunState::Uninterruptible: return "uninterruptible";
				default: return "unknown";
			}
		}

		// Call identity is deliberately limited to what Call exposes publicly.
		// The call header (guest pid/tid, architecture) lives behind the class's
		// protected member today, so the ABI reports it as unreported rather than
		// guessing at a private layout - the exact failure mode this ABI exists to
		// prevent.
		void fillCall(const std::shared_ptr<Call>& call, Debug::CallSnapshot& out, bool& headerExposed) {
			if (!call) {
				out.present = false;
				return;
			}
			out.present = true;
			out.number = static_cast<uint32_t>(call->number());
			out.name.set(Call::callNumberToString(call->number()));
			headerExposed = false;
		}

		void fillWait(Debug::TargetSnapshot& snapshot) {
			Debug::WaitSnapshot& wait = snapshot.wait;
			wait.availability = Debug::Availability::Partial;
			wait.detail.set("wait state from server fields; XNU wait channel needs the duct-tape layout");

			wait.runState = snapshot.thread.runState;
			wait.runStateName = snapshot.thread.runStateName;
			wait.waitingForReply = snapshot.thread.waitingForReply;
			wait.suspended = snapshot.thread.suspended;
			wait.hasContinuation = snapshot.thread.hasContinuation;
			wait.waitChannel = snapshot.dtape.waitChannel;
			wait.waitMessage = snapshot.dtape.waitMessage;
			wait.timerArmed = snapshot.dtape.timerArmed;
			wait.timerDeadlineNanos = snapshot.dtape.timerDeadlineNanos;

		// The waker is INTERPRETATION, so the exporter does not synthesize it: it
		// copies the facts (waiting_for_reply, timer_armed/deadline, suspended,
		// has_continuation) and leaves `expected_waker` empty unless a future
		// server build actually knows the waker as state (for example once ring
		// or process-control state records who was notified). Deriving a name
		// from those facts is the Rust core's job, not the exporter's.
		wait.expectedWaker.set("");
	}
	}

	DebugCollector::DebugCollector() {
	}

	uint64_t DebugCollector::nextSequence() {
		return g_debugSequence.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	std::vector<std::string> DebugCollector::operations() const {
		return {
			"hello",
			"identity_map",
			"list_processes",
			"list_threads",
			"process_info",
			"thread_info",
			"thread_current",
			"wait",
			"transport_current",
			"artifacts",
		};
	}

	void DebugCollector::_fillIdentitiesByThread(Debug::TargetSnapshot& snapshot) const {
		const Debug::ThreadSnapshot& thread = snapshot.thread;
		snapshot.guest.known = thread.identity.known;
		snapshot.guest.pid = snapshot.process.identity.known ? snapshot.process.identity.nsid : -1;
		snapshot.guest.tid = thread.identity.known ? thread.identity.nsid : -1;
	}

	void DebugCollector::_fillLists(Debug::TargetSnapshot& snapshot) const {
		snapshot.list.count = 0;
		snapshot.list.truncated = false;

		auto processes = processRegistry().copyEntries();
		for (const auto& process: processes) {
			if (!process) {
				continue;
			}
			auto threads = process->threads();
			if (threads.empty()) {
				if (snapshot.list.count >= Debug::kMaxListEntries) {
					snapshot.list.truncated = true;
					break;
				}
				Debug::IdentityEntry& entry = snapshot.list.entries[snapshot.list.count++];
				entry.process.known = true;
				entry.process.id = process->id();
				entry.process.nsid = process->nsid();
				entry.process.eternalId = process->eternalID();
				entry.host.known = true;
				entry.host.pid = process->id();
				entry.host.tid = -1;
				continue;
			}
			for (const auto& thread: threads) {
				if (!thread) {
					continue;
				}
				if (snapshot.list.count >= Debug::kMaxListEntries) {
					snapshot.list.truncated = true;
					break;
				}
				Debug::IdentityEntry& entry = snapshot.list.entries[snapshot.list.count++];
				entry.process.known = true;
				entry.process.id = process->id();
				entry.process.nsid = process->nsid();
				entry.process.eternalId = process->eternalID();
				entry.thread.known = true;
				entry.thread.id = thread->id();
				entry.thread.nsid = thread->nsid();
				entry.thread.eternalId = thread->eternalID();
				entry.host.known = true;
				entry.host.pid = process->id();
				entry.host.tid = thread->id();
				entry.guest.known = true;
				entry.guest.pid = process->nsid();
				entry.guest.tid = thread->nsid();
			}
			if (snapshot.list.truncated) {
				break;
			}
		}
	}

	Debug::TargetSnapshot DebugCollector::collect(const Debug::Request& request) {
		Debug::TargetSnapshot snapshot;
		snapshot.sequence = nextSequence();
		snapshot.op = request.op;

		// Resolve a thread first: most operations are thread-shaped because the
		// recurring question is about one thread's Call and wait state.
		std::shared_ptr<Thread> thread;
		if (request.hasHostTid) {
			if (auto found = threadRegistry().lookupEntryByID(request.hostTid)) {
				thread = *found;
			}
		}
		if (!thread && request.hasServerTid) {
			if (auto found = threadRegistry().lookupEntryByID(request.serverTid)) {
				thread = *found;
			}
		}
		if (!thread && request.hasNsid) {
			if (auto found = threadRegistry().lookupEntryByNSID(request.nsid)) {
				thread = *found;
			}
		}
		if (!thread && request.hasEternalId) {
			if (auto found = threadRegistry().lookupEntryByEternalID(request.eternalId)) {
				thread = *found;
			}
		}

		std::shared_ptr<Process> process;
		if (thread) {
			process = thread->process();
		}
		if (!process && request.hasServerPid) {
			if (auto found = processRegistry().lookupEntryByID(request.serverPid)) {
				process = *found;
			}
		}
		if (!process && request.hasNsid) {
			if (auto found = processRegistry().lookupEntryByNSID(request.nsid)) {
				process = *found;
			}
		}

		// Discovery and identity-only operations.
		const bool wantsList = (std::strcmp(request.op.bytes, "list_processes") == 0) ||
		                       (std::strcmp(request.op.bytes, "list_threads") == 0);
		const bool identityOnly = (std::strcmp(request.op.bytes, "hello") == 0) ||
		                          (std::strcmp(request.op.bytes, "identity_map") == 0);

		if (wantsList) {
			_fillLists(snapshot);
		}

		if (process) {
			Debug::ProcessSnapshot& out = snapshot.process;
			out.availability = Debug::Availability::Partial;
			out.detail.set("server process fields; pidfd anchor and call headers not exposed by this build");
			out.identity.known = true;
			out.identity.id = process->id();
			out.identity.nsid = process->nsid();
			out.identity.eternalId = process->eternalID();
			if (auto parent = process->parentProcess()) {
				out.parentId = parent->id();
			}
			if (auto tracer = process->tracerProcess()) {
				out.tracerId = tracer->id();
			}
			out.architecture = static_cast<uint32_t>(process->architecture());
			out.is64Bit = process->is64Bit();
			out.dead = process->isDead();
			out.startSuspended = process->startSuspended();
			out.threadCount = static_cast<uint32_t>(process->threads().size());
			try {
				out.executablePath.set(process->executablePath().c_str());
			} catch (...) {
				out.executablePath.set("");
			}
			try {
				out.vchrootPath.set(process->vchrootPath().c_str());
			} catch (...) {
				out.vchrootPath.set("");
			}
			try {
				const Process::MemoryInfo info = process->memoryInfo();
				out.memoryKnown = true;
				out.virtualSize = info.virtualSize;
				out.residentSize = info.residentSize;
				out.pageSize = info.pageSize;
				out.regionCount = info.regionCount;
			} catch (...) {
				out.memoryKnown = false;
			}

			snapshot.host.known = true;
			snapshot.host.pid = process->id();
			snapshot.host.tid = -1;

			snapshot.artifacts.availability = Debug::Availability::Partial;
			snapshot.artifacts.detail.set("paths only; the host tool hashes the files");
			char selfPath[Debug::kMaxPath] = {};
			const ssize_t selfLength = readlink("/proc/self/exe", selfPath, sizeof(selfPath) - 1);
			if (selfLength > 0) {
				selfPath[selfLength] = '\0';
				snapshot.artifacts.serverExecutable.set(selfPath);
			}
			snapshot.artifacts.guestExecutable = out.executablePath;
		}

		if (thread) {
			Debug::ThreadSnapshot& out = snapshot.thread;
			out.availability = Debug::Availability::Partial;
			out.detail.set("server thread and Call state; call header fields and XNU fields not exposed by this build");

			out.identity.known = true;
			out.identity.id = thread->id();
			out.identity.nsid = thread->nsid();
			out.identity.eternalId = thread->eternalID();
			if (process) {
				out.processId = process->id();
			}
			// The thread's Address is a sockaddr_un for its RPC socket; it is not
			// a scalar identity, and this build exposes no field that carries it,
			// so the ABI says so instead of inventing a number.
			out.addressed = false;
			out.address = 0;

			const Thread::RunState state = thread->getRunState();
			out.runState = static_cast<uint32_t>(state);
			out.runStateName.set(runStateName(state));
			out.dead = thread->isDead();
			out.suspended = thread->isCurrentlySuspended();
			out.waitingForReply = thread->waitingForReply();
			out.pendingSignal = thread->pendingSignal();
			out.impersonating = thread->impersonatingThread() != nullptr;

			bool activeHeaderExposed = false;
			bool pendingHeaderExposed = false;
			fillCall(thread->activeCall(), out.activeCall, activeHeaderExposed);
			fillCall(thread->pendingCall(), out.pendingCall, pendingHeaderExposed);

			snapshot.host.known = true;
			snapshot.host.tid = thread->id();
		}

		if (!identityOnly && !wantsList && thread) {
			// Wait state is assembled from the thread and dtape sections.
			snapshot.dtape.availability = Debug::Availability::Partial;
			snapshot.dtape.detail.set("cancellation state only; wait channel/continuation need the duct-tape layout");
			snapshot.dtape.known = true;
			Debug::WaitSnapshot& wait = snapshot.wait;
			wait.availability = Debug::Availability::Partial;
			fillWait(snapshot);
		}

		// Transport is a first-class dimension, and this build does not carry it.
		// Reporting an explicit "unavailable" with the reason is the whole point:
		// a debugger must not read a missing ring/plane state as "no transport".
		snapshot.transport.availability = Debug::Availability::Unavailable;
		snapshot.transport.detail.set("this server build records no per-thread transport class (UDS/ring/plane/courier)");
		// Left empty on purpose: the exporter copies state, it does not name the
		// waker. The core derives a waker from the facts it did receive.
		snapshot.transport.expectedWaker.set("");

		if (wantsList) {
			snapshot.process.availability = Debug::Availability::Unavailable;
			snapshot.thread.availability = Debug::Availability::Unavailable;
			snapshot.sections.process = Debug::Availability::Unavailable;
			snapshot.sections.thread = Debug::Availability::Unavailable;
			snapshot.sections.dtape = Debug::Availability::Unavailable;
			snapshot.sections.wait = Debug::Availability::Unavailable;
			snapshot.sections.transport = Debug::Availability::Unavailable;
			snapshot.sections.artifacts = Debug::Availability::Unavailable;
			return snapshot;
		}

		if (identityOnly && !process && !thread && !wantsList) {
			snapshot.process.availability = Debug::Availability::Unavailable;
			snapshot.process.detail.set("no process matched the requested identity");
			snapshot.thread.availability = Debug::Availability::Unavailable;
			snapshot.thread.detail.set("no thread matched the requested identity");
		}

		snapshot.sections.process = snapshot.process.availability;
		snapshot.sections.thread = snapshot.thread.availability;
		snapshot.sections.dtape = snapshot.dtape.availability;
		snapshot.sections.wait = snapshot.wait.availability;
		snapshot.sections.transport = snapshot.transport.availability;
		snapshot.sections.artifacts = snapshot.artifacts.availability;

		_fillIdentitiesByThread(snapshot);
		return snapshot;
	}

};
