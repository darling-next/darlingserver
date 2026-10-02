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
#include <darlingserver/registry.hpp>
#include <sys/eventfd.h>
#include <darlingserver/duct-tape.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <darlingserver/logging.hpp>
#include <darlingserver/metrics.hpp>

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
		newSpec.it_value.tv_sec = deadline_ns / 1000000000ull;
		newSpec.it_value.tv_nsec = deadline_ns % 1000000000ull;

		std::unique_lock lock(server._timerLock);

		if (!override && server._currentTimerDeadline != 0 && deadline_ns >= server._currentTimerDeadline) {
			return;
		}

		server._currentTimerDeadline = deadline_ns;

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
	// why abstract and not a pathname). Keyed off the prefix so distinct prefixes differ.
	_statSocketPath("darlingserver-stat:" + _prefix),
	_workQueue(std::bind(&Server::_worker, this, std::placeholders::_1))
{
	sharedInstancePointer = this;

	// perf #0 (dar-dar6x4-perf-5dq.6): record the server start time for uptime.
	Metrics::shared().startMonoUs = Metrics::nowMonoUs();

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
			}
		}
	}
};

DarlingServer::Server::~Server() {
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
				try {
					auto call = DarlingServer::Call::callFromMessage(std::move(*msg));
					if (call) {
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
		int ret = epoll_wait(_epollFD, events, 16, -1);

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
			} else if (event->data.ptr == &_statListenerSocket) {
				// perf #0 (dar-dar6x4-perf-5dq.6): a metrics client connected; serve the
				// snapshot. Drain all pending connections (level-triggered would re-fire,
				// but we accept in a loop to be safe and cheap).
				_handleStatConnection();
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
