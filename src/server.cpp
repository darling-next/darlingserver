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

#include <cstdint>
#include <darlingserver/server.hpp>
#include <darlingserver/s2c-trace.hpp>
#include <sys/socket.h>
#include <stdexcept>
#include <errno.h>
#include <cstring>
#include <unistd.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <system_error>
#include <thread>
#include <array>
#include <sstream>
#include <cstddef>
#include <unordered_map>
#include <filesystem>
#include <darlingserver/registry.hpp>
#include <sys/eventfd.h>
#include <poll.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <set>
#include <csignal>
#include <darlingserver/duct-tape.h>
// The event ring lives in the duct-tape diagnostics unit, whose header is not on this target's include path; the
// declaration is therefore made here. It is one function with no state of its own, and the link is unconditional
// because the server already links the duct-tape.
extern "C" void dtape_test_ring_dump(void);
// perf#30 (doc section 250): the same ring the message queue records into, reached from the server side (the internal
// header is not on this translation unit's include path, and the symbol is plain C).
extern "C" void dtape_test_ring_record4(unsigned long long tag, unsigned long long a, unsigned long long b,
	unsigned long long c, unsigned long long d);
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <darlingserver/logging.hpp>
#include <darlingserver/metrics.hpp>
#include <darlingserver/debug-collect.hpp>
#include <darlingserver/debug-introspection.hpp>
#include <darlingserver/debug-connection.hpp>
#include <darlingserver/debug-serialize.hpp>

// perf#30 (doc section 174): file-scope so BOTH the servicing loop and the region registration can report the
// pass count -- the measurement that separates 'the loop stopped running passes' (the wake) from 'a pass ran
// and did not see the request' (the page).
static uint64_t g_processControlPasses = 0;
#ifdef DSERVER_RING_TRANSPORT
	#include <darlingserver/ring.hpp>
	#include <cstdlib>
	#include <time.h>
#endif

static DarlingServer::Server* sharedInstancePointer = nullptr;

struct DTapeHooks {
	static void dtape_hook_thread_suspend(void* thread_context, dtape_thread_continuation_callback_f continuationCallback, void* continuationContext, libsimple_lock_t* unlockMe) {
		if (auto thread = DarlingServer::Thread::currentThread()) {
			if (auto fakeThread = thread->impersonatingThread()) {
				if (thread_context == fakeThread.get()) {
					return dtape_hook_thread_suspend(thread.get(), continuationCallback, continuationContext, unlockMe);
				}
			}
		}
		if (continuationCallback) {
			static_cast<DarlingServer::Thread*>(thread_context)->suspend([=]() {
				continuationCallback(continuationContext);
			}, unlockMe);
		} else {
			static_cast<DarlingServer::Thread*>(thread_context)->suspend(nullptr, unlockMe);
		}
	};

	static void dtape_hook_thread_resume(void* thread_context) {
		static_cast<DarlingServer::Thread*>(thread_context)->resume();
	};

	static void dtape_hook_thread_clear_resume_permit(void* thread_context) {
		static_cast<DarlingServer::Thread*>(thread_context)->clearResumePermit();
	};

	static dtape_task_t* dtape_hook_current_task(void) {
		auto thread = DarlingServer::Thread::currentThread();
		if (!thread) {
			return NULL;
		}
		if (auto fakeThread = thread->impersonatingThread()) {
			thread = fakeThread;
		}
		auto process = thread->process();
		if (!process) {
			return NULL;
		}
		return process->_dtapeTask;
	};

	static dtape_thread_t* dtape_hook_current_thread(void) {
		auto thread = DarlingServer::Thread::currentThread();
		if (!thread) {
			return NULL;
		}
		if (auto fakeThread = thread->impersonatingThread()) {
			thread = fakeThread;
		}
		return thread->_dtapeThread;
	};

	static void dtape_hook_timer_arm(uint64_t deadline_ns, bool override) {
		auto& server = DarlingServer::Server::sharedInstance();

		if (deadline_ns == UINT64_MAX) {
			deadline_ns = 0;
		}

		struct itimerspec newSpec;
		memset(&newSpec.it_interval, 0, sizeof(newSpec.it_interval));

		std::unique_lock lock(server._timerLock);

		struct timespec armNow;
		clock_gettime(CLOCK_MONOTONIC, &armNow);
		uint64_t nowNs = (uint64_t)armNow.tv_sec * 1000000000ull + (uint64_t)armNow.tv_nsec;
		const bool currentStillPending = server._currentTimerDeadline != 0 && server._currentTimerDeadline > nowNs;
		if (!override && currentStillPending && deadline_ns >= server._currentTimerDeadline) {
			return;
		}

		// perf#30 (doc section 250, MEASURED 2026-10-01): AN OVERRIDE ARM MUST NOT POSTPONE AN EARLIER PENDING DEADLINE.
		// The rule above only guards non-override arms, and the timer queue's cancel/re-assign path arms with
		// override = true. Measured on a frozen server: a guest thread waited in `semaphore_timedwait` (30 s) with
		// `timer=1 tactive=1` in the stall dump, while the timerfd's remaining time fell linearly from ~450 ms and
		// then jumped back to ~500 ms every ~450 ms, and its `ticks` stayed 0 forever -- something re-armed it with
		// a fresh ~500 ms deadline faster than it could expire. The main loop wakes on that same 500 ms cadence
		// whenever the stall watchdog is enabled, which is exactly why that watchdog raised the failure rate it was
		// written to observe. A timerfd pushed forward forever never expires, so `dtape_timer_fired()` never runs,
		// `timer_queue_expire()` never runs, and EVERY pending deadline in the queue is starved: the 30 s semaphore
		// timeout never times out and its thread is never woken. Clamping a later override arm to the pending
		// deadline fixes the starvation; an earlier deadline still wins, and a real disarm (deadline_ns == 0, the
		// UINT64_MAX mapping above) is still honoured, so a cancelled timer costs at most one spurious fire that
		// runs an expire pass finding nothing due.
		if (override && currentStillPending && deadline_ns > server._currentTimerDeadline) {
			deadline_ns = server._currentTimerDeadline;
		}

		newSpec.it_value.tv_sec = deadline_ns / 1000000000ull;
		newSpec.it_value.tv_nsec = deadline_ns % 1000000000ull;

		server._currentTimerDeadline = deadline_ns;

		// perf#30 (doc section 250): record the ARM into the ring (visible without the perturbing line trace).
		dtape_test_ring_record4(7, deadline_ns, override ? 1 : 0, nowNs, 0);

		if (timerfd_settime(server._timerFD, TFD_TIMER_ABSTIME, &newSpec, NULL) < 0) {
			throw std::system_error(errno, std::generic_category(), "Failed to set timerfd expiration deadline");
		}
	};

	static void dtape_hook_log(dtape_log_level_t level, const char* message) {
		static const auto log = DarlingServer::Log("dtape");
		auto process = DarlingServer::Process::currentProcess();
		auto thread = DarlingServer::Thread::currentThread();
		pid_t pid = process ? process->id() : -1;
		pid_t nspid = process ? process->nsid() : -1;
		pid_t tid = thread ? thread->id() : -1;
		pid_t nstid = thread ? thread->nsid() : -1;
		switch (level) {
			case dtape_log_level_debug:
				log.debug() << pid << "(" << nspid << "):" << tid << "(" << nstid << "): " << message << log.endLog;
				break;
			case dtape_log_level_info:
				log.info() << pid << "(" << nspid << "):" << tid << "(" << nstid << "): " << message << log.endLog;
				break;
			case dtape_log_level_warning:
				log.warning() << pid << "(" << nspid << "):" << tid << "(" << nstid << "): " << message << log.endLog;
				break;
			case dtape_log_level_error:
			default:
				log.error() << pid << "(" << nspid << "):" << tid << "(" << nstid << "): " << message << log.endLog;
				break;
		}
	};

	static void dtape_hook_get_load_info(dtape_load_info_t* load_info) {
		load_info->task_count = DarlingServer::processRegistry().size();
		load_info->thread_count = DarlingServer::threadRegistry().size();
	};

	static void dtape_hook_thread_terminate(void* thread_context) {
		static_cast<DarlingServer::Thread*>(thread_context)->terminate();
	};

	static dtape_thread_t* dtape_hook_thread_create_kernel(void) {
		auto thread = std::make_shared<DarlingServer::Thread>(DarlingServer::Thread::KernelThreadConstructorTag());
		thread->registerWithProcess();
		DarlingServer::threadRegistry().registerEntry(thread, true);
		/* CREATOR RECORD, SERVER SIDE. MEASURED NEED: the guest-side counter for guest thread creations and the
		 * loader's own counter agree with each other, but neither says WHICH server-side path brought the thread
		 * into existence. This is the kernel-thread creator the guest's duct-tape path reaches; printing here makes
		 * its participation a measurement. Bounded, raw write, no logging locks. */
		{
			static unsigned g_srv_kthread_records = 0;
			unsigned n = __atomic_fetch_add(&g_srv_kthread_records, 1, __ATOMIC_RELAXED);
			if (n < 32) {
				char b[160];
				int l = snprintf(b, sizeof(b),
					"[srv-kthread-create #%u id=%d nsid=%d pid=%d]\n",
					n, (int)thread->id(), (int)thread->nsid(),
					(int)thread->process()->id());
				if (l > 0) { (void)!write(2, b, (size_t)l); }
			}
		}
		return thread->_dtapeThread;
	};

	static void dtape_hook_thread_setup(void* thread_context, dtape_thread_continuation_callback_f startupCallback, void* startupCallbackContext) {
		static_cast<DarlingServer::Thread*>(thread_context)->setupKernelThread([=]() {
			startupCallback(startupCallbackContext);
		});
	};

	static void dtape_hook_thread_set_pending_signal(void* thread_context, int pending_signal) {
		static_cast<DarlingServer::Thread*>(thread_context)->setPendingSignal(pending_signal);
	};

	static void dtape_hook_thread_set_pending_call_override(void* thread_context, bool pending_call_override) {
		static_cast<DarlingServer::Thread*>(thread_context)->setPendingCallOverride(pending_call_override);
	};

	static dtape_thread_t* dtape_hook_thread_lookup(int id, bool id_is_nsid, bool retain) {
		auto& registry = DarlingServer::threadRegistry();
		auto maybeThread = (id_is_nsid) ? registry.lookupEntryByNSID(id) : registry.lookupEntryByID(id);
		if (!maybeThread) {
			return nullptr;
		}
		auto thread = *maybeThread;
		if (retain) {
			dtape_thread_retain(thread->_dtapeThread);
		}
		return thread->_dtapeThread;
	};

	static dtape_thread_t* dtape_hook_thread_lookup_eternal(dtape_eternal_id_t eid, bool retain) {
		auto& registry = DarlingServer::threadRegistry();
		auto maybeThread = registry.lookupEntryByEternalID(eid);
		if (!maybeThread) {
			return nullptr;
		}
		auto thread = *maybeThread;
		if (retain) {
			dtape_thread_retain(thread->_dtapeThread);
		}
		return thread->_dtapeThread;
	};

	static dtape_thread_state_t dtape_hook_thread_get_state(void* thread_context) {
		return static_cast<dtape_thread_state_t>(static_cast<DarlingServer::Thread*>(thread_context)->getRunState());
	};

	static int dtape_hook_thread_send_signal(void* thread_context, int signal) {
		try {
			static_cast<DarlingServer::Thread*>(thread_context)->sendSignal(signal);
			return 0;
		} catch (std::system_error e) {
			return -e.code().value();
		}
	};

	static void dtape_hook_thread_context_dispose(void* thread_context) {
		static_cast<DarlingServer::Thread*>(thread_context)->_dispose();
	};

	static dtape_eternal_id_t dtape_hook_thread_eternal_id(void* thread_context) {
		if (!thread_context) {
			return DarlingServer::EternalIDInvalid;
		}
		return static_cast<DarlingServer::Thread*>(thread_context)->eternalID();
	};

	static void dtape_hook_current_thread_interrupt_disable(void) {
		DarlingServer::Thread::interruptDisable();
	};

	static void dtape_hook_current_thread_interrupt_enable(void) {
		DarlingServer::Thread::interruptEnable();
	};

	static void dtape_hook_current_thread_syscall_return(int result_code) {
		DarlingServer::Thread::syscallReturn(result_code);
	};

	static void dtape_hook_current_thread_set_bsd_retval(uint32_t retval) {
		DarlingServer::Thread::currentThread()->_bsdReturnValue = retval;
	};

	static bool dtape_hook_task_read_memory(void* task_context, uintptr_t remote_address, void* local_buffer, size_t length) {
		return static_cast<DarlingServer::Process*>(task_context)->readMemory(remote_address, local_buffer, length);
	};

	static bool dtape_hook_task_write_memory(void* task_context, uintptr_t remote_address, const void* local_buffer, size_t length) {
		return static_cast<DarlingServer::Process*>(task_context)->writeMemory(remote_address, local_buffer, length);
	};

	static dtape_task_t* dtape_hook_task_lookup(int id, bool id_is_nsid, bool retain) {
		auto& registry = DarlingServer::processRegistry();
		auto maybeProcess = (id_is_nsid) ? registry.lookupEntryByNSID(id) : registry.lookupEntryByID(id);
		if (!maybeProcess) {
			return nullptr;
		}
		auto process = *maybeProcess;
		if (retain) {
			dtape_task_retain(process->_dtapeTask);
		}
		return process->_dtapeTask;
	};

	static dtape_task_t* dtape_hook_task_lookup_eternal(dtape_eternal_id_t eid, bool retain) {
		auto& registry = DarlingServer::processRegistry();
		auto maybeProcess = registry.lookupEntryByEternalID(eid);
		if (!maybeProcess) {
			return nullptr;
		}
		auto process = *maybeProcess;
		if (retain) {
			dtape_task_retain(process->_dtapeTask);
		}
		return process->_dtapeTask;
	};

	static void dtape_hook_task_get_memory_info(void* task_context, dtape_memory_info_t* memory_info) {
		auto info = static_cast<DarlingServer::Process*>(task_context)->memoryInfo();
		memory_info->virtual_size = info.virtualSize;
		memory_info->resident_size = info.residentSize;
		memory_info->page_size = info.pageSize;
		memory_info->region_count = info.regionCount;
	};

	static bool dtape_hook_task_get_memory_region_info(void* task_context, uintptr_t address, dtape_memory_region_info_t* memory_region_info) {
		int protection;
		try {
			auto info = static_cast<DarlingServer::Process*>(task_context)->memoryRegionInfo(address);
			memory_region_info->start_address = info.startAddress;
			memory_region_info->page_count = info.pageCount;
			memory_region_info->map_offset = info.mapOffset;
			protection = info.protection;
			memory_region_info->shared = info.shared;
		} catch (...) {
			return false;
		}
		memory_region_info->protection = dtape_memory_protection_none;
		if (protection & PROT_READ) {
			// for some reason, we can't just do `|=`;
			// the compiler complains about "can't assign `int` to `dtape_memory_protection`" or something like that
			memory_region_info->protection = (dtape_memory_protection_t)(memory_region_info->protection | dtape_memory_protection_read);
		}
		if (protection & PROT_WRITE) {
			memory_region_info->protection = (dtape_memory_protection_t)(memory_region_info->protection | dtape_memory_protection_write);
		}
		if (protection & PROT_EXEC) {
			memory_region_info->protection = (dtape_memory_protection_t)(memory_region_info->protection | dtape_memory_protection_execute);
		}
		return true;
	};

	static uintptr_t dtape_hook_task_allocate_pages(void* task_context, size_t page_count, int protection, uintptr_t address_hint, dtape_memory_flags_t flags) {
		try {
			return static_cast<DarlingServer::Process*>(task_context)->allocatePages(page_count, protection, address_hint, flags & dtape_memory_flag_fixed, flags & dtape_memory_flag_overwrite);
		} catch (std::system_error e) {
			return 0;
		}
	};

	static int dtape_hook_task_free_pages(void* task_context, uintptr_t address, size_t page_count) {
		try {
			static_cast<DarlingServer::Process*>(task_context)->freePages(address, page_count);
			return 0;
		} catch (std::system_error e) {
			return -1;
		}
	};

	static uintptr_t dtape_hook_task_map_file(void* task_context, int fd, size_t page_count, int protection, uintptr_t address_hint, size_t page_offset, dtape_memory_flags_t flags) {
		try {
			return static_cast<DarlingServer::Process*>(task_context)->mapFile(fd, page_count, protection, address_hint, page_offset, flags & dtape_memory_flag_fixed, flags & dtape_memory_flag_overwrite);
		} catch (std::system_error e) {
			return 0;
		}
	};

	static uintptr_t dtape_hook_task_get_next_region(void* task_context, uintptr_t address) {
		return static_cast<DarlingServer::Process*>(task_context)->getNextRegion(address);
	};

	static bool dtape_hook_task_change_protection(void* task_context, uintptr_t address, size_t page_count, int protection) {
		try {
			static_cast<DarlingServer::Process*>(task_context)->changeProtection(address, page_count, protection);
			return true;
		} catch (std::system_error e) {
			return false;
		}
	};

	static bool dtape_hook_task_sync_memory(void* task_context, uintptr_t address, size_t size, int sync_flags) {
		try {
			static_cast<DarlingServer::Process*>(task_context)->syncMemory(address, size, sync_flags);
			return true;
		} catch (std::system_error e) {
			return false;
		}
	};

	static void dtape_hook_task_context_dispose(void* task_context) {
		static_cast<DarlingServer::Process*>(task_context)->_dispose();
	};

	static dtape_eternal_id_t dtape_hook_task_eternal_id(void* task_context) {
		if (!task_context) {
			return DarlingServer::EternalIDInvalid;
		}
		return static_cast<DarlingServer::Process*>(task_context)->eternalID();
	};

#if DSERVER_EXTENDED_DEBUG
	static void dtape_hook_task_register_name(void* task_context, uint32_t name, uintptr_t pointer) {
		static_cast<DarlingServer::Process*>(task_context)->_registerName(name, pointer);
	};

	static void dtape_hook_task_unregister_name(void* task_context, uint32_t name) {
		static_cast<DarlingServer::Process*>(task_context)->_unregisterName(name);
	};

	static void dtape_hook_task_add_port_set_member(void* task_context, dtape_port_set_id_t port_set, dtape_port_id_t member) {
		static_cast<DarlingServer::Process*>(task_context)->_addPortSetMember(port_set, member);
	};

	static void dtape_hook_task_remove_port_set_member(void* task_context, dtape_port_set_id_t port_set, dtape_port_id_t member) {
		static_cast<DarlingServer::Process*>(task_context)->_removePortSetMember(port_set, member);
	};

	static void dtape_hook_task_clear_port_set(void* task_context, dtape_port_set_id_t port_set) {
		static_cast<DarlingServer::Process*>(task_context)->_clearPortSet(port_set);
	};
#endif

	static constexpr dtape_hooks_t dtape_hooks = {
		.current_task = dtape_hook_current_task,
		.current_thread = dtape_hook_current_thread,

		.timer_arm = dtape_hook_timer_arm,

		.log = dtape_hook_log,
		.get_load_info = dtape_hook_get_load_info,

		.thread_suspend = dtape_hook_thread_suspend,
		.thread_resume = dtape_hook_thread_resume,
		.thread_clear_resume_permit = dtape_hook_thread_clear_resume_permit,
		.thread_terminate = dtape_hook_thread_terminate,
		.thread_create_kernel = dtape_hook_thread_create_kernel,
		.thread_setup = dtape_hook_thread_setup,
		.thread_set_pending_signal = dtape_hook_thread_set_pending_signal,
		.thread_set_pending_call_override = dtape_hook_thread_set_pending_call_override,
		.thread_lookup = dtape_hook_thread_lookup,
		.thread_lookup_eternal = dtape_hook_thread_lookup_eternal,
		.thread_get_state = dtape_hook_thread_get_state,
		.thread_send_signal = dtape_hook_thread_send_signal,
		.thread_context_dispose = dtape_hook_thread_context_dispose,
		.thread_eternal_id = dtape_hook_thread_eternal_id,

		.current_thread_interrupt_disable = dtape_hook_current_thread_interrupt_disable,
		.current_thread_interrupt_enable = dtape_hook_current_thread_interrupt_enable,
		.current_thread_syscall_return = dtape_hook_current_thread_syscall_return,
		.current_thread_set_bsd_retval = dtape_hook_current_thread_set_bsd_retval,

		.task_read_memory = dtape_hook_task_read_memory,
		.task_write_memory = dtape_hook_task_write_memory,
		.task_lookup = dtape_hook_task_lookup,
		.task_lookup_eternal = dtape_hook_task_lookup_eternal,
		.task_get_memory_info = dtape_hook_task_get_memory_info,
		.task_get_memory_region_info = dtape_hook_task_get_memory_region_info,
		.task_allocate_pages = dtape_hook_task_allocate_pages,
		.task_free_pages = dtape_hook_task_free_pages,
		.task_map_file = dtape_hook_task_map_file,
		.task_get_next_region = dtape_hook_task_get_next_region,
		.task_change_protection = dtape_hook_task_change_protection,
		.task_sync_memory = dtape_hook_task_sync_memory,
		.task_context_dispose = dtape_hook_task_context_dispose,
		.task_eternal_id = dtape_hook_task_eternal_id,

#if DSERVER_EXTENDED_DEBUG
		.task_register_name = dtape_hook_task_register_name,
		.task_unregister_name = dtape_hook_task_unregister_name,
		.task_add_port_set_member = dtape_hook_task_add_port_set_member,
		.task_remove_port_set_member = dtape_hook_task_remove_port_set_member,
		.task_clear_port_set = dtape_hook_task_clear_port_set,
#endif
	};
};

