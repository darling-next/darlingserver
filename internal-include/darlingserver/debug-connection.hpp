#ifndef _DARLINGSERVER_DEBUG_CONNECTION_HPP_
#define _DARLINGSERVER_DEBUG_CONNECTION_HPP_

#include <darlingserver/debug-introspection.hpp>
#include <darlingserver/debug-serialize.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <sys/epoll.h>

namespace DarlingServer::Debug {

// The server and host endpoint use this same nonblocking I/O implementation.
// All methods run on the owning epoll context; collection never moves to a worker.
class DebugConnection {
public:
	explicit DebugConnection(int fd, std::chrono::steady_clock::time_point deadline):
		_fd(fd), _deadline(deadline) {}
	~DebugConnection();
	DebugConnection(const DebugConnection&) = delete;
	DebugConnection& operator=(const DebugConnection&) = delete;

private:
	friend class DebugConnections;
	int _fd;
	std::chrono::steady_clock::time_point _deadline;
	std::array<char, kMaxRequestBytes + 1> _request {};
	size_t _requestLength = 0;
	std::string _response;
	size_t _responseOffset = 0;
	bool _responding = false;

	bool handle(uint32_t events, Collector& collector, size_t budget);
	void respond(Collector& collector, FrameStatus status, size_t length, size_t budget);
};

class DebugConnections {
public:
	static constexpr size_t kMaxConnections = 16;
	using Clock = std::chrono::steady_clock;
	// Absolute lifetime: neither trickled input nor stalled output extends it.
	static constexpr auto kConnectionLifetime = std::chrono::seconds(30);
	DebugConnections(int epollFD, Collector& collector, size_t budget = kDefaultMaxResponseBytes);
	~DebugConnections();
	DebugConnections(const DebugConnections&) = delete;
	DebugConnections& operator=(const DebugConnections&) = delete;

	// Takes ownership even on rejection. fd must be nonblocking and CLOEXEC.
	bool addAccepted(int fd, Clock::time_point now = Clock::now());
	// Accept work is capped per callback, even under a continuous connection flood.
	void acceptPending(int listener);
	// Returns true for owned tokens, including removed tokens in this event batch.
	bool handleEvent(const epoll_event& event, Clock::time_point now = Clock::now());
	// Call only after consuming the entire epoll_wait batch. Retaining removed
	// objects until then prevents a stale token from identifying a new client.
	void finishEventBatch(Clock::time_point now = Clock::now());
	// Bound the existing epoll wait to the earliest expiry; -1 with no clients.
	int nextWaitMilliseconds(Clock::time_point now = Clock::now()) const;
	size_t size() const { return _count; }

private:
	int _epollFD;
	Collector& _collector;
	size_t _budget;
	size_t _count = 0;
	size_t _retiredCount = 0;
	std::array<std::unique_ptr<DebugConnection>, kMaxConnections> _active;
	std::array<std::unique_ptr<DebugConnection>, kMaxConnections> _retired;
	void remove(size_t index);
};

// A fixed-size identity independent of prefix spelling and path length.
// Failure is explicit: callers disable the listener, never truncate or fall back.
bool socketNameForPrefix(const std::string& prefix, std::string& name, std::string& reason);

}

#endif
