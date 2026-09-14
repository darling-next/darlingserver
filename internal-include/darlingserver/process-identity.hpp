/**
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling developers
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef _DARLINGSERVER_PROCESS_IDENTITY_HPP_
#define _DARLINGSERVER_PROCESS_IDENTITY_HPP_

#include <sys/types.h>

namespace DarlingServer::ProcessIdentity {
	constexpr pid_t initNamespaceID = 1;

	// In rootless no-PID-namespace mode launchd has a host PID, but it remains
	// the guest's init process. Trust the server-known peer identity rather than
	// a client-provided PID when assigning that one guest namespace ID.
	constexpr pid_t namespaceIDForPeer(pid_t rootlessInitHostPID, pid_t peerHostPID, pid_t reportedNamespaceID) {
		if (rootlessInitHostPID == 0) {
			return reportedNamespaceID;
		}
		if (peerHostPID == rootlessInitHostPID) {
			return initNamespaceID;
		}
		return reportedNamespaceID == initNamespaceID ? peerHostPID : reportedNamespaceID;
	}

	// Normally a process leader has the same namespace thread and process IDs.
	// The rootless init is intentionally an exception: its public namespace ID
	// is 1 while its kernel thread ID remains its host process ID.
	constexpr bool isMainThread(pid_t threadNamespaceID, pid_t threadHostID, pid_t processNamespaceID, pid_t processHostID) {
		return threadNamespaceID == processNamespaceID
			|| (processNamespaceID == initNamespaceID && threadHostID == processHostID);
	}
};

#endif // _DARLINGSERVER_PROCESS_IDENTITY_HPP_