DarlingServer::Server::Server(
	std::string prefix,
	int prefixFD,
	pid_t rootlessInitHostPID
):
	_prefix(prefix),
	_prefixFD(prefixFD),
	_rootlessInitHostPID(rootlessInitHostPID),
	_socketPath(_prefix + "/.darlingserver.sock"),
	// abstract-namespace name for the stat socket (see the stat-socket setup below for
	// why abstract and not a pathname). Resolve rootless /proc/self/fd capabilities
	// to the same canonical prefix identity used by host-side clients.
	_statSocketPath("darlingserver-stat:" + std::filesystem::canonical(_prefix).string()),
	_workQueue(std::bind(&Server::_worker, this, std::placeholders::_1))
{
	sharedInstancePointer = this;

	// perf #0 (dar-dar6x4-perf-5dq.6): record the server start time for uptime.
	Metrics::shared().startMonoUs = Metrics::nowMonoUs();

	// perf #18 P8 D8 (dar-1il.3.2.x): arm the mach_msg_overwrite SHAPE CENSUS if requested. OFF by
	// default; a pure measurement (no behavior change) that classifies each msg_overwrite by
	// send/receive + body descriptor shape so we can size the reclaimable fraction of its ~19%
	// hotness before designing any ring migration. Set DARLING_SERVER_MSG_CENSUS=1 at server start.
	if (const char* env = getenv("DARLING_SERVER_MSG_CENSUS")) {
		if (env[0] == '1') {
			Metrics::shared().msgCensusOn.store(true, std::memory_order_relaxed);
			static DarlingServer::Log censusLog("census");
			censusLog.error() << "[NOTICE] perf#18 D8 mach_msg_overwrite shape census ARMED"
				<< " (DARLING_SERVER_MSG_CENSUS=1). Pure measurement, no behavior change."
				<< " Read msg_* counters via the stat socket; msg_send_only_simple / msg_total"
				<< " sizes the reclaimable share." << censusLog.endLog;
		}
	}

	// perf #18 D9 (dar-1il.4): arm the global RPC HEATMAP + lane-eligibility census if requested. OFF by
	// default; a pure measurement (no behavior change) that records, per call number, the transport split
	// (uds vs ring), per-transport latency, and the runtime facts that decide lane eligibility
	// (used_fiber, caller_s2c). The point: stop GUESSING the next op to ring-migrate (the D8 lesson --
	// the hottest op was barely reclaimable) and find it by DATA. Read rpc_heatmap via the stat socket;
	// rank by count x (uds_p50 - ring_p50) x eligibility. Set DARLING_SERVER_RPC_HEATMAP=1 at server start.
	if (const char* env = getenv("DARLING_SERVER_RPC_HEATMAP")) {
		if (env[0] == '1') {
			Metrics::shared().heatmapOn.store(true, std::memory_order_relaxed);
			static DarlingServer::Log heatmapLog("heatmap");
			heatmapLog.error() << "[NOTICE] perf#18 D9 global RPC heatmap + lane-eligibility census ARMED"
				<< " (DARLING_SERVER_RPC_HEATMAP=1). Pure measurement, no behavior change."
				<< " Read rpc_heatmap via the stat socket; per-callnum transport split + lane verdict."
				<< heatmapLog.endLog;
		}
	}

	// perf #18 D15a (dar-1il.10): arm the ring-ATTACH TIMELINE / reclaimability census if requested.
	// OFF by default; a pure measurement (no behavior change) that records the per-process pre-attach
	// UDS window -- which eligible ops run over UDS before the guest lazily attaches its ring, the
	// ordinal at which attach happens, and attach attempt/reject tallies. D14 found the reclaimable UDS
	// tail is pre-attach-dominated; this sizes it and informs whether to move attach earlier (and how).
	// Set DARLING_SERVER_ATTACH_CENSUS=1 at server start. Read attach_census_* via the stat socket.
	if (const char* env = getenv("DARLING_SERVER_ATTACH_CENSUS")) {
		if (env[0] == '1') {
			Metrics::shared().attachCensusOn.store(true, std::memory_order_relaxed);
			static DarlingServer::Log attachLog("attachcensus");
			attachLog.error() << "[NOTICE] perf#18 D15a ring-attach timeline census ARMED"
				<< " (DARLING_SERVER_ATTACH_CENSUS=1). Pure measurement, no behavior change."
				<< " Read attach_census_* via the stat socket; pre-attach eligible-UDS by callnum."
				<< attachLog.endLog;
		}
	}

	// perf #18 D17 (dar-1il.12): POST-D16 residual-UDS classifier arming. RECON-ONLY, default-OFF;
	// classifies each residual UDS call by WHY it is on UDS (first-before-this-thread's-lane vs
	// despite-a-live-lane) so we can decide if the post-D16 231-call residual is unavoidable (A) or a
	// wrapper/coverage gap (B/D). Read residual_* via the stat socket. Pure measurement, no behavior change.
	if (const char* env = getenv("DARLING_SERVER_RESIDUAL_CENSUS")) {
		if (env[0] == '1') {
			Metrics::shared().residualCensusOn.store(true, std::memory_order_relaxed);
			static DarlingServer::Log residualLog("residualcensus");
			residualLog.error() << "[NOTICE] perf#18 D17 post-D16 residual-UDS classifier ARMED"
				<< " (DARLING_SERVER_RESIDUAL_CENSUS=1). Pure measurement, no behavior change."
				<< " Read residual_* via the stat socket; reason buckets + uds-despite-lane by callnum."
				<< residualLog.endLog;
		}
	}

	// Remove the old socket relative to the already-retained prefix. The path
	// spelling is used only by bind(2), which has no *at variant.
	if (unlinkat(prefixFD, ".darlingserver.sock", 0) == -1 &&
		errno != ENOENT) {
		throw std::system_error(
			errno, std::generic_category(),
			"Failed to remove stale server socket");
	}

	// create the socket
	_listenerSocket = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (_listenerSocket < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to create socket");
	}

	int passCred = 1;
	if (setsockopt(_listenerSocket, SOL_SOCKET, SO_PASSCRED, &passCred, sizeof(passCred)) < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to set SO_PASSCRED on socket");
	}

	struct sockaddr_un addr;
	addr.sun_family = AF_UNIX;
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
	strncpy(addr.sun_path, _socketPath.c_str(), sizeof(addr.sun_path) - 1);

	if (bind(_listenerSocket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to bind socket");
	}

	_wakeupFD = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (_wakeupFD < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to create eventfd for on-demand epoll wakeups");
	}

	_epollFD = epoll_create1(EPOLL_CLOEXEC);
	if (_epollFD < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to create epoll context");
	}

	struct epoll_event settings;
	settings.data.ptr = this;
	settings.events = EPOLLIN | EPOLLOUT | EPOLLET;

	if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, _listenerSocket, &settings) < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to add listener socket to epoll context");
	}

	settings.data.ptr = &_wakeupFD;
	settings.events = EPOLLIN | EPOLLONESHOT;

	if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, _wakeupFD, &settings) < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to add eventfd to epoll context");
	}

	_outbox.setMessageArrivalNotificationCallback([this]() {
		// we don't really have to worry about the eventfd overflowing;
		// if it does, that means the main loop has been waiting a LONG time for the listener socket to become writable again.
		// in that case, we don't really care if the eventfd is being incremented; we can't send anything anyways.
		// once the socket becomes writable again, the eventfd will be monitored again.
		eventfd_write(_wakeupFD, 1);
	});

	_timerFD = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (_timerFD < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to create timer descriptor");
	}

	settings.data.ptr = &_timerFD;
	settings.events = EPOLLIN;

	if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, _timerFD, &settings) < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to add timer descriptor to epoll context");
	}

	// perf #0 (dar-dar6x4-perf-5dq.6): set up the stat socket. Best-effort: any failure
	// here logs and leaves _statListenerSocket == -1; the server runs normally without it.
	//
	// IMPORTANT: the darlingserver runs inside a private MOUNT namespace (the launcher
	// joins the darling-init mnt namespace), so a pathname socket under the prefix is
	// NOT reachable from the host -- only from inside the namespace. The NETWORK namespace
	// is shared with the host, though, so we bind in the ABSTRACT namespace (leading NUL):
	// abstract names live in the net namespace, not the filesystem, so host-side tooling
	// (darling-stat / darling-progress-watch) can connect. The abstract name is keyed off
	// the prefix so multiple prefixes don't collide. _statSocketPath holds that name (the
	// part after the NUL) for logging.
	{
		static DarlingServer::Log metricsLog("metrics");
		_statListenerSocket = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (_statListenerSocket < 0) {
			metricsLog.warning() << "Failed to create stat socket: " << strerror(errno) << "; metrics disabled" << metricsLog.endLog;
		} else {
			struct sockaddr_un statAddr;
			memset(&statAddr, 0, sizeof(statAddr));
			statAddr.sun_family = AF_UNIX;
			// abstract name: sun_path[0] == '\0', then the name bytes. Address length is
			// offsetof(sun_path) + 1 (the NUL) + strlen(name), and the name is NOT NUL-terminated.
			size_t nameLen = _statSocketPath.size();
			if (nameLen > sizeof(statAddr.sun_path) - 1) {
				nameLen = sizeof(statAddr.sun_path) - 1;
			}
			statAddr.sun_path[0] = '\0';
			memcpy(statAddr.sun_path + 1, _statSocketPath.data(), nameLen);
			socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + 1 + nameLen;
			if (bind(_statListenerSocket, (struct sockaddr*)&statAddr, addrLen) != 0 ||
			    listen(_statListenerSocket, 16) != 0) {
				metricsLog.warning() << "Failed to bind/listen stat socket: " << strerror(errno) << "; metrics disabled" << metricsLog.endLog;
				close(_statListenerSocket);
	if (_fdCourierListenerSocket >= 0) {
		close(_fdCourierListenerSocket);
	}
				_statListenerSocket = -1;
			} else {
				settings.data.ptr = &_statListenerSocket;
				settings.events = EPOLLIN;
				if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, _statListenerSocket, &settings) < 0) {
					metricsLog.warning() << "Failed to add stat socket to epoll: " << strerror(errno) << "; metrics disabled" << metricsLog.endLog;
					close(_statListenerSocket);
					_statListenerSocket = -1;
				} else {
					metricsLog.info() << "Stat socket listening at abstract:" << _statSocketPath << metricsLog.endLog;
				}

				// perf#30 FD-COURIER: the process-scoped descriptor channel (PRODUCT). Same abstract-namespace
				// scheme as the stat socket (the server lives in a private mount namespace, so a pathname
				// socket would be unreachable), bound to its own name so the two never collide.
				_fdCourierListenerSocket = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
				if (_fdCourierListenerSocket >= 0) {
					// Same derivation rule the guest uses: the stat name with its service prefix
					// replaced, so both sides compute the identical abstract name from the prefix.
					_fdCourierPath = _statSocketPath;
					{
						const std::string statPrefix = "darlingserver-stat:";
						if (_fdCourierPath.compare(0, statPrefix.size(), statPrefix) == 0) {
							_fdCourierPath = "darlingserver-fdcourier:" + _fdCourierPath.substr(statPrefix.size());
						}
					}
					struct sockaddr_un courierAddr;
					memset(&courierAddr, 0, sizeof(courierAddr));
					courierAddr.sun_family = AF_UNIX;
					size_t cnameLen = _fdCourierPath.size();
					if (cnameLen > sizeof(courierAddr.sun_path) - 1) {
						close(_fdCourierListenerSocket);
						_fdCourierListenerSocket = -1;
					} else {
						memcpy(&courierAddr.sun_path[0] + 1, _fdCourierPath.data(), cnameLen);
						socklen_t caddrLen = offsetof(struct sockaddr_un, sun_path) + 1 + cnameLen;
						if (bind(_fdCourierListenerSocket, (struct sockaddr*)&courierAddr, caddrLen) != 0 ||
						    listen(_fdCourierListenerSocket, 16) != 0) {
							close(_fdCourierListenerSocket);
							_fdCourierListenerSocket = -1;
						} else {
							struct epoll_event csettings;
							memset(&csettings, 0, sizeof(csettings));
							csettings.events = EPOLLIN;
							csettings.data.ptr = &_fdCourierListenerSocket;
							if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, _fdCourierListenerSocket, &csettings) < 0) {
								close(_fdCourierListenerSocket);
								_fdCourierListenerSocket = -1;
							} else {
								metricsLog.info() << "perf#30 fd courier listening at abstract:"
								                  << _fdCourierPath << metricsLog.endLog;
							}
						}
					}
				}
			}
		}
	}
	// Darling native debugger epic: read-only debug introspection listener, OPT-IN only.
	// Guards are structural (bounded request and response, read-only accessors, no fd transfer,
	// no guest dependency, peer-uid authentication, no fallback transport); see docs/debug-abi.md.
	{
		static DarlingServer::Log debugLog("debug-abi");
		const bool debugAbiEnabled = Debug::debugAbiEnabledFor(getenv("DSERVER_DEBUG_ABI"));
		std::string identityFailure;
		if (!debugAbiEnabled) {
			debugLog.info() << "Debug ABI not enabled (set DSERVER_DEBUG_ABI=1 to serve it); no listener created" << debugLog.endLog;
		} else if (!Debug::socketNameForPrefix(_prefix, _debugSocketPath, identityFailure)) {
			debugLog.warning() << identityFailure << "; debug ABI disabled" << debugLog.endLog;
		} else {
			_debugListenerSocket = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
			if (_debugListenerSocket < 0) {
				debugLog.warning() << "Failed to create debug socket: " << strerror(errno) << "; debug ABI disabled" << debugLog.endLog;
			} else {
				struct sockaddr_un debugAddr;
				memset(&debugAddr, 0, sizeof(debugAddr));
				debugAddr.sun_family = AF_UNIX;
				const size_t debugNameLen = _debugSocketPath.size();
				debugAddr.sun_path[0] = '\0';
				memcpy(debugAddr.sun_path + 1, _debugSocketPath.data(), debugNameLen);
				socklen_t debugAddrLen = offsetof(struct sockaddr_un, sun_path) + 1 + debugNameLen;
				if (bind(_debugListenerSocket, (struct sockaddr*)&debugAddr, debugAddrLen) != 0 ||
				    listen(_debugListenerSocket, 16) != 0) {
					debugLog.warning() << "Failed to bind/listen debug socket: " << strerror(errno) << "; debug ABI disabled" << debugLog.endLog;
					close(_debugListenerSocket);
					_debugListenerSocket = -1;
				} else {
					struct epoll_event debugSettings;
					debugSettings.data.ptr = &_debugListenerSocket;
					debugSettings.events = EPOLLIN;
					if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, _debugListenerSocket, &debugSettings) < 0) {
						debugLog.warning() << "Failed to add debug socket to epoll: " << strerror(errno) << "; debug ABI disabled" << debugLog.endLog;
						close(_debugListenerSocket);
						_debugListenerSocket = -1;
					} else {
						_debugCollector = std::make_unique<DebugCollector>();
						_debugConnections = std::make_unique<Debug::DebugConnections>(_epollFD, *_debugCollector);
						debugLog.info() << "Debug ABI listening at abstract:" << _debugSocketPath << debugLog.endLog;
					}
				}
			}
		}
	}
};

DarlingServer::Server::~Server() {
	// Clients must leave epoll before its descriptor is closed.
	_debugConnections.reset();
	_debugCollector.reset();
	close(_epollFD);
	close(_wakeupFD);
	close(_listenerSocket);
	if (unlinkat(_prefixFD, ".darlingserver.sock", 0) == -1 &&
		errno != ENOENT) {
		fprintf(stderr, "Failed to remove server socket: %s\n",
			strerror(errno));
	}
	if (_statListenerSocket >= 0) {
		// abstract socket: no filesystem entry to unlink; closing frees the name.
		close(_statListenerSocket);
	}
	if (_debugListenerSocket >= 0) {
		// abstract socket: closing frees the name.
		close(_debugListenerSocket);
	}
};

// perf#30 FD-COURIER (PRODUCT, dar-gwn.7.7.5). Envelope (no callnum, no RPC body):
//   { uint64 process_generation; uint64 token; uint32 kind; uint32 fd_count; }
// followed by SCM_RIGHTS carrying exactly fd_count descriptors. The semantic half of the operation
// travels on the Ring/shared path and names the token; this channel carries ONLY the descriptor.
//
// The process identity is the kernel's own statement -- SO_PEERCRED on the accepted connection -- not
// something the peer asserts, so a token cannot be replayed onto another process's request. The
// envelope's generation is the peer's incarnation counter for that pid: the first bundle from a pid
// establishes it and any bundle from a different incarnation is rejected rather than paired.
//
// The connection is persistent: ONE per Linux process, established at bootstrap and reused for every
// transfer, so connection count tracks processes, not operations.
bool DarlingServer::Server::_isFdCourierSocket(void* ptr) {
	for (auto& [sock, conn] : _fdCourierConns) {
		if (&conn.socket == ptr) {
			return true;
		}
	}
	return false;
}

void DarlingServer::Server::_handleFdCourierConnection() {
	while (true) {
		int client = accept4(_fdCourierListenerSocket, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client < 0) {
			break;
		}
		struct ucred cred {};
		socklen_t credLen = sizeof(cred);
		if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0) {
			close(client);
			continue;
		}
		FdCourierConnection conn;
		conn.socket = client;
		conn.peerPid = cred.pid;
		_fdCourierConns[client] = conn;
		{
			// perf#30 PHASE-0: watch the CONNECTION, not only the listener. MEASURED: with only the
			// listener registered, a one-byte wake on an established connection left the server in epoll,
			// no pass ran, and a management-page request issued before the doorbell existed was never
			// serviced.
			struct epoll_event cEvent {};
			cEvent.events = EPOLLIN;
			cEvent.data.ptr = &_fdCourierConns[client].socket;
			if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, client, &cEvent) < 0) {
				static DarlingServer::Log courierLog("fd-courier");
				courierLog.warning() << "could not watch connection pid=" << cred.pid << courierLog.endLog;
			}
		}
		Metrics::shared().fdCourierConnections.fetch_add(1, std::memory_order_relaxed);
		Metrics::shared().fdCourierLiveConnections.fetch_add(1, std::memory_order_relaxed);
	}
};

// Read every complete envelope waiting on every courier connection. Non-blocking by construction: it is
// called from the event loop and again immediately before any token is resolved, so a descriptor that
// the guest sent BEFORE publishing its request is always already visible when that request is handled.
// perf#30 FD-COURIER (round 49, SERVER->GUEST): the reverse direction of the same transport. The
// descriptor travels on the process-scoped SOCK_SEQPACKET connection the process already owns, with the
// same envelope shape as the other direction -- generation, token, kind, fd_count and SCM_RIGHTS, and
// nothing else: no semantic body, no callnum, no result code ever crosses this socket. The reply that
// carries the token is a separate, semantic message on the lane or the datagram path.
//
// The send is attempted FIRST, before the reply is published, so a guest that has the reply always
// finds the descriptor already queued (the same ordering rule the request direction uses). Ownership of
// the descriptor transfers: the server closes its copy after a successful send, exactly as it would
// after pushDescriptor.
uint64_t DarlingServer::Server::sendFdCourierBundleToGuest(pid_t pid, uint32_t kind, int fd, bool preferLoader, bool drainFirst) {
	if (fd < 0) {
		return 0;
	}
	// Drain FIRST: the generation this bundle must carry is learned from what the process itself sent, and
	// MEASURED, the request-direction bundle can still be sitting in the backlog when the reply is built
	// (the reply carried gen=0, the guest refused the descriptor as stale, and the attach failed). Reading
	// the connection before answering is what makes the two directions agree on one generation.
	// Draining first is the DEFAULT because the bundle's generation is learned from what the process itself sent -- but
	// a caller that is ALREADY inside the courier receive callback must not re-enter it (measured: re-entrant drain
	// lost a bundle silently and the boot stopped after 6 plane publishes), so that caller passes drainFirst=false.
	if (drainFirst) {
		_drainFdCourierMessages();
	}
	// perf#30 (doc section 90): address the IMAGE, not just the pid. A process has several courier connections
	// (the loader's and the guest image's); picking the first with a matching pid delivered descriptors to the
	// wrong image, which is what the guest's `[fd-courier-recv] MISS ... stored=0 dropped=0` was. The
	// connection that delivered KIND_PROCESS_CONTROL is the loader's; prefer the OTHER one, which is the image
	// that runs the ordinary calls.
	int socket = -1;
	int fallbackSocket = -1;
	int loaderFallbackSocket = -1;
	for (auto& [sock, conn] : _fdCourierConns) {
		// THE LOADER IS FOUND BY ITS FLAG, NOT BY ITS PID. MEASURED root cause of the whole ordering failure: with the
		// pid filter applied first, this function returned `socket=-1 ino=0 isLoader=0` for every process, the send
		// returned 0, and NOTHING was ever delivered -- while the guest waited three seconds and recorded no drops.
		// The loader's connection is identified by delivering the control region (conn.isLoader = true is set exactly
		// there), and its peer-pid record cannot be relied on.
		// THIS process's loader connection first; "some loader connection" only as a fallback. MEASURED: selecting by the
		// flag alone routed the doorbell for MANY pids onto ONE connection (ino=221342049 for every one of them), so it
		// reached a loader incarnation that was not the one waiting -- the flag says "a loader connection", not "this
		// process's loader connection".
		if (preferLoader && conn.isLoader && conn.peerPid == pid) {
			socket = sock;
			break;
		}
		if (preferLoader && conn.isLoader && loaderFallbackSocket < 0) {
			loaderFallbackSocket = sock;
		}
		if (conn.peerPid != pid) {
			continue;
		}
		if (fallbackSocket < 0) { fallbackSocket = sock; }
		// perf#30 (violation A, MEASURED): the default rule addresses the GUEST IMAGE, because that is who runs
		// ordinary calls. A caller that must reach the LOADER asks for it explicitly: the process doorbell has to be
		// in the loader's hands before the loader's first plane publish (the PING), and the channel census measured
		// that it never was -- 36 of 38 publishes reported `via=none`, so the server's bounded poll was load-bearing.
		if (preferLoader ? conn.isLoader : !conn.isLoader) { socket = sock; break; }
	}
	if (socket < 0) { socket = (loaderFallbackSocket >= 0 && preferLoader) ? loaderFallbackSocket : fallbackSocket; }
	if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
		static DarlingServer::Log bLog("courier-send-image");
		bool loaderTag = false;
		if (auto it = _fdCourierConns.find(socket); it != _fdCourierConns.end()) { loaderTag = it->second.isLoader; }
		{
			auto genIt = _fdCourierGeneration.find(pid);
			struct stat connSt;
			unsigned long long connIno = (fstat(socket, &connSt) == 0) ? (unsigned long long)connSt.st_ino : 0ull;
			bLog.error() << "bundle-to pid=" << pid << " kind=" << kind << " socket=" << socket << " ino=" << connIno
				<< " isLoader=" << (loaderTag ? 1 : 0) << " conns=" << _fdCourierConns.size()
				<< " gen=" << ((genIt != _fdCourierGeneration.end()) ? genIt->second : 0)
				<< " genKnown=" << ((genIt != _fdCourierGeneration.end()) ? 1 : 0) << bLog.endLog;
		}
	}
	if (kind == DSERVER_FD_COURIER_KIND_PROCESS_DOORBELL) {
		// UNCONDITIONAL, because the richer `bundle-to` line is env-hatched and the server's env cannot be driven from
		// the caller. This is the fact the ordering analysis needs: which connection the doorbell went to, named by its
		// SOCKET INODE, which the guest also prints for its own connections -- an fd number is not comparable across
		// processes, an inode is.
		struct stat routeSt;
		unsigned long long routeIno = 0ull;
		bool routeIsLoader = false;
		if (socket >= 0 && fstat(socket, &routeSt) == 0) {
			routeIno = (unsigned long long)routeSt.st_ino;
		}
		if (auto it = _fdCourierConns.find(socket); it != _fdCourierConns.end()) {
			routeIsLoader = it->second.isLoader;
		}
		// The connection MAP itself, because "many pids routed to one connection" can only be explained by what the
		// server actually holds: if the loader connection's peer-pid was never learned, selecting "this process's
		// loader connection" cannot succeed and the fallback silently serves everyone.
		std::string connMap;
		for (auto& [mapSock, mapConn] : _fdCourierConns) {
			connMap += std::to_string(mapSock) + ":" + std::to_string(mapConn.peerPid) + ":" + (mapConn.isLoader ? "L" : "-") + " ";
		}
		/* BOUNDED ON PURPOSE (dar-4cp9). MEASURED: this line is written on every doorbell route, unconditionally and at
		 * error level, and it produced a sixty-megabyte log whose tail was nothing but this one sentence repeated for
		 * the same handful of pids -- which is where the record of the server's own process spawn for a hung boot went
		 * to hide. Diagnostics that cannot be read are not diagnostics: the first forty routes are printed in full,
		 * then one line says the sampling stopped, so the file stays evidence. */
		static unsigned long long routeLogPrinted = 0ull;
		static DarlingServer::Log routeLog("plane-doorbell-route");
		if (routeLogPrinted < 40ull) {
			++routeLogPrinted;
			routeLog.error() << "plane-doorbell-route pid=" << pid << " socket=" << socket << " ino=" << routeIno
				<< " isLoader=" << (routeIsLoader ? 1 : 0) << " conns=[" << connMap << "]" << routeLog.endLog;
			if (routeLogPrinted == 40ull) {
				routeLog.error() << "plane-doorbell-route SAMPLING-STOPPED after 40 routes; further routes are not logged"
					<< routeLog.endLog;
			}
		}
	}
	if (socket < 0) {
		// No connection for this process: the caller keeps its legacy path. Counted, because a silent
		// zero here is indistinguishable from a working courier in the metrics.
		Metrics::shared().fdCourierFallbackCmsg.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	auto gen = _fdCourierGeneration.find(pid);
	uint64_t generation = (gen != _fdCourierGeneration.end()) ? gen->second : 0;
	if (generation == 0) {
		// MEASURED: the LOADER's own seed attaches before any courier traffic exists for that incarnation,
		// so the server does not know its generation yet, and a bundle stamped 0 is REFUSED by the guest's
		// staleness check (gen=0 != its own) -- which closed the doorbell and stopped the boot. A bundle the
		// receiver will reject is worse than the legacy path, so this falls back to the reply CMSG and says
		// so in the counters. The courier carries the doorbell for every incarnation whose generation the
		// server already knows.
		Metrics::shared().fdCourierFallbackCmsg.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	static std::atomic<uint64_t> nextToken {1};
	// The pid is mixed in as well as the generation: MEASURED, generation 0 (an incarnation whose bundle
	// has not been seen) made the first token exactly 1, which is both trivially guessable and, worse, the
	// value a zero-generation envelope produces for ANY pid. A token names one descriptor for one process,
	// so it must not be reproducible from a constant.
	uint64_t token = ((uint64_t)pid * 0x9E3779B97F4A7C15ull) ^ (generation * 0xC2B2AE3D27D4EB4Full)
		^ (nextToken.fetch_add(1, std::memory_order_relaxed) * 0x165667B19E3779F9ull);
	// A zero token would mean "no descriptor half" to the guest, so it is never issued.
	if (token == 0) {
		token = nextToken.fetch_add(1, std::memory_order_relaxed) | 1ull;
	}

	dserver_fd_courier_message envelope {};
	envelope.process_generation = generation;
	envelope.token = token;
	envelope.kind = kind;
	envelope.fd_count = 1;

	char control[CMSG_SPACE(sizeof(int))];
	memset(control, 0, sizeof(control));
	struct iovec iov { &envelope, sizeof(envelope) };
	struct msghdr msg {};
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

	// perf#30 FD-COURIER MUTATIONS (round 49, SERVER->GUEST): each mode produces ONE of the failure
	// shapes the guest must reject, so its reject paths are exercised by a real descriptor instead of
	// being asserted in prose. Single-mode, env-gated; an unset hatch is the product path byte for byte.
	const char* mutation = getenv("DARLING_SERVER_COURIER_MUTATE");
	if (mutation != NULL && mutation[0] == 'd') {
		// duplicate: the same token twice; the guest keeps the first and closes the second.
		ssize_t a = sendmsg(socket, &msg, MSG_NOSIGNAL);
		ssize_t b = sendmsg(socket, &msg, MSG_NOSIGNAL);
		if (a != (ssize_t)sizeof(envelope) || b != (ssize_t)sizeof(envelope)) {
			Metrics::shared().fdCourierSendFail.fetch_add(1, std::memory_order_relaxed);
			return 0;
		}
		Metrics::shared().fdCourierSentToGuest.fetch_add(2, std::memory_order_relaxed);
		close(fd);
		Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
		return token;
	} else if (mutation != NULL && mutation[0] == 'k') {
		// wrong kind: a descriptor for an operation this build does not have.
		envelope.kind = 0xFFFFu;
	} else if (mutation != NULL && mutation[0] == 's') {
		// stale generation: a bundle from another incarnation, which the guest must refuse.
		envelope.process_generation = generation + 1u;
	} else if (mutation != NULL && mutation[0] == 'o') {
		// orphan: a token no reply will ever name. It must be closed, not leaked, when the process dies.
		envelope.token = token ^ 0x2000000000000000ull;
	} else if (mutation != NULL && mutation[0] == 'n') {
		// semantic orphan: the reply names a token whose descriptor never arrives. The guest must report
		// the failure rather than fabricate a descriptor, and must not wait forever.
		Metrics::shared().fdCourierSentToGuest.fetch_add(1, std::memory_order_relaxed);
		close(fd);
		Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
		return token;
	}

	ssize_t sent = sendmsg(socket, &msg, MSG_NOSIGNAL);
	if (sent != (ssize_t)sizeof(envelope)) {
		// The guest will not see a token it cannot resolve: the caller falls back, and this descriptor
		// stays the caller's to close (it was never transferred).
		Metrics::shared().fdCourierSendFail.fetch_add(1, std::memory_order_relaxed);
		// NAME THE FAILURE. MEASURED: the doorbell was sent from the arm with the right connection selected
		// (connPeer == pid, isLoader=1) and still returned 0, and a bare counter cannot say whether the socket
		// refused (EAGAIN/EPIPE/ENOBUFS) or the write was short. Every failure branch in this session that stayed
		// silent cost a diagnostic cycle.
		{
			static DarlingServer::Log sendFailLog("fd-courier-send-fail");
			sendFailLog.error() << "fd-courier-send-fail pid=" << pid << " kind=" << kind << " socket=" << socket
				<< " sent=" << (long long)sent << " want=" << (long long)sizeof(envelope)
				<< " errno=" << errno << sendFailLog.endLog;
		}
		return 0;
	}
	auto conn = _fdCourierConns.find(socket);
	if (conn != _fdCourierConns.end()) {
		conn->second.sent++;
	}
	Metrics::shared().fdCourierSentToGuest.fetch_add(1, std::memory_order_relaxed);
	close(fd);
	Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
	if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
		static DarlingServer::Log courierLog("fd-courier");
		courierLog.error() << "sent-to-guest pid=" << pid << " token=" << token << " kind=" << kind
			<< " gen=" << generation << courierLog.endLog;
	}
	return token;
}

