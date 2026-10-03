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

#ifndef _DARLINGSERVER_DEBUG_ABI_HPP_
#define _DARLINGSERVER_DEBUG_ABI_HPP_

// Darling read-only debug introspection ABI (epic: Darling native debugger).
//
// This is a DEVELOPER/DEBUG surface, not a product transport:
//   - it is READ-ONLY: no operation may mutate guest, server, or transport state;
//   - it is VERSIONED: every response carries the ABI version it was produced by;
//   - it is BOUNDED: every response has a hard byte budget and reports truncation;
//   - it is EXPLICITLY TYPED: fixed-width PODs, no `int`, no pointers, no STL layout;
//   - it is SAFE WHEN PARTIALLY BROKEN: a section that this server build cannot
//     describe is reported as Unavailable WITH A REASON, never as a zero value.
//
// It deliberately does not depend on any darlingserver internal type, so the
// schema, the serializer, and their tests can be built and executed on the host
// without a Darling runtime, and so a debugger never has to guess private C++
// layout, member offsets, or the compiler ABI.
//
// Availability semantics are the core of the design:
//   Unavailable -> the section is not carried by this server build; the debugger
//                  must print "unavailable (reason)", not an empty/false value.
//   Partial     -> some fields of the section are carried, others are not; every
//                  struct carries per-section `detail` text.
//   Complete    -> every field of the section is carried.

#include <cstdint>
#include <cstddef>

namespace DarlingServer::Debug {

	static constexpr uint32_t kAbiVersion = 1;
	static constexpr const char* kAbiName = "darling-debug";
	static constexpr size_t kMaxPath = 256;
	static constexpr size_t kMaxText = 96;

	enum class Availability: uint8_t {
		Unavailable = 0,
		Partial = 1,
		Complete = 2,
	};

	static inline const char* availabilityToString(Availability availability) {
		switch (availability) {
			case Availability::Complete: return "complete";
			case Availability::Partial: return "partial";
			case Availability::Unavailable:
			default: return "unavailable";
		}
	}

	// Bounded, NUL-terminated, always-terminated fixed text.
	struct Text {
		char bytes[kMaxText] = {};

		void set(const char* value);
	};

	struct Path {
		char bytes[kMaxPath] = {};

		void set(const char* value);
	};

	// Where the debugger's request came from, and the process it named.
	struct HostIdentity {
		int32_t pid = -1;
		int32_t tid = -1;
		// fd number of the pidfd anchor in the SERVER's fd table, or -1. A host
		// pid alone is not an identity: PIDs are reused, and the server observes
		// two Linux pids for one process across namespaces.
		int32_t pidfd = -1;
		bool known = false;
	};

	// The guest's own view of the same process/thread.
	struct GuestIdentity {
		int32_t pid = -1;
		int32_t tid = -1;
		bool known = false;
	};

	struct ProcessIdentity {
		int32_t id = -1;
		int32_t nsid = -1;
		uint64_t eternalId = 0;
		bool known = false;
	};

	struct ThreadIdentity {
		int32_t id = -1;
		int32_t nsid = -1;
		uint64_t eternalId = 0;
		bool known = false;
	};

	// A single RPC call owned by a thread. `present == false` means the slot is
	// empty, which is different from "the server cannot report it".
	struct CallSnapshot {
		bool present = false;
		uint32_t number = 0;
		Text name {};
		int32_t headerPid = -1;
		int32_t headerTid = -1;
		uint32_t headerArchitecture = 0;
	};

	struct ProcessSnapshot {
		Availability availability = Availability::Unavailable;
		Text detail {};
		ProcessIdentity identity {};
		int32_t parentId = -1;
		int32_t tracerId = -1;
		uint32_t architecture = 0;
		bool is64Bit = false;
		bool dead = false;
		bool startSuspended = false;
		bool pendingReplacement = false;
		Path executablePath {};
		Path vchrootPath {};
		// Memory summary, straight from Process::memoryInfo().
		bool memoryKnown = false;
		uint64_t virtualSize = 0;
		uint64_t residentSize = 0;
		uint64_t pageSize = 0;
		uint64_t regionCount = 0;
		// Number of threads the process registry currently holds.
		uint32_t threadCount = 0;
	};

	// Thread execution/wait state. This is the state a lost-wake or a hang
	// investigation needs, and it is not reachable from outside the server today.
	struct ThreadSnapshot {
		Availability availability = Availability::Unavailable;
		Text detail {};
		ThreadIdentity identity {};
		bool addressed = false; // Thread::address() known
		uint64_t address = 0;
		int32_t processId = -1;
		// RunState as reported by dtape_thread_state_t (dead/running/stopped/
		// interruptible/uninterruptible) - the value the server itself uses.
		uint32_t runState = 0;
		Text runStateName {};
		bool dead = false;
		bool terminating = false;
		// Execution ownership, separate from the saved wait context.
		bool suspended = false;
		bool resumePermit = false;
		bool hasContinuation = false;
		// DeferralState (not-deferred / deferred-not-pending / deferred-pending)
		uint32_t deferralState = 0;
		Text deferralStateName {};
		bool pendingCallOverride = false;
		bool waitingForReply = false;
		// Signal / interrupt path.
		uint32_t interruptDepth = 0;
		uint32_t pendingInterrupts = 0;
		int32_t pendingSignal = 0;
		bool interruptedForSignal = false;
		bool didSyscallReturnDuringInterrupt = false;
		bool handlingInterruptedCall = false;
		// S2C reply deferral: the fields that decide whether a reply is owed.
		bool deferReplyForS2C = false;
		bool hasDeferredReply = false;
		bool hasS2cReply = false;
		bool impersonating = false;
		// The Call slots, in the order the server itself transitions them.
		CallSnapshot activeCall {};
		CallSnapshot pendingCall {};
	};

