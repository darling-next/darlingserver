#include <darlingserver/test-diagnostics.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>
#include <fcntl.h>

namespace DarlingServer::TestDiagnostics {

static constexpr const char* TraceEnv = "DSERVER_TEST_TRACE_FILE";
static constexpr const char* FaultEnv = "DSERVER_TEST_FAULT_FILE";
static std::mutex traceMutex;

bool enabled() {
	return std::getenv(TraceEnv) != nullptr;
}

bool consumeFault(const char* name) {
	const char* path = std::getenv(FaultEnv);
	if (!path || path[0] == '\0' || !name || name[0] == '\0') {
		return false;
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	std::string hostPath;
	if (fd < 0 && path[0] == '/') {
		if (const char* prefix = std::getenv("DPREFIX"); prefix && prefix[0] != '\0') {
			hostPath = std::string(prefix) + path;
			fd = open(hostPath.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				hostPath = std::string(prefix) + "/libexec/darling" + path;
				fd = open(hostPath.c_str(), O_RDONLY | O_CLOEXEC);
			}
		}
	}
	if (fd < 0) {
		return false;
	}

	char buffer[128] = {};
	ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (count <= 0) {
		return false;
	}

	while (count > 0 && (buffer[count - 1] == '\n' || buffer[count - 1] == '\r' || buffer[count - 1] == ' ' || buffer[count - 1] == '\t')) {
		buffer[--count] = '\0';
	}

	if (std::strcmp(buffer, name) != 0) {
		return false;
	}

	if (!hostPath.empty()) {
		unlink(hostPath.c_str());
	} else {
		unlink(path);
	}
	traceLine(std::string("test_fault.consume name=") + name);
	return true;
}

void traceLine(const std::string& line) {
	const char* path = std::getenv(TraceEnv);
	if (!path || path[0] == '\0') {
		return;
	}

	std::string record = line;
	record.push_back('\n');

	std::lock_guard lock(traceMutex);
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
	if (fd < 0) {
		return;
	}

	const char* cursor = record.data();
	size_t remaining = record.size();
	while (remaining > 0) {
		ssize_t written = write(fd, cursor, remaining);
		if (written <= 0) {
			break;
		}
		cursor += written;
		remaining -= static_cast<size_t>(written);
	}

	close(fd);
}

}
