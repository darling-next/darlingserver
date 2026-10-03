#include <darlingserver/debug-connection.hpp>
#include <darlingserver/debug-serialize.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace DarlingServer::Debug {

DebugConnection::~DebugConnection() {
	if (_fd >= 0) close(_fd);
}

void DebugConnection::respond(Collector& collector, FrameStatus status, size_t length, size_t budget) {
	_response = status == FrameStatus::TooLong
		? errorDocument("request_too_long", "request exceeds the ABI request bound", {}, budget)
		: handleRequestLine(collector, _request.data(), length, budget);
	_responding = true;
}

bool DebugConnection::handle(uint32_t events, Collector& collector, size_t budget) {
	if (events & EPOLLERR) return false;
	if (!_responding && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) {
		while (true) {
			const ssize_t got = recv(_fd, _request.data() + _requestLength,
				_request.size() - _requestLength, 0);
			if (got < 0) {
				if (errno == EINTR) continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
				return false;
			}
			if (got == 0) return false; // EOF before the required newline is not a frame.
			_requestLength += static_cast<size_t>(got);
			size_t length = 0;
			const FrameStatus status = frameRequest(_request.data(), _requestLength, got == 0, length);
			if (status != FrameStatus::NeedMore) {
				respond(collector, status, length, budget);
				break;
			}
		}
	}
	if (_responding) {
		while (_responseOffset < _response.size()) {
			const ssize_t sent = send(_fd, _response.data() + _responseOffset,
				_response.size() - _responseOffset, MSG_NOSIGNAL);
			if (sent < 0) {
				if (errno == EINTR) continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
				return false;
			}
			if (sent == 0) return false;
			_responseOffset += static_cast<size_t>(sent);
		}
		return false; // One complete response, then close.
	}
	return true;
}

DebugConnections::DebugConnections(int epollFD, Collector& collector, size_t budget):
	_epollFD(epollFD), _collector(collector), _budget(std::min(budget, kDefaultMaxResponseBytes)) {}

DebugConnections::~DebugConnections() {
	for (size_t i = 0; i < _active.size(); ++i) {
		if (_active[i]) remove(i);
	}
}

bool DebugConnections::addAccepted(int fd, Clock::time_point now) {
	struct ucred peer {};
	socklen_t peerLength = sizeof(peer);
	if (_count + _retiredCount >= kMaxConnections ||
		getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peerLength) != 0 ||
		peerLength != sizeof(peer) || !peerIsAuthorized(peer.uid, geteuid())) {
		close(fd);
		return false;
	}
	for (auto& slot: _active) {
		if (slot) continue;
		auto connection = std::make_unique<DebugConnection>(fd, now + kConnectionLifetime);
		epoll_event event {};
		event.data.ptr = connection.get();
		event.events = EPOLLIN | EPOLLRDHUP;
		if (epoll_ctl(_epollFD, EPOLL_CTL_ADD, fd, &event) != 0) return false;
		slot = std::move(connection);
		++_count;
		return true;
	}
	close(fd);
	return false;
}

void DebugConnections::acceptPending(int listener) {
	for (size_t accepted = 0; accepted < kMaxConnections; ++accepted) {
		const int fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno == EINTR) continue;
			return;
		}
		addAccepted(fd);
	}
}

void DebugConnections::remove(size_t index) {
	auto& connection = _active[index];
	// Delete before close, and retain the token until this returned batch ends.
	epoll_ctl(_epollFD, EPOLL_CTL_DEL, connection->_fd, nullptr);
	close(connection->_fd);
	connection->_fd = -1;
	_retired[_retiredCount++] = std::move(connection);
	--_count;
}

bool DebugConnections::handleEvent(const epoll_event& event, Clock::time_point now) {
	for (size_t i = 0; i < _active.size(); ++i) {
		auto& connection = _active[i];
		if (!connection || connection.get() != event.data.ptr) continue;
		if (now >= connection->_deadline || !connection->handle(event.events, _collector, _budget)) {
			remove(i);
		} else if (connection->_responding) {
			epoll_event settings {};
			settings.data.ptr = connection.get();
			// Do not keep RDHUP armed while writing: a normal SHUT_WR client
			// would otherwise spin the level-triggered loop under backpressure.
			settings.events = EPOLLOUT;
			if (epoll_ctl(_epollFD, EPOLL_CTL_MOD, connection->_fd, &settings) != 0) remove(i);
		}
		return true;
	}
	for (size_t i = 0; i < _retiredCount; ++i) {
		if (_retired[i].get() == event.data.ptr) return true;
	}
	return false;
}

void DebugConnections::finishEventBatch(Clock::time_point now) {
	// All returned events have been consumed: expired tokens can now be released.
	for (size_t i = 0; i < _active.size(); ++i) {
		if (_active[i] && now >= _active[i]->_deadline) remove(i);
	}
	for (size_t i = 0; i < _retiredCount; ++i) _retired[i].reset();
	_retiredCount = 0;
}

int DebugConnections::nextWaitMilliseconds(Clock::time_point now) const {
	int timeout = -1;
	for (const auto& connection: _active) {
		if (!connection) continue;
		const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(connection->_deadline - now).count();
		const int bounded = static_cast<int>(std::clamp<decltype(remaining)>(remaining, 0, INT_MAX));
		if (timeout < 0 || bounded < timeout) timeout = bounded;
	}
	return timeout;
}

bool socketNameForPrefix(const std::string& prefix, std::string& name, std::string& reason) {
	name.clear();
	struct stat identity {};
	if (stat(prefix.c_str(), &identity) != 0) {
		reason = "cannot stat debug prefix: " + std::string(strerror(errno));
		return false;
	}
	if (!S_ISDIR(identity.st_mode)) {
		reason = "debug prefix is not a directory";
		return false;
	}
	name = "darlingserver-debug:" + std::to_string(static_cast<uintmax_t>(geteuid())) + ":" +
		std::to_string(static_cast<uintmax_t>(identity.st_dev)) + ":" +
		std::to_string(static_cast<uintmax_t>(identity.st_ino));
	if (name.size() > sizeof(sockaddr_un::sun_path) - 1) {
		reason = "debug socket identity exceeds the abstract socket name bound";
		name.clear();
		return false;
	}
	reason.clear();
	return true;
}

}