void DarlingServer::Server::_drainFdCourierMessages() {
	// Accept FIRST. MEASURED: a lookup that only reads already-accepted connections cannot see the
	// descriptor a bootstrap caller just sent on a connection the loop has not accepted yet, so the
	// operation reported a committed failure (EBADF) for a descriptor that was sitting in the backlog.
	// The listener event accepts too; doing it here as well is what makes a resolve independent of
	// whether an event pass has run in between.
	if (_fdCourierListenerSocket >= 0) {
		_handleFdCourierConnection();
	}
	if (_fdCourierConns.empty()) {
		return;
	}
	std::vector<int> dead;
	for (auto& [socket, conn] : _fdCourierConns) {
		while (true) {
			dserver_fd_courier_message envelope {};
			char control[CMSG_SPACE(sizeof(int) * 8)];
			struct iovec iov { &envelope, sizeof(envelope) };
			struct msghdr msg {};
			memset(&msg, 0, sizeof(msg));
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			msg.msg_control = control;
			msg.msg_controllen = sizeof(control);

			ssize_t n = recvmsg(socket, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
					break; // nothing more right now; the connection stays open
				}
				dead.push_back(socket);
				break;
			}
			if (n == 0) {
				dead.push_back(socket); // the process closed its end (exit or fork reset)
				break;
			}
			if (n == 1) {
				// perf#30 PHASE-0 WAKE: a one-byte message on the courier connection is the management
				// page's wake before any doorbell exists. It carries no descriptor and no semantics; its
				// only effect is that this read returns, the loop completes a pass, and _serviceProcessControl
				// runs. Counted separately so it cannot be mistaken for courier traffic.
				Metrics::shared().processControlWakes.fetch_add(1, std::memory_order_relaxed);
				// perf#30 (doc section 179): the wake was RECEIVED. The counter alone cannot be read after a run
				// ends, and the question "did the byte arrive at all" is exactly what separates a wake that is
				// sent into a connection nobody reads from a wake that arrives and is not acted on.
				if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
					static DarlingServer::Log wakeLog("process-control-wake");
					wakeLog.error() << "wake-received socket=" << socket << " passes=" << g_processControlPasses
						<< wakeLog.endLog;
				}
				continue;
			}
			if (n != (ssize_t)sizeof(envelope)) {
				// SOCK_SEQPACKET preserves boundaries, so a short envelope is malformed, not a partial
				// read. Close whatever descriptors came with it and keep the connection.
				for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
					if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
						continue;
					}
					uint32_t count = (uint32_t)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
					int* fds = (int*)CMSG_DATA(cmsg);
					for (uint32_t i = 0; i < count; ++i) {
						if (fds[i] >= 0) {
							close(fds[i]);
							Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
						}
					}
				}
				Metrics::shared().fdCourierMalformed.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			const pid_t pid = conn.peerPid;
			uint32_t received = 0;
			int fds[8];
			uint32_t valid = 0;
			for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
				if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
					continue;
				}
				uint32_t count = (uint32_t)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
				int* came = (int*)CMSG_DATA(cmsg);
				for (uint32_t i = 0; i < count && valid < 8; ++i) {
					received++;
					fds[valid++] = came[i];
				}
			}
			conn.received += received;
			Metrics::shared().fdCourierFdsReceived.fetch_add(received, std::memory_order_relaxed);

			// Reject before pairing, never after: a descriptor that cannot be paired is closed here.
			// Every reject is a REAL close of a real file object, which is what keeps a mutation from
			// leaking a descriptor (and what makes the counters meaningful rather than cosmetic).
			auto closeAll = [&]() {
				for (uint32_t i = 0; i < valid; ++i) {
					close(fds[i]);
					Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
				}
			};

			if (valid != envelope.fd_count || valid == 0) {
				Metrics::shared().fdCourierMalformed.fetch_add(1, std::memory_order_relaxed);
				closeAll();
				continue;
			}
			switch (envelope.kind) {
				case DSERVER_FD_COURIER_KIND_LANE_BACKING:
				case DSERVER_FD_COURIER_KIND_CHECKOUT_FD:
				case DSERVER_FD_COURIER_KIND_CHECKIN_FD:
					break;
				case DSERVER_FD_COURIER_KIND_PROCESS_CONTROL: {
					// the page is created only by the loader, so this connection is the loader's (doc 90)
					conn.isLoader = true;   // this connection delivered the page, so it is the loader's (doc 90)
					// perf#30 PROCESS-CONTROL PLANE: the management page. A shared region, not a request --
					// map it and keep it for the process's lifetime; nothing to pair, nothing to resolve.
					struct stat st;
					if (fstat(fds[0], &st) != 0 || st.st_size < (off_t)sizeof(dserver_process_control)) {
						Metrics::shared().fdCourierMalformed.fetch_add(1, std::memory_order_relaxed);
						closeAll();
						continue;
					}
					void* mapped = mmap(nullptr, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
					if (mapped == MAP_FAILED) {
						Metrics::shared().fdCourierMalformed.fetch_add(1, std::memory_order_relaxed);
						closeAll();
						continue;
					}
					auto& region = _processControl[pid];
					// A NEW PAGE MEANS A NEW INCARNATION OF THIS PID, and the once-per-process doorbell records must be
					// reset with it. MEASURED: without this, an incarnation that re-execs under the same pid kept the
					// guard set from its predecessor, no doorbell was sent, and its publishes reported `via=none` while
					// the first incarnation's were `via=doorbell` -- i.e. the ordering fix would silently apply to some
					// processes and not others.
					_planeDoorbellDelivered.erase(pid);
					if (region.fd >= 0) {
						close(region.fd);
					}
					// perf#30 (doc section 199): the OLD mapping is deliberately NOT unmapped. A completion for the
					// previous incarnation of this pid's page can still be in flight -- it is reached after a
					// nested pump -- and munmapping here destroys the memory that completion writes into, which
					// was measured as a SIGSEGV at `page->reply_seq` and, in every earlier run, as a request that
					// was serviced and never completed. The old descriptor is closed (closing does not unmap) and
					// the mapping is left for the process's lifetime: 528 bytes per incarnation is the price of a
					// completion that cannot land in destroyed memory.
					(void)0;
					region.fd = fds[0];
					region.map = mapped;
					region.size = (size_t)st.st_size;
					// perf#30 (violation A, directive section 2): THE process bootstrap descriptor packet establishes the
					// doorbell. This is the earliest point at which the server knows the process AND has the region, and
					// it is measured to be earlier than the loader's bounded window -- the loader's own instrument
					// reports `drain attempts=200 adopted=0`, i.e. the service-pass delivery arrives too late. Sent to
					// the LOADER connection and with drainFirst=false, because this code runs INSIDE the courier receive
					// callback and re-entering the drain was measured to lose a bundle. Its own guard, so the lane
					// attach keeps its delivery (which the shared guard had been cancelling: attach-rc wake=-1).
					// UNCONDITIONAL probe: the doorbell handover below never logged, and a branch that does not say it was
					// reached is indistinguishable from one that was skipped. This line costs one write per process and
					// answers "is the page-envelope arm the one that runs?" without an inference.
					{
						static DarlingServer::Log envProbe("plane-doorbell-envelope");
						envProbe.error() << "plane-doorbell-envelope-enter pid=" << pid << envProbe.endLog;
					}
					// The handover is DEFERRED to the end of this drain pass. MEASURED why: attempting the send HERE fails
					// silently (sendFdCourierBundleToGuest returns 0 while this receive callback is still executing, and the
					// else branch logged nothing), while the same send from the service pass succeeds but only ~3.3 s later --
					// after the loader's whole bootstrap window. Marking the pid and flushing outside the callback gets the
					// descriptor to the loader while it is still waiting.
					// DIRECT send in this arm. MEASURED: deferring to the drain's tail does not work -- the flush runs
					// with an EMPTY connection map (`conns=[]`), so every pid failed with socket=-1 and the queue was
					// consumed in silence. Here the map is non-empty by construction (we are iterating it), and the
					// loader lookup now selects by the isLoader flag, which the earlier pid-based lookup could not.
					{
						// The arm's own view of the connection it is handling, next to the send's view. The two were
						// measured to disagree (`pair=FAILED` while a route line named another pid's connection), and a
						// mismatch that is printed is a fact instead of an inference.
						{
							std::string armMap;
							for (auto& [mSock, mConn] : _fdCourierConns) {
								armMap += std::to_string(mSock) + ":" + std::to_string(mConn.peerPid) + ":" + (mConn.isLoader ? "L" : "-") + " ";
							}
							static DarlingServer::Log armLog("plane-doorbell-arm");
							armLog.error() << "plane-doorbell-arm pid=" << pid << " socket=" << socket
								<< " connPeer=" << conn.peerPid << " connLoader=" << (conn.isLoader ? 1 : 0)
								<< " conns=[" << armMap << "]" << armLog.endLog;
						}
						// The handover must be stamped with the generation THIS incarnation announced in the envelope
						// that created the region; the function derives the stamp from _fdCourierGeneration, which is
						// not yet set for a brand-new process, and its documented generation-0 behaviour then refuses
						// the bundle (MEASURED: the arm's connection was correct, sendmsg did not fail, and the call
						// still returned 0). Recording the guest's own value here is what makes the bootstrap bundle
						// acceptable to that guest.
						if (envelope.process_generation != 0) {
							_fdCourierGeneration[pid] = envelope.process_generation;
						}
						int bootFd = ringDoorbellDupForGuest();
						uint64_t bootTok = 0;
						if (bootFd >= 0) {
							bootTok = sendFdCourierBundleToGuest(pid, DSERVER_FD_COURIER_KIND_PROCESS_DOORBELL, bootFd, true, false);
							if (bootTok == 0) {
								::close(bootFd);
							}
						}
						static DarlingServer::Log bootLog("plane-doorbell-sent");
						if (bootTok != 0) {
							_planeDoorbellDelivered.insert(pid);
							bootLog.error() << "plane-doorbell-sent pid=" << pid << " token=" << bootTok << " via=envelope" << bootLog.endLog;
						} else {
							bootLog.error() << "plane-doorbell-sent pid=" << pid << " via=envelope pair=FAILED fd=" << bootFd << bootLog.endLog;
						}
					}
					if (region.size >= sizeof(dserver_process_control)) {
						auto* page = static_cast<dserver_process_control*>(region.map);
						// perf#30 (directive D1): the version is a REAL check now. transport_ready is the
						// promise that this page may be used, so it is published only when the offset layout
						// matches: size first (done above), then version. On mismatch the plane is UNAVAILABLE,
						// never partially interpreted -- the guest's bounded wait expires and its datagram
						// fallback runs, which is the documented pre-publication legacy path.
						uint32_t pageVersion = __atomic_load_n(&page->version, __ATOMIC_ACQUIRE);
						if (pageVersion != DSERVER_PROCESS_CONTROL_VERSION) {
							Metrics::shared().fdCourierMalformed.fetch_add(1, std::memory_order_relaxed);
							if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
								static DarlingServer::Log verLog("process-control-version");
								verLog.error() << "region-refused pid=" << pid << " page_version=" << pageVersion
									<< " expected=" << DSERVER_PROCESS_CONTROL_VERSION
									<< " size=" << st.st_size << verLog.endLog;
							}
							// keep the mapping for diagnostics but never mark it usable
							for (uint32_t i = 0; i < valid; ++i) {
								fds[i] = -1;
							}
							continue;
						}
						__atomic_store_n(&page->transport_ready, 1u, __ATOMIC_RELEASE);
						__atomic_store_n(&page->futex, __atomic_load_n(&page->futex, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELEASE);
						syscall(SYS_futex, &page->futex, FUTEX_WAKE, INT32_MAX, nullptr, nullptr, 0);
					}
					for (uint32_t i = 0; i < valid; ++i) {
						fds[i] = -1; // the region owns it now; the shared cleanup must not close it
					}
					Metrics::shared().processControlRegions.fetch_add(1, std::memory_order_relaxed);
					if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
						static DarlingServer::Log pcLog("process-control");
						pcLog.error() << "region pid=" << pid << " passes=" << g_processControlPasses
							<< " size=" << st.st_size
							<< " page=" << mapped << " gen=" << envelope.process_generation
							<< " sz=" << sizeof(dserver_process_control)
							<< " off=" << (size_t)((char*)&((dserver_process_control*)0)->transport_ready - (char*)0)
							<< " ino=" << (unsigned long long)st.st_ino
							<< " dev=" << (unsigned long long)st.st_dev << pcLog.endLog;
					}
					continue;
				}
				default:
					Metrics::shared().fdCourierKindReject.fetch_add(1, std::memory_order_relaxed);
					closeAll();
					continue;
			}

			// The generation names the process INCARNATION the descriptor belongs to. It ADVANCES rather
			// than being rejected when it changes for a live pid, because execve keeps the pid while the
			// loader starts a fresh incarnation -- MEASURED: rejecting on change made the post-exec
			// descriptor look stale and the call parked forever. The rejection that matters is a
			// descriptor arriving for a pid whose incarnation has since been REUSED by another process,
			// and that is caught by the token (unique per call, per image) plus the process-lifetime
			// cleanup of pending bundles.
			auto gen = _fdCourierGeneration.find(pid);
			if (gen == _fdCourierGeneration.end()) {
				_fdCourierGeneration[pid] = envelope.process_generation;
			} else if (gen->second != envelope.process_generation) {
				gen->second = envelope.process_generation;
				Metrics::shared().fdCourierGenerationAdvance.fetch_add(1, std::memory_order_relaxed);
			}

			const uint64_t key = envelope.token;  // token, not pid: see the registry comment in server.hpp
			if (_fdCourierPending.find(key) != _fdCourierPending.end()) {
				// A second descriptor under a token that is still pending: the first one stays the
				// operation's descriptor and this one is closed (the operation executes exactly once).
				Metrics::shared().fdCourierDuplicateReject.fetch_add(1, std::memory_order_relaxed);
				closeAll();
				continue;
			}
			// The extra descriptors beyond the first are closed: one token names one descriptor, so a
			// bundle that claims more (or fewer) can never be silently accepted as "close enough".
			for (uint32_t i = 1; i < valid; ++i) {
				close(fds[i]);
				Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
			}

			static DarlingServer::Log courierLog("fd-courier");
			if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
				// Diagnostics only: the courier is a transport, so its traffic must be observable when
				// something waits forever for a descriptor half.
				courierLog.error() << "bundle pid=" << pid << " token=" << envelope.token
					<< " kind=" << envelope.kind << " gen=" << envelope.process_generation
					<< " pending=" << _fdCourierPending.size() << courierLog.endLog;
			}

			auto waiter = _fdCourierWaiters.find(key);
			if (waiter != _fdCourierWaiters.end()) {
				// Semantic half first: resume it with this descriptor rather than parking ours.
				auto resume = std::move(waiter->second.resume);
				_fdCourierWaiters.erase(waiter);
				Metrics::shared().fdCourierFdMatched.fetch_add(1, std::memory_order_relaxed);
				resume(fds[0]);
			} else {
				FdCourierBundle bundle;
				bundle.pid = pid;
				bundle.fd = fds[0];
				bundle.kind = envelope.kind;
				bundle.generation = envelope.process_generation;
				_fdCourierPending[key] = bundle;
				if (_fdCourierPending.size() > _fdCourierPendingPeak) {
					_fdCourierPendingPeak = _fdCourierPending.size();
					Metrics::shared().fdCourierPendingPeak.store(_fdCourierPendingPeak, std::memory_order_relaxed);
				}
			}
		}
	}
	for (int socket : dead) {
		auto it = _fdCourierConns.find(socket);
		if (it != _fdCourierConns.end()) {
			// WHY A CONNECTION DISAPPEARS MUST BE VISIBLE. MEASURED: the failing boot ends with the server holding an
			// EMPTY connection map and being asked to wake a pid it can no longer reach (plane-doorbell-route
			// pid=... socket=-1 conns=[]), so the wake has nowhere to go and the guest waits forever -- yet the
			// removal itself was silent, and "the map is empty" cannot say who closed it or when. Unconditional
			// (a gated instrument is indistinguishable from no instrument) but rare: one line per closed connection.
			{
				struct stat cst;
				unsigned long long cino = (fstat(socket, &cst) == 0) ? (unsigned long long)cst.st_ino : 0ull;
				static DarlingServer::Log closeLog("fd-courier-conn-closed");
				closeLog.error() << "fd-courier-conn-closed fd=" << socket << " ino=" << cino
					<< " peerPid=" << it->second.peerPid << " isLoader=" << (it->second.isLoader ? 1 : 0)
					<< " remaining=" << (_fdCourierConns.size() - 1) << closeLog.endLog;
			}
			_fdCourierConns.erase(it);
			Metrics::shared().fdCourierLiveConnections.fetch_sub(1, std::memory_order_relaxed);
		}
		close(socket);
	}


};

DarlingServer::Server::FdCourierResult DarlingServer::Server::resolveFdCourierBundle(pid_t pid, uint64_t token,
		uint32_t kind, int* outFd, std::function<void(int)> onArrival) {
	// Drain first: the guest sends the descriptor BEFORE it publishes the request that names the token,
	// so a request handled "first" by the event loop still finds its descriptor already enqueued.
	_drainFdCourierMessages();

	const uint64_t key = token;  // token, not pid (measured: the two halves carry different pids)
	auto it = _fdCourierPending.find(key);
	if (it == _fdCourierPending.end()) {
		// Not here yet. There is no polling and no blocking: the caller parks a continuation and the
		// descriptor's arrival resumes it. Process exit closes both halves (see _closeFdCourierForPid).
		FdCourierWaiter w;
		w.pid = pid;
		w.resume = std::move(onArrival);
		_fdCourierWaiters[key] = std::move(w);
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log courierLog("fd-courier");
			courierLog.error() << "PARKED pid=" << pid << " token=" << token << " kind=" << kind
				<< " pending=" << _fdCourierPending.size() << courierLog.endLog;
		}
		return FdCourierResult::Missing;
	}
	FdCourierBundle bundle = it->second;
	_fdCourierPending.erase(it);

	if (bundle.kind != kind) {
		close(bundle.fd);
		Metrics::shared().fdCourierKindReject.fetch_add(1, std::memory_order_relaxed);
		Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
		return FdCourierResult::KindMismatch;
	}
	// A bundle whose generation is not the pid's current one is an incarnation that has already been
	// replaced (execve, or a reused pid). Its descriptor is closed rather than paired: the operation it
	// belonged to is over, and pairing it with a live request would be exactly the wrong-descriptor bug
	// this check exists to prevent.
	auto gen = _fdCourierGeneration.find(bundle.pid >= 0 ? bundle.pid : pid);
	if (gen != _fdCourierGeneration.end() && gen->second != bundle.generation) {
		close(bundle.fd);
		Metrics::shared().fdCourierStaleGenerationReject.fetch_add(1, std::memory_order_relaxed);
		Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
		return FdCourierResult::StaleGeneration;
	}
	*outFd = bundle.fd;
	Metrics::shared().fdCourierFdMatched.fetch_add(1, std::memory_order_relaxed);
	if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
		static DarlingServer::Log courierLog("fd-courier");
		courierLog.error() << "resolved pid=" << pid << " token=" << token << " kind=" << kind
			<< " fd=" << bundle.fd << courierLog.endLog;
	}
	return FdCourierResult::Resolved;
};