	// dtape (duct-tape) state for the same thread, joined by the pointer the
	// server already holds plus the XNU-side identity.
	struct DtapeThreadSnapshot {
		Availability availability = Availability::Unavailable;
		Text detail {};
		bool known = false;
		uint64_t taskAddress = 0;
		uint64_t threadAddress = 0;
		uint64_t xnuThreadId = 0;
		uint32_t suspendCount = 0;
		bool waitingSuspended = false;
		// The XNU wait context: what the thread waits on and what will retire it.
		uint64_t waitChannel = 0;
		Text waitMessage {};
		uint64_t continuation = 0;
		// Cancellation state (disabled / pending / canceled).
		bool cancelDisabled = false;
		bool cancelPending = false;
		bool cancelCanceled = false;
		bool processingSignal = false;
		bool fatalExceptionDelivery = false;
		// Armed wait timer, if any.
		bool timerArmed = false;
		int64_t timerDeadlineNanos = 0;
	};

	// The wait/waker answer, assembled from the fields above. This exists as its
	// own section because "who is supposed to wake this thread" is the single
	// question every lost-wake investigation asked.
	struct WaitSnapshot {
		Availability availability = Availability::Unavailable;
		Text detail {};
		uint32_t runState = 0;
		Text runStateName {};
		bool waitingForReply = false;
		bool suspended = false;
		bool hasContinuation = false;
		uint64_t waitChannel = 0;
		Text waitMessage {};
		bool timerArmed = false;
		int64_t timerDeadlineNanos = 0;
		// The expected waker, named as concretely as this build can:
		//   "reply:<callnum>"       - a pending RPC reply must arrive
		//   "s2c:<name>"            - a server-to-client call must complete
		//   "timer:<deadline_ns>"   - the armed wait timer must fire
		//   "continuation"          - a queued continuation must run
		//   "unknown"               - this build cannot name it
		Text expectedWaker {};
	};

	// Transport is a first-class dimension, not something the debugger infers.
	// On a server build that does not carry ring/process-control state, the
	// section is Unavailable with the exact reason, so a debugger shows
	// "unavailable" instead of fabricating a class.
	struct TransportSnapshot {
		Availability availability = Availability::Unavailable;
		Text detail {};
		// uds | ring | plane | urgent | unknown
		Text transportClass {};
		int64_t laneId = -1;
		uint64_t sequence = 0;
		Text state {};
		Text expectedWaker {};
	};

	struct ArtifactSnapshot {
		Availability availability = Availability::Unavailable;
		Text detail {};
		Path serverExecutable {};
		Path guestExecutable {};
		// dyld info, as the server already tracks it for a process.
		bool dyldInfoKnown = false;
		uint64_t dyldInfoAddress = 0;
		uint64_t dyldInfoLength = 0;
		// Filled by the host-side tool, which can hash the file; the server
		// reports identity anchors (paths), not digests.
	};

	struct Sections {
		Availability process = Availability::Unavailable;
		Availability thread = Availability::Unavailable;
		Availability dtape = Availability::Unavailable;
		Availability wait = Availability::Unavailable;
		Availability transport = Availability::Unavailable;
		Availability artifacts = Availability::Unavailable;
	};

	// Discovery is part of the ABI because attaching to "the guest process" is
	// itself a recurring hand-rolled step. The list is structurally bounded: a
	// fixed array plus an explicit truncation flag, so a large workload can never
	// turn a debug query into an unbounded response.
	static constexpr size_t kMaxListEntries = 64;
	static constexpr size_t kMaxOps = 16;

	struct IdentityEntry {
		ProcessIdentity process {};
		ThreadIdentity thread {};
		HostIdentity host {};
		GuestIdentity guest {};
	};

	struct IdentityList {
		uint32_t count = 0;
		bool truncated = false;
		IdentityEntry entries[kMaxListEntries] = {};
	};

	struct TargetSnapshot {
		uint32_t abiVersion = kAbiVersion;
		// Monotonic per-server sequence, so two snapshots are orderable.
		uint64_t sequence = 0;
		bool truncated = false;
		// Ops/keys the caller asked for; bounded.
		Text op {};
		HostIdentity host {};
		GuestIdentity guest {};
		ProcessSnapshot process {};
		ThreadSnapshot thread {};
		DtapeThreadSnapshot dtape {};
		WaitSnapshot wait {};
		TransportSnapshot transport {};
		ArtifactSnapshot artifacts {};
		Sections sections {};
		// Operations this server build implements, so a client can fail clearly
		// on an older server instead of guessing.
		uint32_t availableOpCount = 0;
		Text availableOps[kMaxOps] = {};
		IdentityList list {};
	};

	// Bounded copy into fixed text. Truncates on a UTF-8-agnostic byte boundary;
	// the field is always NUL-terminated.
	void setBounded(char* destination, size_t capacity, const char* value);

};

#endif // _DARLINGSERVER_DEBUG_ABI_HPP_
