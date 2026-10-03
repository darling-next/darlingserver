#pragma once

#include <string>

namespace DarlingServer::TestDiagnostics {

bool enabled();
bool consumeFault(const char* name);
void traceLine(const std::string& line);
void traceSemaphoreCallBegin(
	const char* operation,
	int pid,
	int tid,
	unsigned int waitName,
	bool hasSignalName,
	unsigned int signalName,
	unsigned int sec,
	unsigned int nsec
);
void traceSemaphoreCallReply(
	const char* operation,
	int pid,
	int tid,
	unsigned int waitName,
	bool hasSignalName,
	unsigned int signalName,
	int resultCode
);
void tracePthreadCanceled(
	int pid,
	int tid,
	int action,
	bool stateKnown,
	bool disabledBefore,
	bool pendingBefore,
	bool canceledBefore,
	bool disabledAfter,
	bool pendingAfter,
	bool canceledAfter,
	int resultCode
);
void tracePthreadMarkcancel(
	int pid,
	int tid,
	unsigned int targetPort,
	bool targetKnown,
	bool disabledBefore,
	bool pendingBefore,
	bool canceledBefore,
	bool disabledAfter,
	bool pendingAfter,
	bool canceledAfter,
	int resultCode
);
void traceExecutablePath(int pid, int tid, const std::string& path, int resultCode);

}