// perf#30 PROCESS-CONTROL PLANE: run pending management requests, in the main loop, one per process
// page. These operations are rare (bootstrap and teardown) and the page holds ONE outstanding request,
// so there is no queue and no extra wake path: the loop services the pages in the same pass, and the
// outcome is written back into the same page.
void DarlingServer::Server::_serviceProcessControl() {
	for (auto& [pid, region] : _processControl) {
		if (!region.map || region.size < sizeof(dserver_process_control)) {
			continue;
		}
		auto* page = static_cast<dserver_process_control*>(region.map);
		// perf#30 (violation A, SECOND attempt, behind a hatch). The first attempt sent this from the courier DRAIN
		// callback and broke the boot: sendFdCourierBundleToGuest() itself drains first, so the send re-entered the
		// drain it was called from and a bundle was lost silently (MEASURED: 6 publishes, doorbell=0, then a stall).
		// This is the safe context instead -- the same pass that builds the lane-attach delivery, which has always
		// drained from here without harm. Hatched so the default path stays exactly the measured-green one.
		// UNCONDITIONAL, because every external control channel was MEASURED unreachable for this server: an exported
		// variable does not arrive (no server-side records at all) and neither does a flag file (the server runs in the
		// rootless mount namespace and does not see the caller's /tmp -- `plane-doorbell-sent` never appeared while the
		// file demonstrably existed). The control channel is therefore the PROTOCOL itself, which is what directive
		// section 2 asks for: the process bootstrap descriptor packet establishes the doorbell. Safe to leave on: with
		// the dedicated guard the attach keeps its own delivery (measured PASS), and once per process incarnation.
		if (_planeDoorbellDelivered.count(pid) == 0) {
			int earlyWakeFd = ringDoorbellDupForGuest();
			if (earlyWakeFd >= 0) {
				uint64_t earlyTok = sendFdCourierBundleToGuest(pid, DSERVER_FD_COURIER_KIND_PROCESS_DOORBELL, earlyWakeFd, true);
				if (earlyTok != 0) {
					_planeDoorbellDelivered.insert(pid);
					// UNCONDITIONAL on purpose: the server's own env hatches are NOT reachable from the caller (measured:
					// an exported DARLING_SERVER_COURIER_LOG produced no server-side records at all), so a hatched
					// instrument here cannot answer "did the early send happen". One line per process incarnation, and
					// it is the fact the ordering analysis needs.
					{
						static DarlingServer::Log dbLog("plane-doorbell-sent");
						dbLog.error() << "plane-doorbell-sent pid=" << pid << " token=" << earlyTok << dbLog.endLog;
					}
				} else {
					::close(earlyWakeFd);
				}
			}
		}
		// perf#30 (doc section 199): COPY the region's fields. `region` is a reference into `_processControl`, and
		// the completion below can be reached after a nested pump that re-registers this pid's page -- which
		// unmaps the old mapping and rewrites the entry, leaving the reference describing memory that is gone.
		// MEASURED as a SIGSEGV at `page->reply_seq` (offset 56) with the address guard one statement earlier
		// reporting the pointers equal. The locals are what the rest of this body must use.
		void* const regionMap = region.map;
		const size_t regionSize = region.size;
		const int regionFd = region.fd;
		(void)regionSize; (void)regionFd;
		// perf#30 (doc section 173): a PASS counter, and the one observation that separates "the loop never runs a
		// pass" from "the loop runs and does not service this request". MEASURED: the second incarnation's page is
		// registered, mapped and transport-ready, its request slot is PENDING, and no `request` line is ever
		// emitted for it -- two very different causes that this line tells apart. It fires only when a pass sees a
		// live request, so a healthy boot prints it a handful of times and a stalled one prints it forever.
		{
			++g_processControlPasses;
			// A heartbeat every thousand passes: whether passes keep happening at all is the question, and a
			// counter that is only readable while something is wrong cannot answer it.
			if (getenv("DARLING_SERVER_COURIER_LOG") != NULL && (g_processControlPasses % 1000) == 0) {
				static DarlingServer::Log hbLog("process-control-pass");
				hbLog.error() << "heartbeat passes=" << g_processControlPasses << " pid=" << pid << hbLog.endLog;
			}
			if (__atomic_load_n(&page->request_state, __ATOMIC_ACQUIRE) == DSERVER_PROCESS_CONTROL_PENDING
					&& getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
				static DarlingServer::Log passLog("process-control-pass");
				passLog.error() << "pass=" << g_processControlPasses << " sees-pending pid=" << pid
					<< " op=" << page->request_op << " seq=" << page->request_seq
					<< " reply_state=" << page->reply_state
					<< " transport_ready=" << __atomic_load_n(&page->transport_ready, __ATOMIC_ACQUIRE)
					<< passLog.endLog;
			}
		}
		// perf#30: publish the guest's attach-route attribution. The guest counts on the page because the
		// environment-gated diagnostic is silent in exactly the window this needs to explain.
		{
			// DELTA, not the value: these counters are per-process monotonic, so publishing them directly
			// made the reading a single sample of the last-serviced process rather than a run total.
			auto& m = Metrics::shared();
			uint32_t nowOk = __atomic_load_n(&page->attach_route_ok, __ATOMIC_RELAXED);
			uint32_t nowNr = __atomic_load_n(&page->attach_route_not_ready, __ATOMIC_RELAXED);
			uint32_t nowRf = __atomic_load_n(&page->attach_route_refused, __ATOMIC_RELAXED);
			uint32_t nowCl = __atomic_load_n(&page->attach_route_claimed_no_lane, __ATOMIC_RELAXED);
			uint32_t nowNs = __atomic_load_n(&page->attach_route_no_slot, __ATOMIC_RELAXED);
			if (nowOk >= region.lastRouteOk) { m.attachRouteOk.fetch_add(nowOk - region.lastRouteOk, std::memory_order_relaxed); }
			if (nowNr >= region.lastRouteNotReady) { m.attachRouteNotReady.fetch_add(nowNr - region.lastRouteNotReady, std::memory_order_relaxed); }
			if (nowRf >= region.lastRouteRefused) { m.attachRouteRefused.fetch_add(nowRf - region.lastRouteRefused, std::memory_order_relaxed); }
			region.lastRouteOk = nowOk;
			region.lastRouteNotReady = nowNr;
			if (nowCl >= region.lastRouteClaimedNoLane) { m.attachRouteClaimedNoLane.fetch_add(nowCl - region.lastRouteClaimedNoLane, std::memory_order_relaxed); }
			region.lastRouteRefused = nowRf;
			if (nowNs >= region.lastRouteNoSlot) { m.attachRouteNoSlot.fetch_add(nowNs - region.lastRouteNoSlot, std::memory_order_relaxed); }
			region.lastRouteClaimedNoLane = nowCl;
			region.lastRouteNoSlot = nowNs;
		}
		// perf#30: the URGENT pool is drained BEFORE the management slot is examined. MEASURED: placed after
		// the `request_state != PENDING` guard it never ran on a page with no management request, which is the
		// common case -- the signal-context calls published and sat in PENDING forever, and the fire-and-forget
		// variant looked like it worked only because it never waits.
		// perf#30 (directive sections 19-21): drain the URGENT pool. These are the signal-context calls, which
		// publish without waiting, so the server services every pending slot in this same pass and writes the
		// informational reply back. No waiter, no futex wake: the caller has already returned.
			// perf#30 (doc section 246): SERVICE THE PENDING URGENT SLOTS IN PUBLISH ORDER, NOT IN SLOT-INDEX ORDER.
			// MEASURED: the guest's signal-context protocol publishes INTERRUPT_ENTER and then SIGPROCESS, and the
			// server services a SIGPROCESS by running `Thread::processSignal`, which writes through `_interrupts.top()`.
			// `_interrupts` is pushed by the INTERRUPT_ENTER call, so when the two landed in different slots and the
			// loop walked slots by index, SIGPROCESS could be serviced FIRST -- `top()` on an empty stack, an immediate
			// SIGSEGV inside `processSignal`, and the server gone (from outside: a hang). `urgent_seq` exists precisely
			// to order these, so the pass honours it now: collect the pending slots, sort by sequence, service oldest
			// first. No allocation, and the reply and state stores are unchanged.
			unsigned urgentOrder[DSERVER_PROCESS_CONTROL_URGENT_SLOTS];
			uint32_t urgentOrderSeq[DSERVER_PROCESS_CONTROL_URGENT_SLOTS];
			unsigned urgentOrderCount = 0;
			for (unsigned u = 0; u < DSERVER_PROCESS_CONTROL_URGENT_SLOTS; ++u) {
				if (__atomic_load_n(&page->urgent_state[u], __ATOMIC_ACQUIRE) == DSERVER_PROCESS_CONTROL_URGENT_PENDING) {
					unsigned j = urgentOrderCount++;
					urgentOrder[j] = u;
					urgentOrderSeq[j] = page->urgent_seq[u];
					while (j > 0 && urgentOrderSeq[j - 1] > urgentOrderSeq[j]) {
						unsigned tu = urgentOrder[j];
						urgentOrder[j] = urgentOrder[j - 1];
						urgentOrder[j - 1] = tu;
						uint32_t ts = urgentOrderSeq[j];
						urgentOrderSeq[j] = urgentOrderSeq[j - 1];
						urgentOrderSeq[j - 1] = ts;
						j--;
					}
				}
			}
			for (unsigned ui = 0; ui < urgentOrderCount; ++ui) {
				const unsigned u = urgentOrder[ui];
			uint32_t uop = page->urgent_op[u];
			uint32_t useq = page->urgent_seq[u];
			// Same rule as the management slot: the publisher's thread identity, published with the slot by a
			// raw syscall because a signal-context publisher cannot touch lazily-initialised machinery.
			int32_t urgentTid = __atomic_load_n(&page->urgent_tid[u], __ATOMIC_ACQUIRE);
			if (urgentTid <= 0) {
				urgentTid = 0;
			}
			int32_t ustatus = -ENOSYS;
			if (uop == DSERVER_PROCESS_CONTROL_OP_INTERRUPT_ENTER) {
				Message message(sizeof(dserver_rpc_call_interrupt_enter_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_interrupt_enter_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->urgent_payload[u][3] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = urgentTid;
				call->header.number = dserver_callnum_interrupt_enter;
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						ustatus = created->suppressedReplyCode();
					} else { ustatus = -ESRCH; }
				} else { ustatus = -ESRCH; }
			} else if (uop == DSERVER_PROCESS_CONTROL_OP_SIGPROCESS) {
				// perf#30 (doc section 87/88): the raw-signal-handler call. The four int32s travel packed two
				// per word, the signal address and the two structure POINTERS the server writes into travel as
				// words -- five of the eight the widened pool carries. The reply's new signal number is written
				// back into the slot's payload, since the urgent pool has only a status word of its own.
				Message message(sizeof(dserver_rpc_call_sigprocess_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_sigprocess_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->urgent_payload[u][5] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = urgentTid;
				call->header.number = dserver_callnum_sigprocess;
				call->body.bsd_signal_number = static_cast<int32_t>(page->urgent_payload[u][0] & 0xffffffffu);
				call->body.linux_signal_number = static_cast<int32_t>(page->urgent_payload[u][0] >> 32);
				call->body.sender_pid = static_cast<int32_t>(page->urgent_payload[u][1] & 0xffffffffu);
				call->body.code = static_cast<int32_t>(page->urgent_payload[u][1] >> 32);
				call->body.signal_address = page->urgent_payload[u][2];
				call->body.thread_state = page->urgent_payload[u][3];
				call->body.float_state = page->urgent_payload[u][4];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						ustatus = created->suppressedReplyCode();
						size_t bodyLen = 0;
						const uint8_t* body = created->suppressedReplyBody(&bodyLen);
						if (body && bodyLen >= sizeof(dserver_reply_sigprocess_t)) {
							auto* rb = reinterpret_cast<const dserver_reply_sigprocess_t*>(body);
							page->urgent_payload[u][6] = (uint64_t)(uint32_t)rb->new_bsd_signal_number;
						}
					} else { ustatus = -ESRCH; }
				} else { ustatus = -ESRCH; }
			} else if (uop == DSERVER_PROCESS_CONTROL_OP_INTERRUPT_EXIT) {
				Message message(sizeof(dserver_rpc_call_interrupt_exit_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_interrupt_exit_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->urgent_payload[u][3] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = urgentTid;
				call->header.number = dserver_callnum_interrupt_exit;
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						ustatus = created->suppressedReplyCode();
					} else { ustatus = -ESRCH; }
				} else { ustatus = -ESRCH; }
			}
			// perf#30 (user directive: a census row must have an INSTRUMENT behind it, never a bare UNMEASURED).
			// One line per serviced urgent slot, so `dwdiag courier` can count the urgent plane and `dwdiag witness`
			// can tell a silent instrument from a quiet transport. MEASURED need: the transport census printed
			// `urgent shared plane UNMEASURED` because nothing in the tree ever recorded this path.
			if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
				static DarlingServer::Log urgentLog("urgent-service");
				urgentLog.error() << "urgent-service pid=" << pid << " slot=" << u << " op=" << uop
					<< " seq=" << useq << " status=" << ustatus << " tid=" << urgentTid << urgentLog.endLog;
			}
			page->urgent_reply_status[u] = ustatus;
			page->urgent_reply_seq[u] = useq;
			__atomic_store_n(&page->urgent_state[u], DSERVER_PROCESS_CONTROL_URGENT_DONE, __ATOMIC_RELEASE);
		}

		if (__atomic_load_n(&page->request_state, __ATOMIC_ACQUIRE) != DSERVER_PROCESS_CONTROL_PENDING) {
			continue;
		}
		uint32_t op = page->request_op;
		uint32_t seq = page->request_seq;
		// perf#30 (directive section 2/11): the THREAD IDENTITY of the requester travels in the transport
		// envelope (request_tid), and it is what THREAD-scoped paths use. The handlers below are
		// PROCESS_SCOPED: their semantics belong to the process, and their execution target is therefore the
		// process's own thread, which is what `pid` names. MEASURED: sending the requester's tid for these
		// broke ATTACH_LANE (the loader's socket registration) immediately -- `Failed to tell darlingserver
		// about our dyld info` -- because the attach must run on the main thread. The envelope identity is
		// used where the requester's own thread is the subject: the urgent pool and pthread_canceled.
		// perf#30 (doc section 192): the file identity AT SERVICE TIME, not at registration time. A descriptor's
		// (dev, ino) can be read from the descriptor the region holds right now, and comparing it with what the
		// guest reports for the page it polls is the only version of this comparison that cannot be defeated by
		// the region having been replaced between the two observations.
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			struct stat svst;
			unsigned long long svino = 0, svdev = 0, svsize = 0;
			if (region.fd >= 0 && fstat(region.fd, &svst) == 0) {
				svino = (unsigned long long)svst.st_ino;
				svdev = (unsigned long long)svst.st_dev;
				svsize = (unsigned long long)svst.st_size;
			}
			static DarlingServer::Log svcLog("process-control-service");
			svcLog.error() << "service pid=" << pid << " op=" << op << " region_fd=" << region.fd
				<< " ino=" << svino << " dev=" << svdev << " size=" << svsize
				<< " map=" << (void*)page << svcLog.endLog;
		}
		int32_t planeTid = __atomic_load_n(&page->request_tid, __ATOMIC_ACQUIRE);
		if (planeTid <= 0) {
			planeTid = 0;
		}
		int32_t status = 0;
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log pcLog("process-control");
			pcLog.error() << "request pid=" << pid << " op=" << op << " seq=" << seq
				<< " payload0=" << page->request_payload[0] << " payload1=" << page->request_payload[1]
				<< " payload2=" << page->request_payload[2] << " payload3=" << page->request_payload[3]
				<< pcLog.endLog;
		}
		// perf#30 (directive D1/D2, doc section 207): THREAD IDENTITY IS PART OF THE ENVELOPE. MEASURED: a plane
		// handler that stamped `tid = pid` executed `semaphore_signal` on the very thread that was legitimately
		// parked inside a blocking Ring `semaphore_wait` -- a Ring Call whose fiber the server had suspended --
		// and the state machine refused it as "Thread has both a pending call and a pending continuation
		// (pending_call=58)". The publisher's OWN tid is what the request belongs to, and it is published with the
		// request before the release store of request_state; `pid` remains the owning process. Falling back to
		// `pid` preserves the old behaviour only for a publisher that did not name itself.
		// perf#30 (directive D1/D2 section 11, doc section 215): the CLASSIFICATION the directive asks for, decided by
		// measurement. MEASURED: stamping EVERY plane handler with the publisher's tid stopped `SET_DYLD_INFO`
		// (op=4) from ever being answered -- the loader sat in its request loop printing `iter=9000 waited=0
		// reply=0 seq=3` while the Call it had published was dispatched onto the very thread that had published it,
		// which is busy by construction. Process-scoped semantics (dyld info, executable path, the Mach port
		// operations, vchroot) do not depend on WHICH thread executes them, so they use the process's own thread,
		// which is the convention that worked before; the publisher's tid is used only where the operation's
		// meaning is tied to the calling thread. The helper remains for those sites.
		bool planeDeferred = false;
		const int32_t planeRequestTid = [&]() -> int32_t {
			int32_t t = __atomic_load_n(&page->request_tid, __ATOMIC_ACQUIRE);
			return (t > 0) ? t : static_cast<int32_t>(pid);
		}();
		(void)planeRequestTid;

		switch (op) {
			case DSERVER_PROCESS_CONTROL_OP_THREAD_SELF_BOOTSTRAP: {
			// perf#30 (doc section 228): the trap's home is the management PLANE, but it CANNOT be answered directly in
			// this pass. MEASURED (and located by the crash tool's resolved backtrace): calling the duct-tape trap from
			// the pass asserts and aborts --
			//   panic <- Assert <- retrieve_thread_self_fast <- thread_self_trap_for <- dtape_thread_self_trap_for
			//   <- Server::_serviceProcessControl
			// because the trap's semantics need the GUEST THREAD's own execution context, which the pass does not have
			// (doc section 223: the pass is not an XNU thread). So the semantics are reused EXACTLY as the RPC path runs
			// them -- the same technique CHECKIN and ATTACH_LANE use: build the ordinary Call, dispatch it, and read the
			// answer out of the SUPPRESSED reply body. callFromMessage also registers the Process/Thread (that is how the
			// checkin creates the server-side Thread the loader's deferred checkin would otherwise have to wait for), so
			// the loader can ask for the port BEFORE the checkin and the datagram is no longer needed for it.
			// request_payload[0] = the tid whose self port is wanted; request_payload[1] bits 0-7 = the guest architecture.
			Message message(sizeof(dserver_rpc_call_thread_self_trap_t), 0);
			auto* call = reinterpret_cast<dserver_rpc_call_thread_self_trap_t*>(message.data().data());
			*call = {};
			call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[1] & 0xffu);
			call->header.pid = static_cast<int32_t>(pid);
			call->header.tid = static_cast<int32_t>(page->request_payload[0] & 0xffffffffu);
			call->header.number = dserver_callnum_thread_self_trap;
			message.setPID(pid);
			std::shared_ptr<Call> trapCall;
			try {
			trapCall = Call::callFromMessage(std::move(message));
			} catch (const std::exception& e) {
			if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log trapExcLog("process-control-exception");
			trapExcLog.error() << "thread-self-bootstrap-exception pid=" << pid << " seq=" << seq
			<< " what=" << e.what() << trapExcLog.endLog;
			}
			status = -EIO;
			break;
			}
			if (!trapCall) { status = -ESRCH; break; }
			trapCall->suppressReplyDelivery();
			auto trapThread = trapCall->thread();
			if (!trapThread) { status = -ESRCH; break; }
			trapThread->noteServicedFromProcessControl();
			trapThread->doWork();
			status = trapCall->suppressedReplyCode();
			size_t trapBodyLen = 0;
			const uint8_t* trapBody = trapCall->suppressedReplyBody(&trapBodyLen);
			if (status == 0 && trapBody && trapBodyLen >= sizeof(dserver_rpc_reply_thread_self_trap_t) - sizeof(dserver_rpc_replyhdr_t)) {
			auto* rb = reinterpret_cast<const dserver_rpc_reply_thread_self_trap_t*>(trapBody - sizeof(dserver_rpc_replyhdr_t));
			page->reply_payload[0] = rb->body.port_name;
			} else if (status == 0) {
			status = -EIO;
			}
			break;
			}
			case DSERVER_PROCESS_CONTROL_OP_CHECKIN: {
				// The plane's first REAL operation, and the reason it exists: checkin is the first call a
				// new thread makes, so no lane can carry it and the server's Thread object is created by
				// this very call. Rather than reimplement checkin semantics here, BUILD THE ORDINARY CALL
				// and run the ordinary path -- the same technique the ring path uses for its eligible ops
				// (rebuild {callhdr,body}, dispatch through callFromMessage -> doWork). What the plane
				// adds is the two things the RPC path could not give it: the descriptor arrives on the
				// courier, and the completion is the page itself.
				uint64_t fdToken = page->request_payload[2];
				int lifetimeFd = -1;
				if (fdToken != 0) {
					auto result = resolveFdCourierBundle(pid, fdToken, DSERVER_FD_COURIER_KIND_CHECKIN_FD,
						&lifetimeFd, [](int arrivedFd) { if (arrivedFd >= 0) { close(arrivedFd); } });
					if (result != FdCourierResult::Resolved) {
						// no descriptor half for a request that names one: committed failure, never a
						// silent retry on the datagram path
						status = -EBADF;
						// perf#30 (doc section 192): a TWO-WAY memory marker, under the courier hatch only. The
						// guest's memfd and this region were measured to be the same (dev, ino), yet the guest
						// reads reply_state as 0 while this line is executed -- so either the memory is not in
						// fact shared (and (dev, ino) is the wrong identity too) or only the state word is not
						// observed. A magic written through the SAME mapping, before the state store, separates
						// those two, and the guest prints it from the same page it polls.
						if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
							page->reply_payload[1] = 0xF00DF00Du;
						}
						// GENERATION-CONDITIONAL COMPLETION (dar-b5pe, case G), same rule as the main completion:
						// the answer must not be published if the slot has moved on to a newer request.
						if (page->request_seq != seq) {
							if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
								static DarlingServer::Log staleLogC("process-control-stale-completion");
								staleLogC.error() << "completion-discarded site=1 pid=" << pid
									<< " seq=" << seq << " slot_seq=" << page->request_seq << staleLogC.endLog;
							}
							continue;
						}
						page->reply_seq = seq;
						page->reply_status = status;
						__atomic_store_n(&page->reply_state, DSERVER_PROCESS_CONTROL_DONE, __ATOMIC_RELEASE);

		// perf#30 (doc section 194): every site that writes the completion, with a read-back -- the
		// instrumented one never ran for the post-exec checkin while the request WAS serviced, so which
		// path completes it has to be readable rather than inferred.
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log storeLogX("process-control-store");
			storeLogX.error() << "reply-stored-other site=2 pid=" << pid
				<< " readback=" << __atomic_load_n(&page->reply_state, __ATOMIC_ACQUIRE)
				<< " reply_seq=" << page->reply_seq << " map=" << (void*)page << storeLogX.endLog;
		}
						// perf#30 (doc section 195): the slot state is written HERE and the guest must therefore treat DONE as
		// claimable: this store races the guest's own RELEASE (which sets IDLE), and when it wins the slot stays
		// at DONE forever. MEASURED as `seq=3 after-dyld status=-3` with the guest's own "Failed to tell
		// darlingserver about our dyld info"; removing the store entirely broke the boot worse (four log lines),
		// so the pair of changes is: keep the store, and let the claim accept DONE.
		__atomic_store_n(&page->request_state, DSERVER_PROCESS_CONTROL_DONE, __ATOMIC_RELEASE);
						__atomic_store_n(&page->futex, seq, __ATOMIC_RELEASE);
						syscall(SYS_futex, &page->futex, FUTEX_WAKE, INT32_MAX, nullptr, nullptr, 0);
						region.requests++;
						continue;
					}
				}

				Message message(sizeof(dserver_rpc_call_checkin_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_checkin_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[0] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(page->request_payload[1] & 0xffffffff);
				call->header.number = dserver_callnum_checkin;
				call->body.is_fork = (page->request_payload[0] >> 8) != 0;
				call->body.stack_hint = page->request_payload[3];
				// The lifetime descriptor is the difference that matters: the RPC ingest reads it from a
				// CMSG INDEX and hands it to the Process as its lifetime anchor. A page has no CMSG, so
				// the descriptor is attached to the synthesized message as one -- pushDescriptor then the
				// index -- and the ordinary ingest does exactly what it does for a datagram. MEASURED
				// note: leaving it at -1 (the first version of this code) registered the process with no
				// lifetime anchor at all, which is not the same operation the datagram path performs.
				if (lifetimeFd >= 0) {
					message.pushDescriptor(lifetimeFd);
					call->body.lifetime_listener_pipe = 0; // CMSG index
				} else {
					call->body.lifetime_listener_pipe = -1;
				}
				call->body.fd_token = 0;
				// The ingest derives the namespace and registers the process from the SENDER'S
				// credentials, which a datagram carries and a page does not. MEASURED: without this the
				// synthesized call was rejected as coming from a non-existent process (call=0,
				// thread=-1) and the plane reported -ESRCH. The pid is known exactly here: the page is
				// keyed by it, and it is the kernel's own SO_PEERCRED answer for that process's courier
				// connection.
				message.setPID(pid);

				// Dispatch through the ordinary path. doWork() runs the call on this thread's fiber and
				// never blocks its caller (a blocking call suspends and reschedules itself), which is
				// exactly how the RPC ingest runs it -- so the plane reuses the semantics rather than
				// describing them again.
				//
				// perf#30 (doc section 193): an EXCEPTION here is the one way control can leave this block
				// without reaching the completion store, and the store is measured to be missing for a
				// post-exec incarnation while the request IS serviced. So it is caught, named, and turned into
				// a status the caller can act on -- and the completion store then happens instead of vanishing.
				std::shared_ptr<Call> created;
				try {
					created = Call::callFromMessage(std::move(message));
				} catch (const std::exception& e) {
					if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
						static DarlingServer::Log excLog("process-control-exception");
						excLog.error() << "checkin-exception pid=" << pid << " seq=" << seq
							<< " what=" << e.what() << excLog.endLog;
					}
					status = -EIO;
					break;
				} catch (...) {
					if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
						static DarlingServer::Log excLog2("process-control-exception");
						excLog2.error() << "checkin-exception-unknown pid=" << pid << " seq=" << seq << excLog2.endLog;
					}
					status = -EIO;
					break;
				}
				if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
					static DarlingServer::Log pcLog("process-control");
					// perf#30 ATTRIBUTION (doc section 69): the tid the REQUEST named, so it can be intersected
					// with the tid the guest logs when it creates a per-thread RPC socket. That intersection
					// says whether a page request happened at all for the thread-creates that fall back.
					pcLog.error() << "checkin-op pid=" << pid << " req_tid=" << (page->request_payload[1] & 0xffffffffu)
						<< " call=" << (created ? (unsigned)created->number() : 0u)
						<< " thread=" << (created && created->thread() ? (long long)created->thread()->nsid() : -1)
						<< " process=" << (created && created->thread() && created->thread()->process() ? (int)created->thread()->process()->id() : -1)
						// The RPC trace prints nsid as well, and MEASURED it differs by class of process
						// (the first one gets the root namespace id, later ones their own pid). Logging it
						// here is what makes the next run decisive: if the plane's nsid differs from the
						// RPC's for the same pid, the difference is in how the namespace is DERIVED from a
						// caller that has a pid but no socket address -- not in anything else.
						<< " nsid=" << (created && created->thread() && created->thread()->process() ? (long long)created->thread()->process()->nsid() : -1)
						<< pcLog.endLog;
				}
				if (created) {
						created->suppressReplyDelivery();
						if (auto t = created->thread()) {
							// perf#30 (doc section 227): DEFERRED dispatch, behind a hatch so the default path is
							// unchanged while the mechanism is measured. The slot stays PENDING and the server loop
							// runs the Call after this pass returns.
							if (getenv("DARLING_SERVER_PLANE_DEFER_CHECKIN") != NULL) {
								created->setPlaneCompletion(page, seq, pid);
								deferPlaneCall(created);
								planeDeferred = true;
							} else {
								t->noteServicedFromProcessControl();
								t->doWork();
								status = created->suppressedReplyCode();
							}
						} else { status = -ESRCH; }
					} else { status = -ESRCH; }
