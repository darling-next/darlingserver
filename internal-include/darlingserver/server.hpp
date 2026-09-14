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

#ifndef _DARLINGSERVER_SERVER_HPP_
#define _DARLINGSERVER_SERVER_HPP_

#include <string>
#include <sys/epoll.h>
#include <thread>

#include <darlingserver/message.hpp>
#include <darlingserver/workers.hpp>
#include <darlingserver/call.hpp>
#include <darlingserver/process-identity.hpp>
#include <darlingserver/registry.hpp>
#include <darlingserver/utility.hpp>
#include <darlingserver/monitor.hpp>

namespace DarlingServer {
	// NOTE: server instances MUST be created with `new` rather than as a normal local/stack variable
	class Server {
		friend class Monitor;

	private:
		int _listenerSocket;
		std::string _prefix;
		int _prefixFD;
		pid_t _rootlessInitHostPID;
		std::string _socketPath;
		// perf #0 (dar-dar6x4-perf-5dq.6): dedicated stat socket. A SOCK_STREAM listener in
		// the ABSTRACT namespace (the server is in a private mount namespace, so a pathname
		// socket is unreachable from the host; the network namespace is shared, so an
		// abstract name works). On accept the server writes a one-shot JSON metrics snapshot
		// and closes. Lives on the same epoll loop, so producing the snapshot is single-
		// threaded and reads the atomic counters consistently. -1 if setup failed (the
		// server keeps running regardless -- metrics are best-effort observability).
		int _statListenerSocket = -1;
		std::string _statSocketPath; // abstract name (without the leading NUL)
		int _epollFD;
		MessageQueue _inbox;
		MessageQueue _outbox;
		WorkQueue<std::shared_ptr<Thread>> _workQueue;
		bool _canRead = false;
		bool _canWrite = true;
		int _wakeupFD;
		int _timerFD;
		uint64_t _currentTimerDeadline = 0;
		std::mutex _timerLock;
		std::vector<std::shared_ptr<Monitor>> _monitors;
		std::vector<std::shared_ptr<Monitor>> _monitorsWaitingToDie;
		std::mutex _monitorsLock;

		void _worker(std::shared_ptr<Thread> thread);

		// perf #0: accept one stat client and write the JSON snapshot.
		void _handleStatConnection();

		friend struct ::DTapeHooks;

	public:
		Server(
			std::string prefix,
			int prefixFD,
			pid_t rootlessInitHostPID = 0);
		~Server();

		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;
		Server(Server&&) = delete;
		Server& operator=(Server&&) = delete;

		void start();

		void monitorProcess(std::shared_ptr<Process> process);

		std::string prefix() const;
		int prefixFD() const;
		pid_t namespaceIDForPeer(pid_t peerHostPID, pid_t reportedNamespaceID) const;

		static Server& sharedInstance();

		void scheduleThread(std::shared_ptr<Thread> thread);

		void addMonitor(std::shared_ptr<Monitor> monitor);
		void removeMonitor(std::shared_ptr<Monitor> monitor);

		void sendMessage(Message&& message);
	};
};

#endif // _DARLINGSERVER_SERVER_HPP_
