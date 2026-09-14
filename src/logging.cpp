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

#include <darlingserver/logging.hpp>
#include <darlingserver/server.hpp>
#include <darlingserver/thread.hpp>
#include <darlingserver/process.hpp>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define DEFAULT_LOG_CUTOFF DarlingServer::Log::Type::Error

static const char* alwaysLoggedCategories[] = {
	//"kprintf",
};

static int openLogDirectoryAt(int rootFD) {
	int current = dup(rootFD);
	if (current == -1) {
		return -1;
	}
	static const char* components[] = { "private", "var", "log" };
	for (const char* component : components) {
		struct stat status;
		if (fstatat(current, component, &status, AT_SYMLINK_NOFOLLOW) == -1) {
			close(current);
			return -1;
		}
		if (!S_ISDIR(status.st_mode)) {
			close(current);
			errno = ENOTDIR;
			return -1;
		}
		const int next = openat(
			current, component,
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (next == -1) {
			close(current);
			return -1;
		}
		struct stat opened;
		if (fstat(next, &opened) == -1 ||
			opened.st_dev != status.st_dev ||
			opened.st_ino != status.st_ino) {
			close(next);
			close(current);
			errno = EAGAIN;
			return -1;
		}
		close(current);
		current = next;
	}
	return current;
}

DarlingServer::Log::Log(std::string category):
	_category(category)
{
	for (size_t i = 0; i < sizeof(alwaysLoggedCategories) / sizeof(*alwaysLoggedCategories); ++i) {
		if (strcmp(alwaysLoggedCategories[i], _category.c_str()) == 0) {
			_alwaysLog = true;
			break;
		}
	}
};

DarlingServer::Log::Stream::Stream(Type type, const Log& log):
	_type(type),
	_log(log)
	{};

DarlingServer::Log::Stream::~Stream() {
	*this << endLog;
};

DarlingServer::Log::Stream& DarlingServer::Log::Stream::operator<<(EndLog value) {
	auto str = _buffer.str();
	if (!str.empty()) {
		_log._log(_type, str);
		_buffer.str(std::string());
		_buffer.clear();
	}
	return *this;
};

DarlingServer::Log::Stream& DarlingServer::Log::Stream::operator<<(const Loggable& loggable) {
	loggable.logToStream(*this);
	return *this;
};

DarlingServer::Log::Stream DarlingServer::Log::debug() const {
	return Stream(Type::Debug, *this);
};

DarlingServer::Log::Stream DarlingServer::Log::info() const {
	return Stream(Type::Info, *this);
};

DarlingServer::Log::Stream DarlingServer::Log::warning() const {
	return Stream(Type::Warning, *this);
};

DarlingServer::Log::Stream DarlingServer::Log::error() const {
	return Stream(Type::Error, *this);
};

std::string DarlingServer::Log::_typeToString(Type type) {
	switch (type) {
		case Type::Debug:
			return "Debug";
		case Type::Info:
			return "Info";
		case Type::Warning:
			return "Warning";
		case Type::Error:
			return "Error";
		default:
			return "Unknown";
	}
};

void DarlingServer::Log::_log(Type type, std::string message) const {
	// NOTE: we use POSIX file APIs because we want to append each message to the log file atomically,
	//       and as far as i can tell, C++ fstreams provide no such guarantee (that they won't write in chunks).
	static int logFile = []() {
		const int directory =
			openLogDirectoryAt(Server::sharedInstance().prefixFD());
		if (directory == -1) {
			return -1;
		}
		const int file = openat(
			directory, "dserver.log",
			O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		close(directory);
		return file;
	}();

	static bool logToStderr = []() {
		auto val = getenv("DSERVER_LOG_STDERR");
		return val && strlen(val) >= 1 && (val[0] == 't' || val[0] == 'T' || val[0] == '1');
	}();

	static Type logMinLevel = []() {
		auto val = getenv("DSERVER_LOG_LEVEL");
		Type level = DEFAULT_LOG_CUTOFF;
		if (val) {
			if (strncmp(val, "err", 3) == 0) {
				level = Type::Error;
			} else if (strncmp(val, "warn", 4) == 0) {
				level = Type::Warning;
			} else if (strncmp(val, "info", 4) == 0) {
				level = Type::Info;
			} else if (strncmp(val, "debug", 5) == 0) {
				level = Type::Debug;
			}
		}
		return level;
	}();

	if ((type < logMinLevel && !_alwaysLog) || logFile < 0) {
		return;
	}

	struct timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	double secs = (double)time.tv_sec + ((double)time.tv_nsec / 1.0e9);
	auto currentProcess = Process::currentProcess();
	auto currentThread = Thread::currentThread();
	std::string pid = currentProcess ? (std::string("[P:") + std::to_string(currentProcess->id()) + "(" + std::to_string(currentProcess->nsid()) + ")]") : "";
	std::string tid = currentThread ? (std::string("[T:") + std::to_string(currentThread->id()) + "(" + std::to_string(currentThread->nsid()) + ")]") : "";
	std::string messageToLog = "[" + std::to_string(secs) + "](" + _category + ", " + _typeToString(type) + ")" + pid + tid + " " + message + "\n";

	write(logFile, messageToLog.c_str(), messageToLog.size());

	if (logToStderr) {
		write(STDERR_FILENO, messageToLog.c_str(), messageToLog.size());
	}
};