break;
			}
			case DSERVER_PROCESS_CONTROL_OP_ATTACH_LANE: {
				// perf#30 PHASE-0 ATTACH_LANE: the attach's SEMANTIC half leaves the datagram. The
				// descriptor half already rides the process-scoped courier (kind = LANE_BACKING, exactly
				// what the UDS route uses). Same technique as CHECKIN: build the ORDINARY ring_attach call
				// and run the ordinary path, then read the answer out of the suppressed reply BODY.
				uint64_t fdToken = page->request_payload[2];
				// ProcessControlTxn: the semantic and descriptor halves of a split transport are allowed to
				// arrive in EITHER order. The request is claimed here and, if the descriptor is not there
				// yet, the transaction goes to WAIT_FD -- no reply, no refusal, and no datagram fallback.
				// MEASURED before this: refusing on the descriptor race cost 7 of 151 attaches, and parking
				// without publishing CLAIMED cost the whole boot, because the guest's bounded wait expired
				// and it attached the same lane a second time over UDS.
				auto txnIt = _processControlTxns.find(pid);
				std::shared_ptr<ProcessControlTxn> txn;
				if (txnIt != _processControlTxns.end() && txnIt->second->seq == seq && txnIt->second->op == op) {
					txn = txnIt->second;
				} else {
					txn = std::make_shared<ProcessControlTxn>();
					txn->pid = pid;
					txn->generation = _ringDoorbellGeneration(pid);
					txn->seq = seq;
					txn->op = op;
					txn->fdToken = fdToken;
					txn->expectedFdKind = DSERVER_FD_COURIER_KIND_LANE_BACKING;
					txn->pageMap = region.map;
					txn->pageSize = region.size;
					for (unsigned i = 0; i < 4; ++i) { txn->payload[i] = page->request_payload[i]; }
					_processControlTxns[pid] = txn;
					Metrics::shared().processControlTxnClaimed.fetch_add(1, std::memory_order_relaxed);
					// Publish ownership BEFORE anything else can be waited on: from here the guest must wait
					// for COMPLETION and must not fall back to another transport.
					__atomic_store_n(&page->reply_state, DSERVER_PROCESS_CONTROL_CLAIMED, __ATOMIC_RELEASE);
					__atomic_store_n(&page->futex, seq, __ATOMIC_RELEASE);
					syscall(SYS_futex, &page->futex, FUTEX_WAKE, INT32_MAX, nullptr, nullptr, 0);
				}
				if (txn->state != ProcessControlTxn::State::READY) {
					int laneFd = -1;
					_drainFdCourierMessages();
					// The continuation hands the descriptor to the TRANSACTION, not to a re-queued bundle and
					// not to a close: after courier acceptance the server owns it (pair-and-consume or close).
					auto result = resolveFdCourierBundle(pid, fdToken, DSERVER_FD_COURIER_KIND_LANE_BACKING,
						&laneFd, [txn](int arrivedFd) {
							if (arrivedFd < 0) { return; }
							txn->fd = arrivedFd;
							txn->state = ProcessControlTxn::State::READY;
						});
					if (result == FdCourierResult::Resolved) {
						txn->fd = laneFd;
						txn->state = ProcessControlTxn::State::READY;
					} else {
						txn->state = ProcessControlTxn::State::WAIT_FD;
						if (!txn->waitedForFd) {
							txn->waitedForFd = true;
							Metrics::shared().processControlTxnWaitFd.fetch_add(1, std::memory_order_relaxed);
							/* NAME THE PARK (dar-4cp9). MEASURED: the guest's bounded attach wait expires with
							 * the lane absent from BOTH its table and the process directory, which proves the
							 * server never completed this transaction -- and this branch, the only place a
							 * transaction is parked, said nothing at all about which descriptor it is waiting
							 * for. Without that line 'the courier never delivered' and 'the token did not match'
							 * are the same observation, and they need different fixes. */
							::fprintf(stderr,
								"[srv-attach-waitfd] pid=%d seq=%u op=%u token=%llu kind=%u\n",
								(int)pid, (unsigned)seq, (unsigned)op,
								(unsigned long long)fdToken, (unsigned)DSERVER_FD_COURIER_KIND_LANE_BACKING);
						}
						// Ownership has moved: this pass ends without an answer, and the courier event path
						// (which drains and then services the page) is what resumes the transaction.
						continue;
					}
				}
				const int laneFd = txn->fd;
				/* NAME THE DISPATCH (dar-4cp9). The attach handler could park (now named), refuse (named) or
				 * dispatch the ring_attach Call with the descriptor -- and the last of those was silent, so a
				 * guest that spends its whole bounded wait seeing no completion could not be told apart from a
				 * server that never got a descriptor. This line settles it: it carries the sequence the
				 * completion will be published under and the descriptor the Call is about to receive. */
				/* A LANE WITHOUT A DESCRIPTOR (dar-4cp9). MEASURED: two of five attaches in a failing boot answered
				 * status=0 with token=0 -- a lane record with no backing descriptor -- and exactly those two threads then
				 * repeated negative answers on plane op=7 while the boot watchdog expired. Every other attach in the same
				 * run carried a real token. This line says whether a zero token is a missing descriptor or a failed send,
				 * so the next fix targets the delivery step rather than the layer above it. */
				if (fdToken == 0) {
					::fprintf(stderr, "[srv-attach-notoken] pid=%d seq=%u fd=%d kind=%u\n",
						(int)pid, (unsigned)seq, laneFd, (unsigned)DSERVER_FD_COURIER_KIND_LANE_BACKING);
				}
				::fprintf(stderr, "[srv-attach-dispatch] pid=%d seq=%u fd=%d token=%llu\n",
					(int)pid, (unsigned)seq, laneFd, (unsigned long long)fdToken);
				txn->fd = -1;  // consumed below by pushDescriptor

				Message message(sizeof(dserver_rpc_call_ring_attach_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_ring_attach_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(page->request_payload[0] & 0xffffffffu);
				call->header.number = dserver_callnum_ring_attach;
				call->body.mapping_size = page->request_payload[1];
				call->body.ring_fd = 0; // the CMSG index the ordinary ingest reads
				message.pushDescriptor(laneFd);
				message.setPID(pid);

				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
						// perf#18 D15a: the attach census' caller attribution. MEASURED: its recorder
						// `recordAttachBinaryFact` had NO caller anywhere in the tree, so
						// attach_mldr_callers / attach_dylib_callers were constant zero -- a counter that
						// always answers the same way, which is exactly the kind of reading that let two
						// real defects survive a round. The image identity travels in the page payload
						// (bits 8+), which is the one place this route can learn it.
						Metrics::shared().recordAttachBinaryFact(
							((page->request_payload[3] >> 8) & 0xffu) == 1u, true);
						size_t bodyLen = 0;
						const uint8_t* body = created->suppressedReplyBody(&bodyLen);
						if (body && bodyLen >= sizeof(dserver_rpc_reply_ring_attach_t) - sizeof(dserver_rpc_replyhdr_t)) {
							auto* rb = reinterpret_cast<const dserver_rpc_reply_ring_attach_t*>(
								body - sizeof(dserver_rpc_replyhdr_t));
							page->reply_payload[0] = rb->body.reject_reason;
							page->reply_payload[1] = rb->body.fd_token;
							// The reply body's token is only filled by the generated DATAGRAM reply; on
							// the page route there is no such thing, so when the reply owned a descriptor
							// the server sends it on the courier here and reports the token through the
							// page. MEASURED before this: every plane attach reported wake=-1, i.e. the
							// ONE doorbell delivery never reached the page route at all.
							int wakeFd = created->suppressedReplyWakeFd();
							if (page->reply_payload[1] == 0 && wakeFd >= 0) {
								uint64_t tok = sendFdCourierBundleToGuest(pid,
									DSERVER_FD_COURIER_KIND_PROCESS_DOORBELL, wakeFd);
								if (tok != 0) {
									page->reply_payload[1] = tok;
									wakeFd = -1; // ownership moved to the courier
								}
							}
							if (wakeFd >= 0) {
								close(wakeFd);
							}
						} else {
							status = -EIO;
						}
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				{
					static DarlingServer::Log pcLog("process-control");
					pcLog.error() << "attach-lane-op pid=" << pid << " tid=" << (long long)page->request_payload[0]
						<< " size=" << page->request_payload[1] << " status=" << status
						<< " reject=" << page->reply_payload[0] << " token=" << page->reply_payload[1]
						<< pcLog.endLog;
				}
				// COMMITTED: the operation is durably done. The transaction is retired here, and the reply
				// write below is guarded by the page identity it was claimed against.
				Metrics::shared().processControlTxnCompleted.fetch_add(1, std::memory_order_relaxed);
				_processControlTxns.erase(pid);
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_CHECKOUT: {
				// perf#30 CHECKOUT ON THE PAGE: the execve path's teardown call. Same two-half shape as
				// everything else on this page -- the exec listener pipe rides the process courier with
				// kind = CHECKOUT_FD, and the page carries the semantics and the completion. MEASURED: this
				// operation was the largest remaining semantic UDS user (153 datagrams per run).
				uint64_t fdToken = page->request_payload[2];
				int execFd = -1;
				_drainFdCourierMessages();
				if (fdToken != 0) {
					auto result = resolveFdCourierBundle(pid, fdToken, DSERVER_FD_COURIER_KIND_CHECKOUT_FD,
						&execFd, [](int arrivedFd) { if (arrivedFd >= 0) { close(arrivedFd); } });
					if (result != FdCourierResult::Resolved) {
						status = -EBADF;
						break;
					}
				}

				Message message(sizeof(dserver_rpc_call_checkout_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_checkout_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(page->request_payload[1] & 0xffffffffu);
				call->header.number = dserver_callnum_checkout;
				call->body.executing_macho = (page->request_payload[0] & 1u) != 0;
				if (execFd >= 0) {
					message.pushDescriptor(execFd);
					call->body.exec_listener_pipe = 0;  // CMSG index
				} else {
					call->body.exec_listener_pipe = -1;
				}
				call->body.fd_token = 0;
				message.setPID(pid);

				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_SET_DYLD_INFO: {
				// perf#30 (doc section 216): DIRECT servicing, no synthesized Call. MEASURED: building an ordinary Call and
				// running it with doWork() on the process's own thread deadlocked for this request -- that thread is the
				// one parked waiting for THIS reply, so the plane pass waited on work that waited on the pass, and the
				// loader spun (`iter=20000 waited=0 reply=0`). The semantics are not reimplemented: this calls the very
				// core `Call::SetDyldInfo::processCall` calls, reached through the Process, exactly as
				// `pthread_canceled` already reaches `Thread::pthreadCanceled`.
				status = -ESRCH;
				// MEASURED: with the NSID key alone this answered -ESRCH and the loader reported "Failed to tell
				// darlingserver about our dyld info" -- the request no longer HANGS (the shape is right) but the
				// process was not found under that key, so both keys the registry offers are tried here, and the
				// namespace one first because that is what the envelope names.
				{
					auto maybeProcess = processRegistry().lookupEntryByNSID(static_cast<int>(pid));
					if (!maybeProcess) {
						maybeProcess = processRegistry().lookupEntryByID(static_cast<Process::ID>(pid));
					}
					if (maybeProcess) {
						if (auto proc = *maybeProcess) {
							proc->setDyldInfo(page->request_payload[0], page->request_payload[1]);
							status = 0;
						}
					}
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_SET_EXECUTABLE_PATH: {
				// perf#30 (doc section 230): DIRECT servicing, no synthesized Call -- the same defect section 216
				// measured for SET_DYLD_INFO and the same remedy. MEASURED: this handler built an ordinary Call and ran
				// it with doWork() on the process's OWN thread, but the requester of this op is the LOADER, which is
				// spinning in the plane loop itself rather than parked in a server RPC wait. The call therefore went
				// into that thread's pending slot and could never run, no completion was ever stored, and the loader
				// blocked in its unbounded futex wait: the diagnostic log ends at `iter=20000 waited=0 reply=0 seq=3`
				// and never prints another heartbeat, because `waited_ms` never advances past the first wait.
				//
				// The semantics need a Process and nothing else: SetExecutablePath::processCall reads the string
				// through the process's memory interface and stores it on the Process, so this calls the very same
				// memory read and the very same setter, exactly as the SET_DYLD_INFO case reaches the core that
				// Call::SetDyldInfo::processCall calls. The path does not fit the four payload words, so it still
				// travels as a guest pointer in the page -- the plane needs no string channel for it.
				status = -ESRCH;
				{
					// Both registry keys are tried, the namespace one first, for the reason recorded on the
					// SET_DYLD_INFO case: the envelope names the namespace identity and a process outside the root
					// namespace is keyed by its own id.
					auto maybeProcess = processRegistry().lookupEntryByNSID(static_cast<int>(pid));
					if (!maybeProcess) {
						maybeProcess = processRegistry().lookupEntryByID(static_cast<Process::ID>(pid));
					}
					if (maybeProcess) {
						if (auto proc = *maybeProcess) {
							const uint64_t buffer = page->request_payload[0];
							const uint64_t bufferSize = page->request_payload[2];
							int code = 0;
							std::string tmpstr;
							tmpstr.resize((size_t)bufferSize);
							if (!proc->readMemory((uintptr_t)buffer, tmpstr.data(), (size_t)bufferSize, &code)) {
								status = -code;
							} else {
								std::string path = tmpstr.c_str();
								proc->setExecutablePath(path);
								if (TestDiagnostics::enabled()) {
									TestDiagnostics::traceExecutablePath(static_cast<int32_t>(pid),
										static_cast<int32_t>(pid), path, 0);
								}
								status = 0;
							}
						}
					}
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_ALLOCATE: {
				// perf#30 (directive section 2 / doc section 112): early Mach port operation -- the plane is its
				// home, the server runs the ORDINARY Call, and the semantics exist once (directive section 15).
				Message message(sizeof(dserver_rpc_call_mach_port_allocate_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_allocate_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_allocate;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.right = static_cast<int32_t>(static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu));
				call->body.name = page->request_payload[2];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_EXTRACT_MEMBER: {
				// perf#30 (directive section 2 / doc section 112): early Mach port operation -- the plane is its
				// home, the server runs the ORDINARY Call, and the semantics exist once (directive section 15).
				Message message(sizeof(dserver_rpc_call_mach_port_extract_member_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_extract_member_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_extract_member;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.pset = static_cast<uint32_t>(page->request_payload[2] & 0xffffffffu);
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_GUARD: {
				// perf#30 (directive section 2 / doc section 112): early Mach port operation -- the plane is its
				// home, the server runs the ORDINARY Call, and the semantics exist once (directive section 15).
				Message message(sizeof(dserver_rpc_call_mach_port_guard_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_guard_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_guard;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.guard = page->request_payload[2];
				call->body.strict = (page->request_payload[3] & 0xffffffffu) != 0;
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_INSERT_MEMBER: {
				// perf#30 (directive section 2 / doc section 112): early Mach port operation -- the plane is its
				// home, the server runs the ORDINARY Call, and the semantics exist once (directive section 15).
				Message message(sizeof(dserver_rpc_call_mach_port_insert_member_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_insert_member_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_insert_member;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.pset = static_cast<uint32_t>(page->request_payload[2] & 0xffffffffu);
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_INSERT_RIGHT: {
				// perf#30 (directive section 2 / doc section 112): early Mach port operation -- the plane is its
				// home, the server runs the ORDINARY Call, and the semantics exist once (directive section 15).
				Message message(sizeof(dserver_rpc_call_mach_port_insert_right_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_insert_right_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_insert_right;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.poly = static_cast<uint32_t>(page->request_payload[2] & 0xffffffffu);
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_MOD_REFS: {
				// perf#30 (directive section 2 / doc section 112): early Mach port operation -- the plane is its
				// home, the server runs the ORDINARY Call, and the semantics exist once (directive section 15).
				Message message(sizeof(dserver_rpc_call_mach_port_mod_refs_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_mod_refs_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_mod_refs;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.right = static_cast<int32_t>(static_cast<uint32_t>(page->request_payload[2] & 0xffffffffu));
				call->body.delta = static_cast<int32_t>(static_cast<uint32_t>(page->request_payload[3] & 0xffffffffu));
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_TYPE: {
				// perf#30 (directive section 2 / doc section 112): early Mach port operation -- the plane is its
				// home, the server runs the ORDINARY Call, and the semantics exist once (directive section 15).
				Message message(sizeof(dserver_rpc_call_mach_port_type_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_type_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_type;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.ptype = page->request_payload[2];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_REQUEST_NOTIFICATION: {
				// perf#30 (directive section 1, doc section 206): the hard oracle named this call as the first
				// per-thread-socket dependency, reached from the main thread before a lane can be relied on. It is
				// an EARLY Mach port operation, so the process management plane is its home and the server runs the
				// ORDINARY Call (directive section 15: one semantic core). Two request words travel per payload
				// word; `previous` is an OUT parameter and travels as the guest ADDRESS the Call writes through
				// writeMemory, exactly as the datagram path returned it.
				Message message(sizeof(dserver_rpc_call_mach_port_request_notification_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_request_notification_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_request_notification;
				const uint64_t w0 = page->request_payload[0];
				const uint64_t w1 = page->request_payload[1];
				const uint64_t w2 = page->request_payload[2];
				call->body.target = static_cast<uint32_t>(w0 & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(w0 >> 32);
				call->body.msgid = static_cast<int32_t>(static_cast<uint32_t>(w1 & 0xffffffffu));
				call->body.sync = static_cast<uint32_t>(w1 >> 32);
				call->body.notify = static_cast<uint32_t>(w2 & 0xffffffffu);
				call->body.notifyPoly = static_cast<uint32_t>(w2 >> 32);
				call->body.previous = page->request_payload[3];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_UNGUARD: {
				// perf#30 (directive section 2 / doc section 112): early Mach port operation -- the plane is its
				// home, the server runs the ORDINARY Call, and the semantics exist once (directive section 15).
				Message message(sizeof(dserver_rpc_call_mach_port_unguard_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_unguard_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_unguard;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.guard = page->request_payload[2];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_SEMAPHORE_SIGNAL: {
				// perf#30 (doc section 245): DIRECT, NO CALL. MEASURED: synthesizing the ordinary Call and running it on
				// the process's own thread aborted the server -- that thread is very often parked in a blocking
				// receive and the state machine refused the assignment ("Thread has both a pending call and a pending
				// continuation, pending_call=58 ... active_call=38"), after which every RPC the guest had outstanding
				// was never answered and the run looked like a transport hang. A signal is a state mutation on a
				// semaphore named in this process's space, so the plane performs it here and touches no thread.
				status = -ESRCH;
				{
					auto maybeProcess = processRegistry().lookupEntryByNSID(static_cast<int>(pid));
					if (!maybeProcess) {
						maybeProcess = processRegistry().lookupEntryByID(static_cast<Process::ID>(pid));
					}
					if (maybeProcess) {
						if (auto proc = *maybeProcess) {
							// The trap's own return code (0 = KERN_SUCCESS) is the answer the guest turns into `code`.
							status = proc->signalSemaphore(
								static_cast<unsigned int>(page->request_payload[0] & 0xffffffffu), false);
						}
					}
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_SEMAPHORE_SIGNAL_ALL: {
				// perf#30 (doc section 245): the same direct path as the single-name signal above.
				status = -ESRCH;
				{
					auto maybeProcess = processRegistry().lookupEntryByNSID(static_cast<int>(pid));
					if (!maybeProcess) {
						maybeProcess = processRegistry().lookupEntryByID(static_cast<Process::ID>(pid));
					}
					if (maybeProcess) {
						if (auto proc = *maybeProcess) {
							status = proc->signalSemaphore(
								static_cast<unsigned int>(page->request_payload[0] & 0xffffffffu), true);
						}
					}
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_VCHROOT: {
				// perf#30 (directive section 14/15): the early-lifecycle vchroot setup. Same technique as
				// SET_EXECUTABLE_PATH -- the guest's pointer and size travel in the payload and the server runs
				// the ORDINARY Call, so there is exactly one implementation of the semantics and this is only a
				// transport adapter. Vchroot::processCall reads the string through the process's own memory
				// interface, so no string channel is needed on the page.
				Message message(sizeof(dserver_rpc_call_vchroot_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_vchroot_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_vchroot;
				call->body.path = page->request_payload[0];
				call->body.path_size = page->request_payload[1];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_VCHROOT_PATH: {
				// perf#30 (doc section 202): the QUERY counterpart of op 8. Same adapter shape as the neighbouring
				// early-lifecycle ops: the guest's buffer pointer and size travel in the payload and the server
				// runs the ORDINARY call, so the semantics -- including writing the path through the process's own
				// memory interface -- have exactly one implementation. The reply's length is this op's status.
				Message message(sizeof(dserver_rpc_call_vchroot_path_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_vchroot_path_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_vchroot_path;
				call->body.buffer = page->request_payload[0];
				call->body.buffer_size = page->request_payload[1];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
						if (status >= 0) {
							size_t bodyLen = 0;
							const uint8_t* body = created->suppressedReplyBody(&bodyLen);
							if (body && bodyLen >= sizeof(dserver_reply_vchroot_path_t)) {
								auto* rb = reinterpret_cast<const dserver_reply_vchroot_path_t*>(body);
								status = (int32_t)rb->length;
							}
						}
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_DESTRUCT: {
				// perf#30 (directive section 2 / doc section 111): same technique as VCHROOT. All four
				// parameters fill the payload words. `guard` is an arbitrary 64-bit value rather than a
				// pointer, so the architecture byte is NOT packed into it -- the synthesized Call carries no
				// architecture for this op, as it carries none for the other early operations.
				Message message(sizeof(dserver_rpc_call_mach_port_destruct_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_destruct_t*>(message.data().data());
				*call = {};
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_destruct;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.name = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.srdelta = static_cast<int32_t>(static_cast<uint32_t>(page->request_payload[2] & 0xffffffffu));
				call->body.guard = page->request_payload[3];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_CONSTRUCT: {
				// perf#30 (directive section 2 / doc section 110): same technique as VCHROOT. The four
				// parameters fill the four payload words and the architecture byte rides in the TOP byte of
				// payload[3] -- `options` is a pointer, so its high byte is zero -- exactly the convention
				// kqchan_mach_port_open uses. The server runs the ORDINARY MachPortConstruct Call.
				Message message(sizeof(dserver_rpc_call_mach_port_construct_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_construct_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] >> 56);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_construct;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.options = page->request_payload[3] & 0x00ffffffffffffffull;
				call->body.context = page->request_payload[1];
				call->body.name = page->request_payload[2];
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_PTHREAD_CANCELED: {
				// perf#30 (directive D2 / doc section 159): DIRECT servicing, no synthesized Call.
				//
				// This operation's target IS the caller, and the caller is suspended inside the very syscall
				// that is asking (CANCELATION_POINT inside open()). Attaching a second request to that
				// microthread therefore deadlocks by construction -- MEASURED:
				//   "Thread has both a pending call and a pending continuation
				//    (pending_call=31 tid=... suspended=1 interrupts=0)"
				// Deferral cannot fix it (the caller never becomes idle: it is waiting for THIS answer), so
				// the semantics run here, directly, through the same Thread::pthreadCanceled that the ordinary
				// Call uses. One semantic core, two transports.
				//
				// The thread is resolved from the EXPLICIT request_tid published with the request, looked up
				// INSIDE the requesting process (never globally by tid, and never the old `tid = pid`
				// substitute, which named a different thread entirely). A missing thread is -ESRCH: a request
				// must never be serviced against whatever thread the pid happened to resolve to.
				int32_t wantTid = __atomic_load_n(&page->request_tid, __ATOMIC_ACQUIRE);
				int action = (int)(int32_t)(uint32_t)(page->request_payload[0] & 0xffffffffu);
				status = -ESRCH;
				if (wantTid > 0) {
					auto found = threadRegistry().lookupEntryByNSID((Thread::NSID)wantTid);
					if (found.has_value()) {
						auto target = *found;
						auto owner = target ? target->process() : nullptr;
						if (target && owner && owner->id() == (Process::ID)pid) {
							status = target->pthreadCanceled(action);
						} else if (target) {
							// the tid belongs to a thread of ANOTHER process: refuse, do not service
							if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
								static DarlingServer::Log wrongLog("plane-wrong-owner");
								wrongLog.error() << "pthread_canceled-refused pid=" << pid
									<< " request_tid=" << wantTid << wrongLog.endLog;
							}
						}
					}
				}
				if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
					static DarlingServer::Log pcLog("pthread-canceled-direct");
					pcLog.error() << "pthread_canceled-direct pid=" << pid << " request_tid=" << wantTid
						<< " action=" << action << " status=" << status << pcLog.endLog;
				}
				break;
			}

			case DSERVER_PROCESS_CONTROL_OP_PTHREAD_KILL: {
				// perf#30 REMOVAL STEP 2: the ordinary PThreadKill Call's WHOLE semantics is
				// `Thread::threadForPort(port)` + `Thread::sendSignal(signal)` + the reply code, and sendSignal is a
				// host tgkill -- so it is serviced here DIRECTLY, exactly as pthread_canceled is, and needs neither a
				// lane nor a per-thread socket. The port name and the signal travel in the payload, and the target is
				// resolved the way the ordinary Call resolves it: by the PORT. An unknown port is -ESRCH; the request
				// is never serviced against whatever thread the pid happened to resolve to.
				int32_t killPort = (int32_t)(uint32_t)(page->request_payload[0] & 0xffffffffu);
				int32_t killSignal = (int32_t)(uint32_t)(page->request_payload[1] & 0xffffffffu);
				status = -ESRCH;
				if (auto targetThread = Thread::threadForPort(killPort)) {
					try {
						targetThread->sendSignal(killSignal);
						status = 0;
					} catch (std::system_error e) {
						status = -e.code().value();
					}
				}
				if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
					static DarlingServer::Log pkLog("pthread-kill-direct");
					pkLog.error() << "pthread_kill-direct pid=" << pid << " port=" << killPort
						<< " signal=" << killSignal << " status=" << status << pkLog.endLog;
				}
				break;
			}

			case DSERVER_PROCESS_CONTROL_OP_MACH_PORT_MOVE_MEMBER: {
				// perf#30 (directive section 2 / doc sections 107/108): same technique as VCHROOT and
				// SET_EXECUTABLE_PATH -- the three port names travel in the payload and the server runs the
				// ORDINARY MachPortMoveMember Call, so there is exactly one implementation of the semantics
				// and this is only a transport adapter. No descriptor is involved.
				Message message(sizeof(dserver_rpc_call_mach_port_move_member_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_mach_port_move_member_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] & 0xffu);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_mach_port_move_member;
				call->body.target = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.member = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				call->body.after = static_cast<uint32_t>(page->request_payload[2] & 0xffffffffu);
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_KQCHAN_MACH_PORT_OPEN: {
				// perf#30 (directive section 16): the kqueue-channel class. The SEMANTIC half is the ordinary
				// Call -- one implementation of the semantics -- and the descriptor the call RETURNS travels on
				// the process courier with its token published in reply_payload[1], exactly as ATTACH_LANE
				// returns the doorbell. The guest resolves the token with the courier receive.
				Message message(sizeof(dserver_rpc_call_kqchan_mach_port_open_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_kqchan_mach_port_open_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] >> 56);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_kqchan_mach_port_open;
				call->body.port_name = static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu);
				call->body.receive_buffer = page->request_payload[1];
				call->body.receive_buffer_size = page->request_payload[2];
				call->body.saved_filter_flags = page->request_payload[3] & 0x00ffffffffffffffull;
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
						int returnedFd = created->suppressedReplyWakeFd();
						// perf#30 (doc section 98): record BOTH outcomes. This call alternates between
						// delivering a descriptor and reporting success with none, so the instrument must name
						// which branch produced it (wake fd, reply body, or neither) and what the call's own
						// status was -- otherwise only the failing half of the question is answered.
						int fromWake = returnedFd;
						// perf#30 (doc section 91): kqchan returns its descriptor in the reply BODY, not as a
						// wake descriptor, so read it there too -- the same place ATTACH_LANE reads its body.
						size_t bodyLen = 0;
						int bodySocketSeen = -1;
						{
							const uint8_t* body = created->suppressedReplyBody(&bodyLen);
							if (body && bodyLen >= sizeof(dserver_reply_kqchan_mach_port_open_t)) {
								auto* rb = reinterpret_cast<const dserver_reply_kqchan_mach_port_open_t*>(body);
								bodySocketSeen = rb->socket;
								// perf#30 (doc sections 100-116): when the sender could NOT use the courier it
								// fell back to CMSG and wrote the **CMSG INDEX** into body.socket, not a
								// descriptor -- MEASURED as `socket=0`, which this code then treated as fd 0 and
								// attempted to re-send, leaving fd 0 to be closed. A suppressed reply has no
								// transport to carry a CMSG, so that fallback is unusable here and must not be
								// read as a descriptor. Only a real token is a descriptor transfer.
								if (rb->fd_token != 0) {
									page->reply_payload[1] = rb->fd_token;
									returnedFd = -1; // the sender already moved ownership to the courier
								} else {
									// The descriptor (if any) went to a transport the plane caller cannot read.
									// Recorded instead of guessed: sending fd 0 or closing it corrupts the
									// service, and the real fault is named by the courier log.
									Metrics::shared().fdCourierFallbackCmsg.fetch_add(1, std::memory_order_relaxed);
									if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
										static DarlingServer::Log kqMiss("kqchan-plane-miss");
										kqMiss.error() << "kqchan-plane-no-token pid=" << pid
											<< " status=" << status << " body_socket_index=" << bodySocketSeen
											<< " body_len=" << bodyLen << kqMiss.endLog;
									}
									returnedFd = -1; // never treat a CMSG index as a descriptor
								}
							}
						}
						if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
							static DarlingServer::Log kqLog("kqchan-branch");
							kqLog.error() << "kqchan-open pid=" << pid << " status=" << status
								<< " from_wake=" << fromWake << " body_len=" << bodyLen
								<< " body_socket=" << (bodySocketSeen) << kqLog.endLog;
						}
						if (returnedFd >= 0) {
							uint64_t tok = sendFdCourierBundleToGuest(pid,
								DSERVER_FD_COURIER_KIND_KQCHAN_FD, returnedFd);
							if (tok != 0) {
								page->reply_payload[1] = tok;
								returnedFd = -1; // ownership moved to the courier
							} else {
								/* NEVER CLAIM SUCCESS WITHOUT THE DESCRIPTOR THE CALL PROMISES (dar-4cp9). MEASURED on the
								 * residual boot stall: the guest's [kqchan-plane] line reported status=0 token=0 fd=-1, i.e. this
								 * handler answered success while the courier returned no token, so the guest returned success with
								 * its out_socket still unset and launchd's channel was never created -- no denial, no signal, no
								 * registration on the server, just a launcher waiting for a socket that cannot appear. The same
								 * shape existed for every descriptor-bearing plane route, so the status now matches reality:
								 * the completion is an error, and the descriptor is closed below as before. */
								static DarlingServer::Log tokLog("plane-token");
								tokLog.error() << "plane-token-missing kind=KQCHAN_FD pid=" << pid
									<< " fd=" << returnedFd << " status=-EIO (descriptor courier returned no token)" << tokLog.endLog;
								status = -EIO;
							}
						}
						if (returnedFd >= 0) {
							close(returnedFd);
						}
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_KQCHAN_PROC_OPEN: {
				// perf#30 (directive sections 16-17, doc section 206): the kqueue-channel class for a PROCESS, the
				// same shape as KQCHAN_MACH_PORT_OPEN (9u). The SEMANTIC half is the ordinary Call -- one
				// implementation of the semantics -- and the descriptor it RETURNS travels on the process courier
				// with its token published in reply_payload[1]. The completion status is carried by reply_status,
				// so a real failure is a failure and a missing transport is not mistaken for one.
				Message message(sizeof(dserver_rpc_call_kqchan_proc_open_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_kqchan_proc_open_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] >> 56);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_kqchan_proc_open;
				call->body.pid = static_cast<int32_t>(static_cast<uint32_t>(page->request_payload[0] & 0xffffffffu));
				call->body.flags = static_cast<uint32_t>(page->request_payload[1] & 0xffffffffu);
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
						int returnedFd = created->suppressedReplyWakeFd();
						size_t bodyLen = 0;
						const uint8_t* body = created->suppressedReplyBody(&bodyLen);
						if (body && bodyLen >= sizeof(dserver_reply_kqchan_proc_open_t)) {
							auto* rb = reinterpret_cast<const dserver_reply_kqchan_proc_open_t*>(body);
							// Only a real token is a descriptor transfer: a suppressed reply has no transport to
							// carry a CMSG, so an index written there must never be read as a descriptor.
							if (rb->fd_token != 0) {
								page->reply_payload[1] = rb->fd_token;
								returnedFd = -1;
							}
						}
						if (returnedFd >= 0) {
							uint64_t tok = sendFdCourierBundleToGuest(pid,
								DSERVER_FD_COURIER_KIND_KQCHAN_FD, returnedFd);
							if (tok != 0) {
								page->reply_payload[1] = tok;
								returnedFd = -1; // ownership moved to the courier
							} else {
								/* NEVER CLAIM SUCCESS WITHOUT THE DESCRIPTOR THE CALL PROMISES (dar-4cp9). MEASURED on the
								 * residual boot stall: the guest's [kqchan-plane] line reported status=0 token=0 fd=-1, i.e. this
								 * handler answered success while the courier returned no token, so the guest returned success with
								 * its out_socket still unset and launchd's channel was never created -- no denial, no signal, no
								 * registration on the server, just a launcher waiting for a socket that cannot appear. The same
								 * shape existed for every descriptor-bearing plane route, so the status now matches reality:
								 * the completion is an error, and the descriptor is closed below as before. */
								static DarlingServer::Log tokLog("plane-token");
								tokLog.error() << "plane-token-missing kind=KQCHAN_FD pid=" << pid
									<< " fd=" << returnedFd << " status=-EIO (descriptor courier returned no token)" << tokLog.endLog;
								status = -EIO;
							}
						}
						if (returnedFd >= 0) {
							close(returnedFd);
						}
						page->reply_payload[0] = (uint64_t)(uint32_t)(int32_t)status;
						if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
							static DarlingServer::Log kqpLog("kqchan-proc-plane");
							kqpLog.error() << "kqchan-proc-plane pid=" << pid << " status=" << status
								<< " token=" << page->reply_payload[1] << kqpLog.endLog;
						}
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_CONSOLE_OPEN: {
				// perf#30 (directive sections 2/16/17): /dev/console is not a file in the guest, it is the
				// ordinary ConsoleOpen Call -- one implementation of the semantics -- and the descriptor it
				// RETURNS travels on the process courier with its token in reply_payload[1], exactly as
				// KQCHAN_MACH_PORT_OPEN does. The completion status is carried by reply_status (what the
				// guest sees as the plane call's return), so a real failure is reported as a failure and a
				// missing transport is not mistaken for one.
				Message message(sizeof(dserver_rpc_call_console_open_t), 0);
				auto* call = reinterpret_cast<dserver_rpc_call_console_open_t*>(message.data().data());
				*call = {};
				call->header.architecture = static_cast<dserver_rpc_architecture_t>(page->request_payload[3] >> 56);
				call->header.pid = static_cast<int32_t>(pid);
				call->header.tid = static_cast<int32_t>(pid);  // PROCESS_SCOPED: the process's own thread executes it
				call->header.number = dserver_callnum_console_open;
				message.setPID(pid);
				auto created = Call::callFromMessage(std::move(message));
				if (created) {
					created->suppressReplyDelivery();
					if (auto t = created->thread()) {
						t->noteServicedFromProcessControl();
						t->doWork();
						status = created->suppressedReplyCode();
						int returnedFd = created->suppressedReplyWakeFd();
						size_t bodyLen = 0;
						const uint8_t* body = created->suppressedReplyBody(&bodyLen);
						if (body && bodyLen >= sizeof(dserver_reply_console_open_t)) {
							auto* rb = reinterpret_cast<const dserver_reply_console_open_t*>(body);
							// Only a real token is a descriptor transfer. A suppressed reply has no transport
							// to carry a CMSG, so an index written there must never be read as a descriptor.
							if (rb->fd_token != 0) {
								page->reply_payload[1] = rb->fd_token;
								returnedFd = -1;
							}
						}
						if (returnedFd >= 0) {
							uint64_t tok = sendFdCourierBundleToGuest(pid,
								DSERVER_FD_COURIER_KIND_CONSOLE_FD, returnedFd);
							if (tok != 0) {
								page->reply_payload[1] = tok;
								returnedFd = -1; // ownership moved to the courier
							} else {
								/* NEVER CLAIM SUCCESS WITHOUT THE DESCRIPTOR THE CALL PROMISES (dar-4cp9). MEASURED on the
								 * residual boot stall: the guest's [kqchan-plane] line reported status=0 token=0 fd=-1, i.e. this
								 * handler answered success while the courier returned no token, so the guest returned success with
								 * its out_socket still unset and launchd's channel was never created -- no denial, no signal, no
								 * registration on the server, just a launcher waiting for a socket that cannot appear. The same
								 * shape existed for every descriptor-bearing plane route, so the status now matches reality:
								 * the completion is an error, and the descriptor is closed below as before. */
								static DarlingServer::Log tokLog("plane-token");
								tokLog.error() << "plane-token-missing kind=CONSOLE_FD pid=" << pid
									<< " fd=" << returnedFd << " status=-EIO (descriptor courier returned no token)" << tokLog.endLog;
								status = -EIO;
							}
						}
						if (returnedFd >= 0) {
							close(returnedFd);
						}
						// The guest reads the RESULT from the reply payload and the STATUS from the call's
						// return, so the descriptor number (or the failing status) must be published here.
						// MEASURED contract (dserver-ring.c: return (int)seenStatus, outReply0 = payload):
						// leaving payload[0] at its cleared zero would hand the guest fd 0.
						page->reply_payload[0] = (uint64_t)(uint32_t)(int32_t)status;
						if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
							static DarlingServer::Log conLog("console-plane");
							conLog.error() << "console-plane pid=" << pid << " status=" << status
								<< " token=" << page->reply_payload[1] << conLog.endLog;
						}
					} else {
						status = -ESRCH;
					}
				} else {
					status = -ESRCH;
				}
				break;
			}
			case DSERVER_PROCESS_CONTROL_OP_PING:
				// The probe that makes the plane believable end to end: a request and its reply cross
				// through shared memory only -- no datagram, no lane. It ALSO teaches the incarnation's
				// generation when the courier drain has not seen this process yet, which is exactly the
				// window the first attach lives in.
				if (page->request_payload[3] != 0) {
					auto gen = _fdCourierGeneration.find(pid);
					if (gen == _fdCourierGeneration.end() || gen->second == 0) {
						_fdCourierGeneration[pid] = page->request_payload[3];
					}
				}
				status = 0;
				break;
			default:
				status = -ENOSYS;
				break;
		}
		// incarnation-safe completion: never write a late answer into a different page incarnation, a reused
		// mapping, or a later request that happens to reuse this slot.
		if (regionMap != page) {
			Metrics::shared().processControlLateReplyRetired.fetch_add(1, std::memory_order_relaxed);
			// perf#30 (doc section 192): say WHEN a completion is retired. The completion path logs its reply
			// between this guard and the stores, so a retired reply is the one case where the log prints and no
			// store follows -- and that is indistinguishable from "the store is invisible" without this line.
			if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
				static DarlingServer::Log lateLog("process-control-late");
				lateLog.error() << "late-reply-retired pid=" << pid << " op=" << op << " seq=" << seq
					<< " page=" << (void*)page << " region_map=" << (void*)regionMap << lateLog.endLog;
			}
			continue;
		}
		if (planeDeferred) {
			// the Call has not finished: pushCallReply writes its completion; writing one here would hand the
			// waiter a status for work that has not run.
			continue;
		}
		// perf#30 ATTRIBUTION: the DECISIVE line for the duplicate-checkin finding (doc section 57). The
		// guest accepts a page-route checkin only when the published status is 0 (or when the server claimed
		// the transaction), so if these statuses are non-zero the guest's repetition on the datagram is
		// explained exactly; if they are zero, the defect is in the guest's completion wait instead.
		if (op == DSERVER_PROCESS_CONTROL_OP_CHECKIN && getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log ciReplyLog("checkin-reply");
			ciReplyLog.error() << "checkin-reply pid=" << pid << " seq=" << seq << " status=" << status << ciReplyLog.endLog;
		}
		// perf#30 (doc section 197): a separator per STATEMENT, because the log immediately above prints for the
		// second incarnation's checkin and the store below does not run -- and there is no conditional statement
		// between them. A marker after each one turns "somewhere in these four lines" into a single line.
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log markLog("process-control-marks");
			markLog.error() << "completion-mark A pid=" << pid << " op=" << op << " seq=" << seq << markLog.endLog;
		}
		// DISCARD A COMPLETION FOR A GENERATION THAT NO LONGER OWNS THE SLOT (dar-b5pe, case G). Once a stale claim
		// can be reclaimed by a newer generation, publishing this answer anyway would write DONE over that newer
		// request and strand it -- the same harm as a client release stealing a claimed slot, arriving from the
		// other end. If the slot no longer carries the sequence this completion answers, the answer is dropped and
		// counted instead of published.
		if (page->request_seq != seq) {
			Metrics::shared().processControlRequests.fetch_add(1, std::memory_order_relaxed);
			if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
				static DarlingServer::Log staleLog("process-control-stale-completion");
				staleLog.error() << "completion-discarded pid=" << pid << " op=" << op
					<< " seq=" << seq << " slot_seq=" << page->request_seq << staleLog.endLog;
			}
			continue;
		}
		page->reply_seq = seq;
		page->reply_status = status;
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log markLogB("process-control-marks");
			markLogB.error() << "completion-mark B pid=" << pid << " op=" << op << markLogB.endLog;
		}
		region.requests++;
		Metrics::shared().processControlRequests.fetch_add(1, std::memory_order_relaxed);
		__atomic_store_n(&page->reply_state, DSERVER_PROCESS_CONTROL_DONE, __ATOMIC_RELEASE);
		// perf#30 (doc section 192): READ IT BACK, from the same mapping, in the same process, immediately after
		// the store. This separates "the store ran and the other side cannot see it" from "the store did not run
		// here" -- the two explanations the outside read of the guest's page could not tell apart.
		if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
			static DarlingServer::Log storeLog("process-control-store");
			storeLog.error() << "reply-stored pid=" << pid << " op=" << op << " seq=" << seq
				<< " readback=" << __atomic_load_n(&page->reply_state, __ATOMIC_ACQUIRE)
				<< " reply_seq=" << page->reply_seq << " map=" << (void*)page
				<< " region_map=" << (void*)regionMap << storeLog.endLog;
		}
		// perf#30 MAILBOX SLOT OWNERSHIP (doc sections 59/67): the slot is NOT released to IDLE here -- it
		// belongs to the publisher until the publisher has READ its own answer, and releasing it on publish
		// let a second thread of the same process claim it and publish its sequence before the first had
		// read (the foreign-reply_seq refusal that made 27 thread-create checkins fall back and create a
		// socket). But it must not stay PENDING either: MEASURED RED -- the loop services a page whenever it
		// reads PENDING, so leaving it PENDING made the server RE-SERVICE the same request and publish its
		// old sequence again, and the next bootstrap write then read a foreign `reply_seq` and failed with
		// -2 ("Failed to tell darlingserver about our executable path"). The serviced state is therefore
		// DONE: the loop no longer re-services it, and the guest's DSERVER_PROCESS_CONTROL_RELEASE sets IDLE
		// after it has read the answer.
		// perf#30 (doc section 195): the slot state is written HERE and the guest must therefore treat DONE as
		// claimable: this store races the guest's own RELEASE (which sets IDLE), and when it wins the slot stays
		// at DONE forever. MEASURED as `seq=3 after-dyld status=-3` with the guest's own "Failed to tell
		// darlingserver about our dyld info"; removing the store entirely broke the boot worse (four log lines),
		// so the pair of changes is: keep the store, and let the claim accept DONE.
		__atomic_store_n(&page->request_state, DSERVER_PROCESS_CONTROL_DONE, __ATOMIC_RELEASE);
		// The guest may be waiting on the page's futex: wake it. Best effort -- the guest re-reads the
		// state, so a lost wake costs latency, never correctness.
		__atomic_store_n(&page->futex, seq, __ATOMIC_RELEASE);
		syscall(SYS_futex, &page->futex, FUTEX_WAKE, INT32_MAX, nullptr, nullptr, 0);
	}
};

void DarlingServer::Server::_cancelProcessControlTxn(pid_t pid) {
	// Process incarnation died. Its transaction is CANCELLED: descriptors are closed, the transaction is
	// destroyed, and no late answer is ever published -- a stale request must not survive into the next
	// incarnation, and a waiter must not leak.
	auto it = _processControlTxns.find(pid);
	if (it == _processControlTxns.end()) {
		return;
	}
	if (it->second->fd >= 0) {
		close(it->second->fd);
		it->second->fd = -1;
	}
	_processControlTxns.erase(it);
	Metrics::shared().processControlTxnCancelled.fetch_add(1, std::memory_order_relaxed);
}

// perf#30 (doc section 234): the stall dump. Called from the main loop with a monotonic timestamp. It reports only
// after the serviced counter has stood still for STALL_IDLE_MS, and only once per STALL_REPRINT_MS, so it can never
// flood a log and can never itself be the reason a run is slow. A monotonic clock is used, never the guest's.
#define STALL_IDLE_MS 5000ull
#define STALL_REPRINT_MS 30000ull
void DarlingServer::Server::_stallDumpNoteProgress(uint64_t servicedNow) {
	if (!_stallDumpEnabled) {
		return;
	}
	if (servicedNow != _stallDumpServicedTotal) {
		_stallDumpServicedTotal = servicedNow;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		_stallDumpLastProgressMs = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
	}
}

void DarlingServer::Server::_maybeStallDump(uint64_t nowMs) {
	if (!_stallDumpEnabled || _stallDumpLastProgressMs == 0) {
		return;
	}
	if (nowMs - _stallDumpLastProgressMs < STALL_IDLE_MS) {
		return;
	}
	if (_stallDumpLastPrintMs != 0 && nowMs - _stallDumpLastPrintMs < STALL_REPRINT_MS) {
		return;
	}
	_stallDumpLastPrintMs = nowMs;
	static DarlingServer::Log stallLog("stall-dump");
	size_t processes = 0, threads = 0, waiting = 0, pending = 0, continuations = 0, suspended = 0;
	std::string detail;
	DarlingServer::processRegistry().forEachEntry([&](const std::shared_ptr<DarlingServer::Process>& process) {
		if (!process) {
			return;
		}
		++processes;
		for (auto& thread : process->threads()) {
			if (!thread) {
				continue;
			}
			++threads;
			auto pendingCall = thread->pendingCall();
			auto activeCall = thread->activeCall();
			const bool susp = thread->isCurrentlySuspended();
			const bool cont = thread->hasContinuation();
			const bool wait = thread->waitingForReply();
			if (wait) ++waiting;
			if (pendingCall) ++pending;
			if (cont) ++continuations;
			if (susp) ++suspended;
			// Only the threads that are IN a state a deadlock can be made of are printed in detail: a healthy thread
			// list is noise, and the detail string is bounded by taking the first 24 such threads.
			unsigned long long timerActive = 0;
			int timerSet = dtape_thread_wait_timer_state(thread->dtapeThread(), &timerActive);
			if ((pendingCall || cont || susp || wait) && detail.size() < 4096) {
				char buf[320];
				snprintf(buf, sizeof(buf), " pid=%d tid=%d dtape=0x%llx pending=%d active=%d suspended=%d continuation=%d waiting=%d timer=%d tactive=%llu;",
					(int)process->nsid(), (int)thread->nsid(),
					(unsigned long long)thread->dtapeThread(),
					pendingCall ? (int)pendingCall->number() : -1,
					activeCall ? (int)activeCall->number() : -1,
					susp ? 1 : 0, cont ? 1 : 0, wait ? 1 : 0, timerSet, timerActive);
				detail += buf;
			}
		}
	});
	// composes with the ring: the stall dump says WHICH state the run is in, the ring says HOW it got there. The ring
	// only exists when DSERVER_TRACE_RING=1, so an ordinary run pays nothing for either.
	dtape_test_ring_dump();
	stallLog.error() << "stall-dump idle_ms=" << (nowMs - _stallDumpLastProgressMs)
		<< " serviced=" << _stallDumpServicedTotal
		<< " processes=" << processes << " threads=" << threads
		<< " waiting=" << waiting << " pending=" << pending
		<< " continuations=" << continuations << " suspended=" << suspended
		<< " detail=" << detail << stallLog.endLog;
}

void DarlingServer::Server::_closeFdCourierForPid(pid_t pid) {
	_cancelProcessControlTxn(pid);
	// perf#28d: the ONE delivery is re-armed when the process's courier connection goes away, because
	// the loader and the guest image carry SEPARATE connections and the guest-side slot is per IMAGE:
	// without this the boot never reaches ready (measured -- `shellspawn did not become ready`, failing
	// at the earliest RING_ATTACH_RPC_SENT with `doorbell=-1`). So a pid-keyed rule delivers once per
	// connection (sent_to_guest/process = 2.00, each delivered once and both absorbed by the guest's own
	// slot logic: attach-failed=0, machmsg_uds=0). Exactly-once needs an identity finer than the pid.
	_ringDoorbellDelivered.erase(pid);
	_planeDoorbellDelivered.erase(pid);
	// perf#28d: clearing here gives the NEXT connection a delivery. MEASURED: with this erase removed the
	// rootless boot no longer reaches ready at all (`shellspawn did not become ready`, the failure landing
	// at the earliest RING_ATTACH_RPC_SENT), because the second image of the same process then owns no
	// doorbell anywhere: the loader and the guest image carry separate courier connections, and the
	// guest-side slot is per IMAGE, not per pid. So a pid-keyed rule cannot deliver exactly once -- it
	// delivers once per connection (`sent_to_guest/process = 2.00`, both deliveries legitimate and both
	// absorbed by the guest's own slot logic, with `attach-failed=0` and `machmsg_uds=0` across the whole
	// regression). Keying this on the incarnation properly requires an identity finer than the pid; until
	// then this erase stays, because removing it starves the boot.
	for (auto it = _fdCourierPending.begin(); it != _fdCourierPending.end();) {
		// keyed by token, so the process is named by the bundle's own arrival pid
		if (it->second.pid == pid) {
			if (it->second.fd >= 0) {
				close(it->second.fd);
				Metrics::shared().fdCourierOrphanFdClosed.fetch_add(1, std::memory_order_relaxed);
				Metrics::shared().fdCourierFdsClosed.fetch_add(1, std::memory_order_relaxed);
			}
			it = _fdCourierPending.erase(it);
		} else {
			++it;
		}
	}
	// A waiter whose descriptor never came is a semantic orphan: no reply is sent (the caller is gone),
	// and no success is faked. Counted so the condition is visible instead of silent.
	for (auto it = _fdCourierWaiters.begin(); it != _fdCourierWaiters.end();) {
		if (it->second.pid == pid) {
			Metrics::shared().fdCourierSemanticOrphan.fetch_add(1, std::memory_order_relaxed);
			it = _fdCourierWaiters.erase(it);
		} else {
			++it;
		}
	}
	_fdCourierGeneration.erase(pid);

	// perf#30 PROCESS-CONTROL PLANE (doc section 200): the page belongs to an INCARNATION, and this map is keyed
	// by pid -- so the outgoing incarnation's destructor used to unmap and erase the page the INCOMING one had
	// already registered, because an execve keeps the pid. MEASURED: a SIGSEGV at `page->reply_seq` (offset 56)
	// with the fault address landing exactly on the new region's page, every time the second incarnation's checkin
	// was serviced -- the completion ran after a nested pump and stored into the destroyed mapping, the server died
	// silently, and the guest waited on a page nobody would answer.
	//
	// The mapping is therefore NOT destroyed here. The entry is replaced when the next incarnation registers its
	// page (which also leaves the previous mapping in place), and an entry left behind by a process that simply
	// exits costs one page that is reclaimed with the prefix. Closing the descriptor is safe: closing does not
	// unmap. A mapping that no completion can destroy is worth 528 bytes per incarnation.
	auto control = _processControl.find(pid);
	if (control != _processControl.end() && control->second.fd >= 0) {
		// perf#30 (doc section 228): KEEP THE DESCRIPTOR WHILE THE PROCESS IS ALIVE. MEASURED: closing it here left
		// `region_fd=-1 ino=0 dev=0 size=0` for a LIVE process, and every completion for the next incarnation then went
		// into a mapping the guest was not polling -- the loader spun with `reply=0` while the server logged
		// `process-control-service` for the very same request. The caller of this function is a Process DESTRUCTOR, and
		// an execve keeps the pid, so the destructor that arrives may belong to an OUTGOING incarnation while the entry
		// is the incoming one's. The identity is therefore released only when no process for this pid exists any more,
		// which is also what keeps a dead pid from leaking the descriptor.
		if (processRegistry().lookupEntryByNSID(static_cast<int>(pid))) {
			if (getenv("DARLING_SERVER_COURIER_LOG") != NULL) {
				static DarlingServer::Log keepLog("process-control-fd-kept");
				keepLog.error() << "region-fd-kept-for-live-process pid=" << pid << keepLog.endLog;
			}
		} else {
			close(control->second.fd);
			control->second.fd = -1;
		}
	}
};

void DarlingServer::Server::_handleStatConnection() {
	static DarlingServer::Log metricsLog("metrics");

	// accept every pending connection (the listener is non-blocking)
	while (true) {
		int client = accept4(_statListenerSocket, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				break;
			}
			metricsLog.warning() << "stat accept failed: " << strerror(errno) << metricsLog.endLog;
			break;
		}

		// gauges we own (work queue state). Everything else is read from atomics inside
		// the snapshot. clients_blocked_in_rpc == work items still queued + in-flight is
		// approximated by depth (a parked guest's call sits in the queue until serviced);
		// we expose the raw queue stats so the watcher can apply its decision rule.
		auto wq = _workQueue.stats();
		std::ostringstream gauges;
		gauges << "\"workqueue_depth\": " << wq.depth
		       << ", \"workers_total\": " << wq.threadsTotal
		       << ", \"workers_busy\": " << wq.threadsBusy
		       << ", \"workers_available\": " << wq.threadsAvailable
		       << ", \"clients_blocked_in_rpc\": " << (wq.depth + wq.threadsBusy);

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 D17 (dar-1il.12): set the max-ring-threads-per-process gauge at snapshot time (only
		// when the residual census is armed). The server registers one ring thread per attached guest
		// thread, so the peak count of ring threads sharing a process IS the server-side proxy for the
		// guest's "max lanes/process". Computed off the hot path, under the ring-threads lock.
		if (Metrics::shared().residualCensusOn.load(std::memory_order_relaxed)) {
			std::unordered_map<Process*, uint64_t> perProc;
			{
				std::unique_lock lock(_ringThreadsLock);
				for (auto& weak : _ringThreads) {
					if (auto t = weak.lock()) {
						if (auto p = t->process()) {
							perProc[p.get()] += 1;
						}
					}
				}
			}
			uint64_t mx = 0;
			for (auto& kv : perProc) mx = std::max(mx, kv.second);
			Metrics::shared().maxRingThreadsPerProcess.store(mx, std::memory_order_relaxed);
		}
#endif

		std::string json = Metrics::shared().snapshotJSON(gauges.str());

		// best-effort blocking-ish write; the payload is tiny (<1KB) so a single write
		// almost always completes. Loop for partial writes; give up on hard error.
		size_t off = 0;
		while (off < json.size()) {
			ssize_t n = write(client, json.data() + off, json.size() - off);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// the tiny snapshot didn't fit the socket buffer and the client isn't
					// reading; don't block the event loop -- drop the rest.
					break;
				}
				break;
			}
			off += static_cast<size_t>(n);
		}
		close(client);
	}
};

void DarlingServer::Server::start() {
	Thread::interruptDisable();
	dtape_init(&DTapeHooks::dtape_hooks);
	Thread::interruptEnable();

	// force the kernel process to be created now
	Process::kernelProcess();

	// perform dtape initialization that requires a microthread context
	Thread::kernelSync(dtape_init_in_thread);

	while (true) {
		// perf#30 PROCESS-CONTROL PLANE: service the rare management pages once per loop pass. They are
		// shared memory, so there is no event to wait for beyond the loop itself -- and a request only
		// appears when a guest is blocked on it, which is exactly when the loop is otherwise idle.
		_serviceProcessControl();
		// drain AFTER the pass: the pass must have returned before any Call it armed is run (see the member's comment)
		runDeferredPlaneCalls();
		if (_canRead) {
			_canRead = _inbox.receiveMany(_listenerSocket);

			while (auto msg = _inbox.pop()) {
				// perf #0 (dar-dar6x4-perf-5dq.6): count every inbound message.
				Metrics::shared().messagesReceived.fetch_add(1, std::memory_order_relaxed);
				// TODO: this could be done concurrently
				//
				// callFromMessage() runs here on the main event loop and can throw:
				//   * std::invalid_argument for a malformed/unknown call,
				//   * std::runtime_error("Thread's pending call overwritten while active")
				//     from Thread::setPendingCall when an RPC races a still-pending call
				//     (routinely under a heavy fork/signal storm, e.g. building Homebrew),
				//   * std::system_error from the dtape/registry machinery.
				// There is no try/catch anywhere above this in Server::start(), so an
				// uncaught throw here unwinds out of the event loop -> std::terminate ->
				// the whole darlingserver process dies, orphaning every guest. (This is a
				// distinct server-exit path from the processCall() guards in
				// Thread::microthreadWorker / Call::kernelAsync, which do NOT cover message
				// construction/dispatch.) Contain it: log and drop just this one message so
				// the server keeps serving every other guest.
				// perf#26 RING-MACH-MSG: this is the GENUINE UDS ingress (the listener socket); the ring
				// dispatch builds its own Message and never passes through here. So a callnum-38 header
				// counted here is unambiguously a UDS-transported mach_msg_overwrite.
				if (msg->data().size() >= sizeof(dserver_rpc_callhdr_t)
				    && reinterpret_cast<const dserver_rpc_callhdr_t*>(msg->data().data())->number
				       == dserver_callnum_mach_msg_overwrite) {
					Metrics::shared().udsMachMsgRequest.fetch_add(1, std::memory_order_relaxed);
				}
				try {
					auto call = DarlingServer::Call::callFromMessage(std::move(*msg));
					if (call) {
						// perf #18 D15a (dar-1il.10): attach-timeline census. This is the GENUINE UDS
						// receive site (the main event loop reading the listener socket) -- ring calls
						// are dispatched in ringServiceThread and never pass through here, so a call
						// recorded here is unambiguously UDS-transported. Record the per-process UDS
						// ordinal + whether the ring had attached yet + static ring-eligibility, so we
						// can size the pre-attach eligible-UDS window. No-op unless the census is armed.
						if (Metrics::shared().attachCensusOn.load(std::memory_order_relaxed)) {
							if (auto t = call->thread()) {
								if (auto p = t->process()) {
									uint64_t ord = p->nextUdsCallOrdinal();
									bool attachedYet = p->ringAttachedYet();
									bool eligible = DarlingServer::Call::ringEligibleCallnum(
										static_cast<uint32_t>(call->number()));
									Metrics::shared().recordAttachCensusUdsCall(
										static_cast<uint32_t>(call->number()), ord, attachedYet, eligible);
								}
							}
						}
						// perf #18 D17 (dar-1il.12): POST-D16 residual UDS classifier. Same UDS choke point;
						// classifies each UDS call by WHY it is on UDS using server-observable per-(thread,
						// process) state -- whether THIS thread has a live ring now (#ifdef'd: ring() exists
						// only with the ring transport) and whether the process ever attached one. This is
						// what separates reason A (first-eligible-before-this-thread's-lane) from B/D (an
						// eligible op on UDS despite a live lane = wrapper gap / forced fallback). No-op
						// unless DARLING_SERVER_RESIDUAL_CENSUS=1. No lane-ownership change (D17 constraint).
						if (Metrics::shared().residualCensusOn.load(std::memory_order_relaxed)) {
							uint32_t cn = static_cast<uint32_t>(call->number());
							bool eligible = DarlingServer::Call::ringEligibleCallnum(cn);
							bool controlPlane = (cn == static_cast<uint32_t>(dserver_callnum_checkin))
							                 || (cn == static_cast<uint32_t>(dserver_callnum_ring_attach));
							bool threadHasRing = false;
							bool procEverAttached = false;
							if (auto t = call->thread()) {
#ifdef DSERVER_RING_TRANSPORT
								threadHasRing = (t->ring() != nullptr);
#endif
								if (auto p = t->process()) {
									procEverAttached = p->ringAttachedYet();
								}
							}
							Metrics::shared().recordResidualReason(cn, threadHasRing, procEverAttached, eligible, controlPlane);
						}
						// perf #2b (dar-dar6x4-perf-5dq.8): run the call INLINE on the main
						// event loop instead of always handing it to the worker pool. The
						// profile (perf #2a) showed ~70% of server CPU was the main-loop ->
						// worker condvar handoff (pthread_cond_signal -> futex_wake) for work
						// that is itself <1% of CPU -- so for the common case of a cheap,
						// non-blocking RPC (checkin etc.) the wakeup tax dwarfs the work.
						//
						// doWork() never blocks its caller: a non-blocking call runs to
						// completion here; a call that blocks suspends its microthread (via
						// setcontext) and returns control to us, having already arranged its
						// own resume onto the work queue (Thread::doWork doneWorking ->
						// scheduleThread). So running inline is safe in BOTH cases and only
						// the genuinely-blocking minority ever pays for the worker pool.
						auto thread = call->thread();
						if (thread) {
							thread->doWork();
							auto& m = Metrics::shared();
							if (thread->isCurrentlySuspended()) {
								m.queuedToPool.fetch_add(1, std::memory_order_relaxed);
							} else {
								m.inlineHandled.fetch_add(1, std::memory_order_relaxed);
							}
						}
					}
				} catch (const std::exception& ex) {
					static DarlingServer::Log serverLog("server");
					serverLog.error() << "Dropping message: callFromMessage threw: " << ex.what() << serverLog.endLog;
				} catch (...) {
					static DarlingServer::Log serverLog("server");
					serverLog.error() << "Dropping message: callFromMessage threw a non-std exception" << serverLog.endLog;
				}
			}
		}

		// reset the eventfd by reading from it
		eventfd_t value;
		eventfd_read(_wakeupFD, &value);

		if (_canWrite) {
			_canWrite = _outbox.sendMany(_listenerSocket);
		}

		struct epoll_event settings;
		settings.data.ptr = &_wakeupFD;
		settings.events = (_canWrite) ? (EPOLLIN | EPOLLONESHOT) : 0;

		if (epoll_ctl(_epollFD, EPOLL_CTL_MOD, _wakeupFD, &settings) < 0) {
			throw std::system_error(errno, std::generic_category(), "Failed to modify eventfd in epoll context");
		}

		struct epoll_event events[16];
		int ret;

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 P4 (dar-dar6x4-perf-5dq.33): adaptive pre-epoll spin (the gist's wake model).
		// Before committing to a blocking epoll_wait we busy-poll the attached rings for a bounded
		// budget so a hot RPC stream is serviced with zero doorbell syscalls and zero scheduler
		// handoffs. The budget is small (balanced default 20us) and we ALSO poll epoll with a 0
		// timeout each iteration so UDS + other Monitors are never starved -- if anything else is
		// ready we break straight out and handle it. Only when the budget elapses with no ring or
		// epoll activity do we arm + block, exactly as a server with no rings always has.
		_resolveRingSpinBudget();
		bool haveRings;
		{
			std::unique_lock lock(_ringThreadsLock);
			haveRings = !_ringThreads.empty();
		}
		ret = -2; // sentinel: "not yet blocked"
		if (haveRings && _ringSpinNs > 0) {
			struct timespec ts0;
			clock_gettime(CLOCK_MONOTONIC, &ts0);
			uint64_t startNs = (uint64_t)ts0.tv_sec * 1000000000ull + ts0.tv_nsec;
			_setAllRingStates(DSERVER_RING_SRV_ACTIVE_POLLING);
			for (;;) {
				uint32_t serviced = _drainRings();

				// Harvest any UDS / Monitor / wakeup readiness without blocking, so the ring spin
				// never delays them. If something is ready, take it now (ret > 0 -> dispatch loop).
				ret = epoll_wait(_epollFD, events, 16, 0);
				if (ret != 0) {
					break; // ready fds (ret>0) or error (ret<0, handled below)
				}
				// THE WORK QUANTUM BOUNDS THE SPIN EVEN WHILE RING WORK KEEPS ARRIVING (dar-dles). MEASURED:
				// `serviced > 0 -> continue` skipped this check entirely, and _drainRings() has NO internal
				// bound -- it walks every attached ring thread and runs the full Call path per thread -- so
				// with continuous ring load the outer loop body was never reached. The management plane was
				// then scanned only in the gaps between ring bursts (5-26 scans per 2 s against a ~130/s
				// run average), a published management request waited seconds, and pthread_create stalled
				// behind it. Consulting the budget unconditionally keeps the hot path hot (the drain still
				// runs first, still with zero doorbell syscalls) and guarantees the outer body -- management
				// service AND the deferred plane calls -- is reached at least once per quantum. This is not
				// periodic polling of an idle server: the spin only exists while rings are attached, and a
				// genuinely idle loop still arms and blocks in epoll below.
				struct timespec ts1;
				clock_gettime(CLOCK_MONOTONIC, &ts1);
				uint64_t nowNs = (uint64_t)ts1.tv_sec * 1000000000ull + ts1.tv_nsec;
				if (nowNs - startNs >= _ringSpinNs) {
					break; // quantum exhausted -> service the plane + deferred calls, then re-arm
				}
				if (serviced == 0) {
					__builtin_ia32_pause();
				}
			}
		}

		if (ret == -2 || ret == 0) {
			// Either we never spun (no rings / low-power) or the spin budget elapsed idle. Arm the
			// sleep state with the gist's critical recheck: publish SLEEP_ARMED, drain ONCE more
			// (closing the publish/sleep race -- a guest that produced after our last drain but
			// before reading SLEEP_ARMED will have doorbelled, but a guest that read ACTIVE_POLLING
			// and skipped the doorbell is caught here), and only if still idle commit to epoll.
			_setAllRingStates(DSERVER_RING_SRV_SLEEP_ARMED);
			_ringSleepProbe("armed");
			if (haveRings && _drainRings() > 0) {
				// raced in a request -> go service its reply path; don't sleep.
				_setAllRingStates(DSERVER_RING_SRV_ACTIVE_POLLING);
				continue;
			}
			_ringSleepProbe("presleep");
			_setAllRingStates(DSERVER_RING_SRV_SLEEPING_EPOLL);
			_ringSleepProbe("sleeping");
			// Under the stall watchdog the block must be finite, or the watchdog never gets to run while the server
			// is idle -- which is exactly the state a deadlock leaves it in.
			// perf#30 R1 COURIER PURITY: a bounded wait, because the process-management plane must NOT need a datagram to
		// be noticed. MEASURED: with `-1` the loop blocked until some other event arrived, the four plane publishes had to
		// send a zero-fd wake byte on the courier to get serviced, and removing that byte (the requirement) made the boot
		// stop after one publish with no completion. The plane pass already runs before the loop blocks; this makes it run
		// periodically too, so removing the wake byte costs at most this much latency on the RARE plane path.
		ret = epoll_wait(_epollFD, events, 16, _stallDumpEnabled ? 500 : (_debugConnections ? _debugConnections->nextWaitMilliseconds() : -1) /* perf#30 (directive section 1): the 2 ms management poll is GONE. MEASURED basis: every plane publish now rings the per-process doorbell (8/8, none=0) and the arm hands the descriptor over within 2 ms of the page, so nothing depends on a periodic wake. The server sleeps indefinitely and is woken by work, exactly as section 4 requires. */);
			_setAllRingStates(DSERVER_RING_SRV_ACTIVE_POLLING);
		}
#else
		// perf#30 R1 COURIER PURITY: a bounded wait, because the process-management plane must NOT need a datagram to
		// be noticed. MEASURED: with `-1` the loop blocked until some other event arrived, the four plane publishes had to
		// send a zero-fd wake byte on the courier to get serviced, and removing that byte (the requirement) made the boot
		// stop after one publish with no completion. The plane pass already runs before the loop blocks; this makes it run
		// periodically too, so removing the wake byte costs at most this much latency on the RARE plane path.
		ret = epoll_wait(_epollFD, events, 16, _stallDumpEnabled ? 500 : (_debugConnections ? _debugConnections->nextWaitMilliseconds() : -1) /* perf#30 (directive section 1): the 2 ms management poll is GONE. MEASURED basis: every plane publish now rings the per-process doorbell (8/8, none=0) and the arm hands the descriptor over within 2 ms of the page, so nothing depends on a periodic wake. The server sleeps indefinitely and is woken by work, exactly as section 4 requires. */);
#endif

		// perf#30 (doc section 234): the stall watchdog. Progress is the number of requests this loop has serviced;
		// when it stands still the dump names every thread that is waiting, pending, suspended or continuation-parked.
		// Progress is read from counters that tick ONLY when the server actually served or answered something, so the
		// ring spin (which calls this loop continuously) cannot keep the watchdog quiet by merely spinning.
		uint64_t _progress = Metrics::shared().ringS2cFull.load(std::memory_order_relaxed)
			+ Metrics::shared().ringServicedSpin.load(std::memory_order_relaxed)
			+ Metrics::shared().ringServicedDoorbell.load(std::memory_order_relaxed)
			+ Metrics::shared().ringProcessDoorbellServiced.load(std::memory_order_relaxed);
		if (!_stallDumpInitialized) {
			_stallDumpInitialized = true;
			const char* sd = getenv("DARLING_SERVER_STALL_DUMP");
			_stallDumpEnabled = (sd != NULL && sd[0] == '1');
			if (_stallDumpEnabled) {
				static DarlingServer::Log sdLog("stall-dump");
				sdLog.error() << "stall-dump enabled (idle " << STALL_IDLE_MS << "ms, reprint " << STALL_REPRINT_MS << "ms)" << sdLog.endLog;
				_stallDumpNoteProgress(_progress);
			}
		}
		if (_stallDumpEnabled) {
			_stallDumpNoteProgress(_progress);
			struct timespec tsd;
			clock_gettime(CLOCK_MONOTONIC, &tsd);
			_maybeStallDump((uint64_t)tsd.tv_sec * 1000ull + (uint64_t)tsd.tv_nsec / 1000000ull);
		}

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}

			throw std::system_error(errno, std::generic_category(), "Failed to wait on epoll context");
		}

		for (size_t i = 0; i < ret; ++i) {
			struct epoll_event* event = &events[i];

			if (event->data.ptr == this) {
				if (event->events & EPOLLIN) {
					_canRead = true;
				}

				if (event->events & EPOLLOUT) {
					_canWrite = true;
				}
			} else if (event->data.ptr == &_wakeupFD) {
				// we allow the loop to go back to the top and try to send some messages
				// (if _canWrite is true, the eventfd will be reset; otherwise, there's no point in resetting it)
			} else if (event->data.ptr == &_fdCourierListenerSocket) {
				// perf#30 FD-COURIER: descriptor delivery. Accept new process connections, then read
				// every complete envelope that is waiting (persistent connections carry many transfers).
				_handleFdCourierConnection();
				_drainFdCourierMessages();
				_serviceProcessControl();
		// drain AFTER the pass: the pass must have returned before any Call it armed is run (see the member's comment)
		runDeferredPlaneCalls();
			} else if (_isFdCourierSocket(event->data.ptr)) {
				// perf#30 PHASE-0: traffic on an established courier connection -- a descriptor bundle or
				// the one-byte management wake. Either way the loop drains, then services the page.
				_drainFdCourierMessages();
				_serviceProcessControl();
		// drain AFTER the pass: the pass must have returned before any Call it armed is run (see the member's comment)
		runDeferredPlaneCalls();
			} else if (event->data.ptr == &_statListenerSocket) {
				// perf #0 (dar-dar6x4-perf-5dq.6): a metrics client connected; serve the
				// snapshot. Drain all pending connections (level-triggered would re-fire,
				// but we accept in a loop to be safe and cheap).
				_handleStatConnection();
			} else if (event->data.ptr == &_debugListenerSocket) {
				_debugConnections->acceptPending(_debugListenerSocket);
			} else if (_debugConnections && _debugConnections->handleEvent(*event)) {
				// The owning event context collects and advances bounded client I/O.
			} else if (event->data.ptr == &_timerFD) {
				std::unique_lock lock(_timerLock);
				uint64_t expirations = 0;

				if (read(_timerFD, &expirations, sizeof(expirations)) < 0) {
					if (errno == EAGAIN) {
						// spurious event?
						continue;
					}

					throw std::system_error(errno, std::generic_category(), "Failed to read from timerfd");
				}

				if (expirations < 1) {
					// spurious expiration?
					continue;
				}

				// we're done handling the timerfd;
				// we don't need to lock anymore (and the following call might need to arm the timer again)
				lock.unlock();

				// perf#30 (doc section 250): the loop DID see the timerfd. Recorded before the dispatch, so a
				// `timer_arm` with no `timer_fired` means the loop never read it.
				dtape_test_ring_record4(8, expirations, 0, 0, 0);

				// dtape_timer_fired() calls duct-taped functions that may need to wait (briefly), so it needs to be called in a microthread
				Thread::kernelAsync(dtape_timer_fired);
			} else {
				Monitor* monitor = static_cast<Monitor*>(event->data.ptr);
				std::shared_ptr<Monitor> aliveMonitor = nullptr;

				// check whether the monitor is still valid
				_monitorsLock.lock();
				for (const auto& mon: _monitors) {
					if (mon.get() == monitor) {
						aliveMonitor = mon;
						break;
					}
				}
				_monitorsLock.unlock();

				// if the monitor died/was removed, ignore the event
				if (!aliveMonitor) {
					continue;
				}

				aliveMonitor->_callback(aliveMonitor, static_cast<Monitor::Event>(event->events & (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP)));
			}
		}

		if (_debugConnections) _debugConnections->finishEventBatch();

		// as our final job on this wakeup, clear the list of monitors waiting to be removed.
		// this will destroy those references, possibly causing the monitors to be deallocated.
		//
		// it's necessary to do this instead of just removing them in removeMonitor in order to
		// avoid a potential race between an existing monitor being removed in removeMonitor,
		// another being subsequently created for the same address and added to the server,
		// and an event being received for the original monitor.
		//
		// since we keep a reference to the shared_ptrs until the end of this event loop iteration,
		// there's no chance that a new monitor will be created with the same address as a monitor
		// for which an event was returned in this event loop iteration.
		_monitorsLock.lock();
		_monitorsWaitingToDie.clear();
		_monitorsLock.unlock();
	}

	// shouldn't ever be reached (exiting the main loop would be an error), but just in case
	dtape_deinit();
};

void DarlingServer::Server::monitorProcess(std::shared_ptr<Process> process) {
	// the this-capture here is safe because the Server will always out-live everything else
	std::weak_ptr<Process> weakProcess = process;
	auto monitor = std::make_shared<Monitor>(process->_pidfd, Monitor::Event::Readable | Monitor::Event::HangUp, false, false, [this, weakProcess](std::shared_ptr<Monitor> thisMonitor, Monitor::Event events) {
		removeMonitor(thisMonitor);

		auto process = weakProcess.lock();

		if (!process) {
			// the process already died...
			return;
		}

		process->notifyDead();
	});

	addMonitor(monitor);
};

DarlingServer::Server& DarlingServer::Server::sharedInstance() {
	return *sharedInstancePointer;
};

std::string DarlingServer::Server::prefix() const {
	return _prefix;
};

int DarlingServer::Server::prefixFD() const {
	return _prefixFD;
};

pid_t DarlingServer::Server::namespaceIDForPeer(pid_t peerHostPID, pid_t reportedNamespaceID) const {
	return ProcessIdentity::namespaceIDForPeer(_rootlessInitHostPID, peerHostPID, reportedNamespaceID);
};

void DarlingServer::Server::_worker(std::shared_ptr<Thread> thread) {
	thread->doWork();
};

void DarlingServer::Server::scheduleThread(std::shared_ptr<Thread> thread) {
	_workQueue.push(thread);
};

void DarlingServer::Server::addMonitor(std::shared_ptr<Monitor> monitor) {
	bool valid = true;

	_monitorsLock.lock();
	for (size_t i = 0; i < _monitors.size(); ++i) {
		if (_monitors[i].get() == monitor.get()) {
			valid = false;
			break;
		}
	}

	if (!valid) {
		_monitorsLock.unlock();
		return;
	}

	monitor->_lock.lock();
	struct epoll_event settings;
	settings.data.ptr = monitor.get();
	settings.events = monitor->_events;

	if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, monitor->_fd->fd(), &settings) < 0) {
		monitor->_lock.unlock();
		_monitorsLock.unlock();
		throw std::system_error(errno, std::generic_category(), "Failed to add descriptor to epoll context");
	}

	monitor->_server = this;

	monitor->_lock.unlock();

	_monitors.push_back(monitor);

	_monitorsLock.unlock();
};

void DarlingServer::Server::removeMonitor(std::shared_ptr<Monitor> monitor) {
	bool valid = false;

	_monitorsLock.lock();
	for (size_t i = 0; i < _monitors.size(); ++i) {
		if (_monitors[i].get() == monitor.get()) {
			valid = true;
			_monitorsWaitingToDie.push_back(monitor);
			_monitors.erase(_monitors.begin() + i);
			break;
		}
	}

	if (!valid) {
		_monitorsLock.unlock();
		return;
	}

	if (epoll_ctl(_epollFD, EPOLL_CTL_DEL, monitor->_fd->fd(), NULL) < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to remove descriptor from epoll context");
	}

	monitor->_server = nullptr;

	_monitorsLock.unlock();

	// force an event loop wakeup (so the removal can be finalized as soon as possible)
	eventfd_write(_wakeupFD, 1);
};

#ifdef DSERVER_RING_TRANSPORT
void DarlingServer::Server::registerRingThread(std::shared_ptr<Thread> thread) {
	std::unique_lock lock(_ringThreadsLock);
	// prune dead entries while we're here, and avoid duplicates
	for (size_t i = 0; i < _ringThreads.size();) {
		auto t = _ringThreads[i].lock();
		if (!t) {
			_ringThreads.erase(_ringThreads.begin() + i);
		} else if (t.get() == thread.get()) {
			return; // already registered
		} else {
			++i;
		}
	}
	_ringThreads.push_back(thread);
	// perf #18 D17 (dar-1il.12): cumulative ring-thread registrations = a server-side "lanes acquired"
	// proxy (each successful per-thread ring_attach lands here exactly once). A plain stat counter.
	Metrics::shared().totalRingThreadsRegistered.fetch_add(1, std::memory_order_relaxed);
};

void DarlingServer::Server::unregisterRingThread(std::shared_ptr<Thread> thread) {
	std::unique_lock lock(_ringThreadsLock);
	for (size_t i = 0; i < _ringThreads.size();) {
		auto t = _ringThreads[i].lock();
		if (!t || t.get() == thread.get()) {
			_ringThreads.erase(_ringThreads.begin() + i);
		} else {
			++i;
		}
	}
};

// perf#28 (ONE doorbell): create + register the single Ring wake eventfd on first use and hand the
// caller a fresh dup to send to the guest. Idempotent: later lanes get another dup of the SAME object.
uint64_t DarlingServer::Server::_ringDoorbellGeneration(pid_t pid) {
	auto gen = _fdCourierGeneration.find(pid);
	return (gen != _fdCourierGeneration.end()) ? gen->second : 0;
}

int DarlingServer::Server::ringDoorbellDupForGuest() {
	std::unique_lock lock(_ringDoorbellLock);
	if (_ringDoorbellFD.fd() < 0) {
		int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (efd < 0) {
			return -1;
		}
		_ringDoorbellFD = FD(efd);
		int watchDup = ::dup(efd);
		if (watchDup < 0) {
			return -1;
		}
		auto monitor = std::make_shared<Monitor>(
			std::make_shared<FD>(watchDup),
			Monitor::Event::Readable | Monitor::Event::HangUp,
			false, false,
			[](std::shared_ptr<Monitor> thisMonitor, Monitor::Event events) {
				eventfd_t value;
				eventfd_read(thisMonitor->fd()->fd(), &value);
				if (static_cast<uint64_t>(events & Monitor::Event::Readable) != 0) {
					Server::sharedInstance()._ringDoorbellWake();
				}
				if (static_cast<uint64_t>(events & Monitor::Event::HangUp) != 0) {
					Server::sharedInstance()._ringDoorbellHangup(thisMonitor);
				}
			}
		);
		addMonitor(monitor);
		_ringDoorbellMonitor = monitor;
	}
	return ::dup(_ringDoorbellFD.fd());
}

// The COLD wake: the guest doorbelled because it could not rely on the server seeing the publish by
// polling. Service everything that has work -- the scan is process-independent (see the header), so one
// scan covers every lane of every process that shares this doorbell.
void DarlingServer::Server::_ringDoorbellWake() {
	Metrics::shared().ringProcessDoorbellWakeups.fetch_add(1, std::memory_order_relaxed);
	uint32_t n = _drainRings();
	if (n > 0) {
		Metrics::shared().ringProcessDoorbellServiced.fetch_add(n, std::memory_order_relaxed);
	}
	// perf#30 (directive sections 18-22, violation A): the doorbell must service the MANAGEMENT PAGE too, not only
	// the rings. MEASURED: while the plane's wake rode the courier's four bytes, deleting those bytes left a plane
	// publish unserviced for as long as the loop stayed blocked, and the compensating change at the time was a 2 ms
	// epoll timeout -- a busy-wake the directive forbids. The guest already rings THIS process doorbell on every
	// plane publish (mldr.c:1534-1545), so servicing the page here is what lets the loop block indefinitely again:
	// the wake path, not the timeout, becomes the thing that makes progress. The scan is global for the same reason
	// the ring scan is (see _ringDrainAll): the wake counter belongs to one process incarnation, the scan does not.
	_serviceProcessControl();
}

void DarlingServer::Server::_ringDoorbellHangup(std::shared_ptr<Monitor> monitor) {
	// Every guest dup of the doorbell is gone (guest process exit). Drop the Monitor; the ring threads
	// themselves retire through their own weak_ptr lifetime in _ringThreads.
	removeMonitor(monitor);
	std::unique_lock lock(_ringDoorbellLock);
	if (_ringDoorbellMonitor == monitor) {
		_ringDoorbellMonitor = nullptr;
	}
}

// perf#28c: the wake scan. Deliberately still GLOBAL: the isolation being bought is that a wake
// counter belongs to one process incarnation, not that the scan is narrowed. Narrowing it
// (_drainProcessRings) is a fairness/perf question, and it is only worth doing with evidence.
uint32_t DarlingServer::Server::_ringDrainAll() {
	Metrics::shared().ringProcessDoorbellWakeups.fetch_add(1, std::memory_order_relaxed);
	uint32_t n = _drainRings();
	if (n > 0) {
		Metrics::shared().ringProcessDoorbellServiced.fetch_add(n, std::memory_order_relaxed);
	}
	return n;
}

std::shared_ptr<DarlingServer::Server::RingProcessWake> DarlingServer::Server::_ringWakeFor(pid_t pid) {
	std::unique_lock lock(_ringWakeRegistryLock);
	auto it = _ringWakeRegistry.find(pid);
	if (it != _ringWakeRegistry.end()) {
		// PID-REUSE DEFENCE: an entry is valid only for the incarnation its pidfd was opened for. A
		// pidfd for a dead process is readable, so if that entry's anchor says dead, this pid now
		// names a DIFFERENT incarnation and the entry must be replaced rather than reused.
		bool dead = false;
		if (it->second->pidfd.fd() >= 0) {
			struct pollfd p = { it->second->pidfd.fd(), POLLIN, 0 };
			dead = (::poll(&p, 1, 0) == 1) && ((p.revents & (POLLIN | POLLHUP | POLLERR)) != 0);
		} else {
			dead = true;
		}
		if (!dead) {
			return it->second;
		}
	}
	lock.unlock();
	if (it != _ringWakeRegistry.end()) {
		_ringWakeRetire(pid);
	}

	int pfd = (int)::syscall(SYS_pidfd_open, pid, 0);
	if (pfd < 0) {
		return nullptr;
	}
	int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (efd < 0) {
		::close(pfd);
		return nullptr;
	}
	int watchDup = ::dup(efd);
	int deathDup = ::dup(pfd);
	if (watchDup < 0 || deathDup < 0) {
		if (watchDup >= 0) ::close(watchDup);
		if (deathDup >= 0) ::close(deathDup);
		::close(efd);
		::close(pfd);
		return nullptr;
	}

	auto wake = std::make_shared<RingProcessWake>();
	wake->pid = pid;
	wake->pidfd = FD(pfd);
	wake->doorbell = FD(efd);
	wake->doorbellMonitor = std::make_shared<Monitor>(
		std::make_shared<FD>(watchDup),
		Monitor::Event::Readable | Monitor::Event::HangUp,
		false, false,
		[this](std::shared_ptr<Monitor> thisMonitor, Monitor::Event events) {
			eventfd_t value;
			eventfd_read(thisMonitor->fd()->fd(), &value);
			if (static_cast<uint64_t>(events & Monitor::Event::Readable) != 0) {
				_ringDrainAll();
				// WAKES MUST REACH THE MANAGEMENT PLANE FROM EITHER DOORBELL (dar-dles). A guest rings the
				// doorbell it actually holds, and that object is whichever of the two it adopted first: the
				// server-wide one (whose wake handler already services the plane) or this per-incarnation one.
				// Servicing only the rings here made the plane's progress depend on which object the guest
				// happened to keep, which is not a property any caller can see or reason about.
				_serviceProcessControl();
			}
		}
	);
	wake->deathMonitor = std::make_shared<Monitor>(
		std::make_shared<FD>(deathDup),
		Monitor::Event::Readable | Monitor::Event::HangUp,
		false, false,
		[this, pid](std::shared_ptr<Monitor> thisMonitor, Monitor::Event events) {
			(void)thisMonitor;
			(void)events;
			// the incarnation is gone: retire its wake object (monitors + fds) so nothing accumulates.
			_ringWakeRetire(pid);
		}
	);

	{
		std::unique_lock relock(_ringWakeRegistryLock);
		auto existing = _ringWakeRegistry.find(pid);
		if (existing != _ringWakeRegistry.end()) {
			// a racing attach created it first: keep that one and drop ours
			addMonitor(wake->doorbellMonitor);
			addMonitor(wake->deathMonitor);
			relock.unlock();
			_ringWakeRetire(pid);
			return _ringWakeFor(pid);
		}
		_ringWakeRegistry[pid] = wake;
	}
	addMonitor(wake->doorbellMonitor);
	addMonitor(wake->deathMonitor);
	Metrics::shared().ringDoorbellProcesses.fetch_add(1, std::memory_order_relaxed);
	return wake;
}

void DarlingServer::Server::_ringWakeRetire(pid_t pid) {
	std::shared_ptr<RingProcessWake> wake;
	{
		std::unique_lock lock(_ringWakeRegistryLock);
		auto it = _ringWakeRegistry.find(pid);
		if (it == _ringWakeRegistry.end()) {
			return;
		}
		wake = it->second;
		_ringWakeRegistry.erase(it);
	}
	if (wake->doorbellMonitor) {
		removeMonitor(wake->doorbellMonitor);
	}
	if (wake->deathMonitor) {
		removeMonitor(wake->deathMonitor);
	}
}

// perf#28d (round 49): ONE descriptor delivery per process incarnation. The guest's loader keeps the
// first doorbell it is given and answers every later ask from its own slot, so re-attaching a lane needs
// no descriptor: this returns -2, the reply carries no wake fd, and the guest keeps using the doorbell it
// already owns. -1 is reserved for "no doorbell exists for this process" (a real failure). MEASURED before this: every ring_attach reply carried another dup of the same kernel
// object -- N lanes meant N transfers of the same eventfd.
int DarlingServer::Server::ringDoorbellDupFor(pid_t pid) {
	auto wake = _ringWakeFor(pid);
	if (!wake) {
		return -1;
	}
	uint64_t generation = _ringDoorbellGeneration(pid);
	// perf#28d (round 49, MEASURED NEGATIVE): returning -1 for "this incarnation already has a doorbell"
	// is NOT yet safe. The loader's own seed attach builds its reply twice on the courier park path, so
	// the first build consumed the one delivery and the second got -1: the seed published no lane at all
	// (`attach-rc ... wake=-1`, `map=(nil)`) and the boot stopped. The one-time delivery therefore needs
	// the reply to be built once per attach, or the delivery to be keyed on something the guest supplies,
	// before it can be turned on. Reverted to the verified behaviour: every attach reply carries a dup,
	// and the guest's shared loader keeps the first and closes the rest (one wake fd per process, N
	// transfers). The generation is not even known for the loader's first attach (gen=0), which is the
	// second reason this cannot simply be switched on.
	// perf#28d (retry after Phase-0): one descriptor delivery per process incarnation. MEASURED why the
	// first attempt could not work: the loader's seed attach ran before any courier traffic existed, so the
	// bundle was stamped generation 0 and the guest refused it. The plane is now established before the
	// first write, so the generation is known here; and if it still is not, the courier send falls back to
	// the reply CMSG (sendFdCourierBundleToGuest returns 0 for generation 0), which is the correct
	// degradation rather than a refused bundle.
	// perf#28d (SECOND measured negative, round 49n): with Phase-0 in place the generation IS known and the
	// one-time rule still stops the boot -- `kind7=0` in the trace (the courier never carried the doorbell,
	// so the generation-0 fallback was taken), no `[dring-doorbell]` line at all, and HELLO never printed.
	// Reverted again to the verified behaviour: every attach reply carries a dup and the guest's shared
	// loader keeps the first and closes the rest. The remaining work is exactly what §26 said: make the
	// attach reply be built once per attach, and give the incarnation's generation to the server before its
	// first attach (the loader's seed attach still precedes its own courier traffic).
	// perf#28d (third attempt): one descriptor delivery per process incarnation. The two earlier attempts
	// are kept in the doc as measured negatives; what changed is that the generation now reaches the server
	// from the plane's PING, before this process's first attach, so the bundle is no longer stamped 0.
	// perf#28d (THIRD measured negative, with the generation fix in place): `kind=7` sent-to-guest = 1,
	// i.e. the courier DID carry the doorbell for the first time -- but the loader's own seed read
	// `wake=-1` and its client never called the courier receive, so the reply's token did not reach it and
	// the seed published no lane (HELLO=0). The next step is therefore not the delivery but the REPLY
	// TOKEN path: the generated wrapper resolves it, and it demonstrably did not resolve here. Reverted so
	// the doorbell stays on the verified CMSG-per-attach path; the GENERATION fix above is kept, since it
	// is verified GREEN on its own and is what the first two attempts lacked.
	// perf#28d (5th attempt): ONE doorbell descriptor per process lifetime, keyed by PID. The generation
	// was the wrong key -- MEASURED, it moves during a boot (the plane's PING teaches it after the first
	// attaches) and a generation-keyed rule sent 36 bundles. Combined with the reply token for
	// `ring_attach` in the SAME change, because a caller expecting a CMSG fd must not receive -1: both
	// halves were measured to work individually (kind=7 with a real generation; wake=8 resolved by the
	// loader after the loader-side receive was implemented).
	// perf#28d (FIFTH measured negative, round 49r): with the pid-keyed rule AND the reply token enabled
	// together, the first attach produced NO courier bundle at all (`kind=7` count 0) and the loader's seed
	// attach did not proceed -- so the two halves do not simply compose: with the rule on, the first call
	// either returns -1 before the courier is tried, or the fallback CMSG path does not carry the fd the
	// loader needs. Reverted to the verified behaviour; the pid-keyed set and the generator's reply token
	// field stay in the tree (they are correct in themselves and are what the next attempt needs), but the
	// rule and the table entry are OFF until the first-delivery path is instrumented step by step.
	// perf#28d (DECISIVE negative, round 49s): with the rule enabled the full regression still PASSES
	// (every test pass=1) while the TRANSPORT collapses -- `ring_doorbell_reused=1512`,
	// `attach-failed=16`, and the stress test runs `machmsg_ring=4 machmsg_uds=640`, i.e. it passes by
	// falling back to the datagram the whole way. That is the worst possible shape: a green suite on the
	// wrong transport. The rule makes the guest's ring_attach fail, and the guest's own
	// `no-wake-fd` diagnostic never fires, so the failure is not on the acceptance path I instrumented.
	// DISABLED, and it must stay disabled until that failure is understood: a doorbell strategy that
	// silently sends the workload back to UDS defeats the purpose of this bead.
	if (_ringDoorbellDelivered.count(pid)) {
		// -2, NOT -1: "-1" means "this process has no doorbell at all" and is a hard attach failure,
		// while a suppressed re-delivery is the expected steady state. Conflating them made EVERY later
		// attach fail (measured: attach-failed=16, then 20, with the whole workload falling back to UDS).
		Metrics::shared().ringDoorbellReused.fetch_add(1, std::memory_order_relaxed);
		return -2;
	}
	_ringDoorbellDelivered.insert(pid);
	Metrics::shared().ringDoorbellSentToGuest.fetch_add(1, std::memory_order_relaxed);
	return ::dup(wake->doorbell.fd());
}

void DarlingServer::Server::_setAllRingStates(uint32_t state) {
	std::unique_lock lock(_ringThreadsLock);
	for (auto& weak : _ringThreads) {
		if (auto t = weak.lock()) {
			if (auto r = t->ring()) {
				r->setServerState(state);
			}
		}
	}
};

#ifdef DSERVER_RING_TRANSPORT
// perf#26: a stall in this transport has to be readable without guessing. A signal handler cannot walk
// the thread registry safely, so SIGUSR1 only sets this flag and the MAIN LOOP (which is alive even when
// a call is stuck) prints a compact per-thread table on its next iteration: which thread, which call,
// which lane and seq, whether it is suspended, and whether a duplex upcall is in flight. One snapshot
// instead of thousands of trace lines.
static volatile sig_atomic_t g_ringStateDumpRequested = 0;
static void ringStateDumpHandler(int) { g_ringStateDumpRequested = 1; }
#endif

// PW4: see the header. Inert unless the test asks for one specific window, and BOUNDED (default one
// fire) because the probe delays the main loop: an unbounded 300ms stall per sleep transition starves
// boot -- measured as "Rootless shellspawn did not become ready" with the probe left on.
void DarlingServer::Server::_ringSleepProbe(const char* where) {
	static long probeUs = -2;
	static long probeMax = 1;
	static long probeFired = 0;
	static std::string probeAt;
	if (probeUs == -2) {
		const char* v = ::getenv("DSERVER_RING_SLEEP_PROBE_US");
		probeUs = (v && *v) ? ::strtol(v, nullptr, 10) : 0;
		const char* a = ::getenv("DSERVER_RING_SLEEP_PROBE_AT");
		probeAt = (a && *a) ? a : "";
		const char* m = ::getenv("DSERVER_RING_SLEEP_PROBE_MAX");
		probeMax = (m && *m) ? ::strtol(m, nullptr, 10) : 1;
	}
	if (probeUs <= 0 || probeAt.empty() || probeAt != where || probeFired >= probeMax) {
		return;
	}
	++probeFired;
	::usleep((useconds_t)probeUs);
}

uint32_t DarlingServer::Server::_drainRings() {
	// Snapshot the live thread set under the lock, then service OUTSIDE the lock: ringServiceThread
	// runs the full Call path (which can register/unregister rings, take Process/Thread locks, and
	// suspend microthreads) -- holding _ringThreadsLock across that would invite the dar-6x4
	// lock-across-suspend hazard class. The snapshot is shared_ptrs so the threads stay alive.
	std::vector<std::shared_ptr<Thread>> live;
	{
		std::unique_lock lock(_ringThreadsLock);
		live.reserve(_ringThreads.size());
		for (size_t i = 0; i < _ringThreads.size();) {
			auto t = _ringThreads[i].lock();
			if (!t) {
				_ringThreads.erase(_ringThreads.begin() + i);
			} else {
				live.push_back(std::move(t));
				++i;
			}
		}
	}
#ifdef DSERVER_RING_TRANSPORT
	{
		static bool installed = false;
		if (!installed) { ::signal(SIGUSR1, ringStateDumpHandler); installed = true; }
		if (g_ringStateDumpRequested) {
			g_ringStateDumpRequested = 0;
			fprintf(stderr, "STATE_DUMP threads=%zu\n", live.size());
			for (auto& t : live) {
				auto call = t->activeCall();
				const RingCallContext* ctx = call ? call->ringContext() : nullptr;
				fprintf(stderr,
				        "STATE_DUMP tid=%lld htid=%d call=%u lane=%llu seq=%u suspended=%d duplex=%d\n",
				        (long long)t->nsid(), t->id(), call ? (unsigned)call->number() : 0u,
				        (unsigned long long)((ctx && ctx->ring) ? ctx->ring->debugLaneId() : 0),
				        ctx ? ctx->seq : 0u, (int)t->isCurrentlySuspended(), (int)t->duplexInFlight());
			}
			// The ring-thread list only has threads that attached a lane. A stalled operation may be held
			// by a thread that has NO lane (a churned short-lived sender on the UDS path), so the dump
			// also walks every thread of the processes those ring threads belong to.
			std::vector<std::shared_ptr<Thread>> all;
			{
				std::set<void*> seen;
				for (auto& t : live) {
					auto proc = t->process();
					if (!proc) continue;
					for (auto& tt : proc->threads()) {
						if (!tt) continue;
						if (!seen.insert((void*)tt.get()).second) continue;
						all.push_back(tt);
					}
				}
			}
			fprintf(stderr, "STATE_DUMP_ALL threads=%zu\n", all.size());
			for (auto& t : all) {
				auto call = t->activeCall();
				const RingCallContext* ctx = call ? call->ringContext() : nullptr;
				fprintf(stderr,
				        "STATE_DUMP_ALL pnsid=%lld pid=%d tid=%lld htid=%d call=%u lane=%llu seq=%u suspended=%d duplex=%d ring=%d\n",
				        (long long)(t->process() ? t->process()->nsid() : 0),
				        (int)(t->process() ? t->process()->id() : -1),
				        (long long)t->nsid(), t->id(), call ? (unsigned)call->number() : 0u,
				        (unsigned long long)((ctx && ctx->ring) ? ctx->ring->debugLaneId() : 0),
				        ctx ? ctx->seq : 0u, (int)t->isCurrentlySuspended(), (int)t->duplexInFlight(),
				        (int)(t->ring() != nullptr));
			}
			fflush(stderr);
		}
	}
#endif
	uint32_t serviced = 0;
	for (auto& t : live) {
		// perf #18 P8 D3 (dar-1il.3.1.1): harvest any pending DUPLEX upcall reply for this thread
		// FIRST. A thread blocked in a duplex S2C upcall has its microthread fiber parked on its reply
		// semaphore; the guest pump publishes the reply into the ring mailbox. This is the main-loop
		// resume point: a correlated reply ups the semaphore so the fiber is rescheduled, all without a
		// dedicated waiter thread (the "scope the server wait to the op, don't block the dserver"
		// requirement). No-op for threads with nothing in flight. Counts as serviced so the spin budget
		// stays hot while a duplex roundtrip is mid-flight.
		if (t->drainDuplexReply()) {
			++serviced;
		}
		serviced += ringServiceThread(t);
	}
	if (serviced > 0) {
		Metrics::shared().ringServicedSpin.fetch_add(serviced, std::memory_order_relaxed);
	}
	return serviced;
};

void DarlingServer::Server::_resolveRingSpinBudget() {
	if (_ringSpinResolved) {
		return;
	}
	_ringSpinResolved = true;

	// Default budget by mode (the gist's low-power/balanced/latency). balanced is the default:
	// a short adaptive spin so a back-to-back RPC burst stays hot without burning a core on an
	// idle desktop. DARLING_SERVER_SPIN_US overrides the microsecond budget directly.
	uint64_t us = 20; // balanced default
	if (const char* mode = getenv("DARLING_SERVER_MODE")) {
		if (strcmp(mode, "low-power") == 0) {
			us = 0;      // straight to epoll; never poll
		} else if (strcmp(mode, "latency") == 0) {
			us = 200;    // aggressive spin (pinning recommended)
		} else {
			us = 20;     // balanced / unknown
		}
	}
	if (const char* env = getenv("DARLING_SERVER_SPIN_US")) {
		char* end = nullptr;
		unsigned long v = strtoul(env, &end, 10);
		if (end && *end == '\0') {
			us = (uint64_t)v;
		}
	}
	_ringSpinNs = us * 1000ull;

	// Announce the transport and scheduling mode once when the first ring attaches.
	static DarlingServer::Log ringLog("ring");
	// Emitted at error() level deliberately: the default log cutoff is Error, and a staged rollout
	// REQUIRES this notice be visible without raising the log level. It is a one-time NOTICE, not a
	// fault. (Worded as [NOTICE] so it doesn't read as a server error.)
	ringLog.error() << "[NOTICE] perf#18 shared-memory ring transport ACTIVE (experimental). spin_budget="
		<< (_ringSpinNs / 1000ull) << "us"
		<< " fiber_dispatch=on"
		<< ". Disable: rebuild without DSERVER_RING_TRANSPORT."
		<< " Mode via DARLING_SERVER_MODE=low-power|balanced|latency"
		<< " or DARLING_SERVER_SPIN_US=<n>." << ringLog.endLog;
};
#endif // DSERVER_RING_TRANSPORT

DarlingServer::Monitor::Monitor(std::shared_ptr<FD> descriptor, Event events, bool edgeTriggered, bool oneshot, std::function<void(std::shared_ptr<Monitor>, Event)> callback):
	_fd(descriptor),
	_userEvents(events),
	_events((uint32_t)events | (oneshot ? EPOLLONESHOT : 0) | (edgeTriggered ? EPOLLET : 0)),
	_callback(callback),
	_server(nullptr)
	{};

void DarlingServer::Monitor::enable(bool edgeTriggered, bool oneshot) {
	std::unique_lock lock(_lock);

	if (!_server) {
		return;
	}

	_events = (uint32_t)_userEvents;

	if (edgeTriggered) {
		_events |= EPOLLET;
	} else {
		_events &= ~EPOLLET;
	}

	if (oneshot) {
		_events |= EPOLLONESHOT;
	} else {
		_events &= ~EPOLLONESHOT;
	}

	struct epoll_event settings;
	settings.data.ptr = this;
	settings.events = _events;

	if (epoll_ctl(_server->_epollFD, EPOLL_CTL_MOD, _fd->fd(), &settings) < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to modify descriptor in epoll context");
	}
};

void DarlingServer::Monitor::disable() {
	std::unique_lock lock(_lock);

	if (!_server) {
		return;
	}

	_events = 0;

	struct epoll_event settings;
	settings.data.ptr = this;
	settings.events = _events;

	if (epoll_ctl(_server->_epollFD, EPOLL_CTL_MOD, _fd->fd(), &settings) < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to modify descriptor in epoll context");
	}
};

std::shared_ptr<DarlingServer::FD> DarlingServer::Monitor::fd() const {
	return _fd;
};

void DarlingServer::Server::sendMessage(Message&& message) {
	_outbox.push(std::move(message));
};
