#include <darlingserver/kqchan.hpp>
#include <darlingserver/server.hpp>
#include <darlingserver/rpc-supplement.h>
#include <darlingserver/thread.hpp>
#include <darlingserver/logging.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdarg>

static DarlingServer::Log kqchanLog("kqchan");
static DarlingServer::Log kqchanMachPortLog("kqchan:mach_port");
static DarlingServer::Log kqchanProcLog("kqchan:proc");
static std::atomic_uint64_t kqchanDebugIDCounter = 0;

// A0 (perf#25a-hang) INSTRUMENTATION -- env-gated (DARLING_SERVER_AUXLOG=1) compact per-connection
// trace of the SEQPACKET kqchan aux channel: the ONLY SEQPACKET channel between guest and server
// (kqchan.cpp socketpair), which the per-fd HANG capture localized to (guest fd 14/39 hold a
// stranded 12-byte notification while every guest thread is parked in a DGRAM RPC recvmsg). Goal:
// name WHICH kqchan op strands and WHY the notification is never acked/drained under a fork storm.
// Zero cost when the env is unset (single relaxed-load bool guard). Writes raw to stderr so it is
// independent of DSERVER_LOG_LEVEL. NOT default-on; reverted before any non-instrumented build.
static bool __auxlog_enabled() {
	static std::atomic<int> cached{-1};
	int v = cached.load(std::memory_order_relaxed);
	if (v < 0) {
		const char* e = getenv("DARLING_SERVER_AUXLOG");
		v = (e && e[0] == '1') ? 1 : 0;
		cached.store(v, std::memory_order_relaxed);
	}
	return v != 0;
}

// Open the aux-log file once. The server is a daemon (stderr may be detached), so we write to a
// stable file under the prefix's log dir -- same convention as dserver.log. O_APPEND makes each
// write() atomic across the server's threads.
static int __auxlog_fd() {
	static int fd = []() -> int {
		if (!__auxlog_enabled()) {
			return -1;
		}
		std::string path = DarlingServer::Server::sharedInstance().prefix() + "/private/var/log/dserver-auxlog.txt";
		return open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	}();
	return fd;
}

__attribute__((format(printf, 1, 2)))
static void auxlog(const char* fmt, ...) {
	if (!__auxlog_enabled()) {
		return;
	}
	int fd = __auxlog_fd();
	if (fd < 0) {
		return;
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	char line[512];
	int n = snprintf(line, sizeof(line), "[AUXLOG %ld.%06ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000);
	va_list ap;
	va_start(ap, fmt);
	if (n >= 0 && (size_t)n < (int)sizeof(line)) {
		int m = vsnprintf(line + n, sizeof(line) - n, fmt, ap);
		if (m >= 0) {
			n += m;
		}
	}
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(line)) {
		n = sizeof(line) - 1;
	}
	line[n++] = '\n';
	// single O_APPEND write() so lines from concurrent server threads never interleave mid-line
	(void)!write(fd, line, n);
}

//
// base class
//

DarlingServer::Kqchan::Kqchan(std::shared_ptr<DarlingServer::Process> process):
	_debugID(kqchanDebugIDCounter++),
	_process(process)
{
	kqchanLog.debug() << *this << ": Constructing kqchan" << kqchanLog.endLog;
};

DarlingServer::Kqchan::~Kqchan() {
	kqchanLog.debug() << *this << ": Destroying kqchan" << kqchanLog.endLog;
};

uintptr_t DarlingServer::Kqchan::_idForProcess() const {
	throw std::runtime_error("must be overridden in derived class");
};

void DarlingServer::Kqchan::_processMessages() {
	throw std::runtime_error("must be overridden in derived class");
};

std::shared_ptr<DarlingServer::Kqchan> DarlingServer::Kqchan::sharedFromRoot() {
	throw std::runtime_error("must be overridden in derived class");
};

int DarlingServer::Kqchan::setup() {
	int fds[2];

	kqchanLog.debug() << *this << ": Setting up kqchan" << kqchanLog.endLog;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) < 0) {
		int ret = errno;
		kqchanLog.debug() << "Failed to create socket pair" << kqchanLog.endLog;
		throw std::system_error(ret, std::generic_category());
	}

	// we'll keep fds[0] and give fds[1] away
	_socket = std::make_shared<FD>(fds[0]);

	kqchanLog.debug() << *this << ": Keeping socket " << fds[0] << " and giving away " << fds[1] << kqchanLog.endLog;

	// set O_NONBLOCK on our socket
	int flags = fcntl(_socket->fd(), F_GETFL);
	if (flags < 0) {
		int ret = errno;
		throw std::system_error(ret, std::generic_category());
	}
	if (fcntl(_socket->fd(), F_SETFL, flags | O_NONBLOCK) < 0) {
		int ret = errno;
		throw std::system_error(ret, std::generic_category());
	}

	std::weak_ptr<Kqchan> weakThis = sharedFromRoot();

	_outbox.setMessageArrivalNotificationCallback([weakThis]() {
		auto self = weakThis.lock();

		if (!self) {
			return;
		}

		std::unique_lock lock(self->_sendingMutex);

		kqchanLog.debug() << *self << ": Got messages to send, attempting to send them" << kqchanLog.endLog;

		// we probably won't be sending very many messages at once;
		// we can send all the messages in the same context that they were pushed
		do {
			self->_canSend = self->_outbox.sendMany(self->_socket->fd());
		} while (self->_canSend && !self->_outbox.empty());
		auxlog("outbox.flush(arrival) id=%llu socket_fd=%d canSend=%d outbox_empty=%d",
			(unsigned long long)self->_debugID, self->_socket->fd(),
			(int)self->_canSend, (int)self->_outbox.empty());
	});

	_monitor = std::make_shared<Monitor>(_socket, Monitor::Event::Readable | Monitor::Event::Writable | Monitor::Event::HangUp, true, false, [weakThis](std::shared_ptr<Monitor> monitor, Monitor::Event event) {
		auto self = weakThis.lock();

		if (!self) {
			return;
		}

		kqchanLog.debug() << *self << ": Got event(s) on socket: " << static_cast<uint64_t>(event) << kqchanLog.endLog;

		if (static_cast<uint64_t>(event & Monitor::Event::HangUp) != 0) {
			// socket hangup (peer closed their socket)

			kqchanLog.debug() << *self << ": Peer hung up their socket; cleaning up monitor and kqchan" << kqchanLog.endLog;

			auxlog("socket HANGUP id=%llu socket_fd=%d outbox_empty=%d (guest closed the channel)",
				(unsigned long long)self->_debugID, self->_socket->fd(), (int)self->_outbox.empty());

			// stop monitoring the socket (we're not gonna get any more events out of it)
			Server::sharedInstance().removeMonitor(monitor);

			// and unregister ourselves from the process (if it still exists)
			// (which means the instance should be freed once we return)
			if (auto process = self->_process.lock()) {
				process->unregisterKqchan(self);
			}

			// no need to process any other events that may have occurred
			return;
		}

		if (static_cast<uint64_t>(event & Monitor::Event::Readable) != 0) {
			// incoming messages

			kqchanLog.debug() << *self << ": socket has pending incoming messages" << kqchanLog.endLog;

			auxlog("socket READABLE id=%llu socket_fd=%d (guest sent us a request; will receiveMany + process)",
				(unsigned long long)self->_debugID, self->_socket->fd());

			// receive them all
			while (self->_inbox.receiveMany(self->_socket->fd()));

			// process the messages
			self->_processMessages();
		}

		if (static_cast<uint64_t>(event & Monitor::Event::Writable) != 0) {
			// we can now send messages again;
			// send as many messages as we can

			std::unique_lock lock(self->_sendingMutex);

			kqchanLog.debug() << *self << ": socket is now writable; sending all pending outgoing messages" << kqchanLog.endLog;
			do {
				self->_canSend = self->_outbox.sendMany(self->_socket->fd());
			} while (self->_canSend  && !self->_outbox.empty());
			auxlog("socket WRITABLE id=%llu socket_fd=%d canSend=%d outbox_empty=%d (backpressure relieved)",
				(unsigned long long)self->_debugID, self->_socket->fd(),
				(int)self->_canSend, (int)self->_outbox.empty());
		}
	});

	DarlingServer::Server::sharedInstance().addMonitor(_monitor);

	return fds[1];
};

void DarlingServer::Kqchan::_sendNotification() {
	std::unique_lock lock(_notificationMutex);

	kqchanLog.debug() << *this << ": received request to send notification" << kqchanLog.endLog;

	if (!_canSendNotification) {
		// we've already sent our peer a notification that they haven't acknowledged yet;
		// let's not send another and needlessly clog up the socket
		kqchanLog.debug() << *this << ": earlier notification has not yet been acknowledged; not sending another notification" << kqchanLog.endLog;
		auxlog("sendNotif GATED id=%llu socket_fd=%d (prior notification unacked; suppressed)",
			(unsigned long long)_debugID, _socket ? _socket->fd() : -1);
		return;
	}

	// perf#25a-hang (A0): if notifications are currently deferred (a _read is in flight and will send
	// its reply first, to keep channel messages in-order), DON'T consume _canSendNotification and DON'T
	// stash a Message. Just record that a notification is owed. _sendDeferredNotification will re-drive
	// _sendNotification once deferral clears, at which point it takes the real send path below. The old
	// code consumed _canSendNotification here AND stashed into _deferredNotification; if the _read that
	// was supposed to flush the stash had already run (the two are serialized only by _notificationMutex,
	// not by program order across the split _mutex/_notificationMutex), the stash was orphaned and the
	// gate stayed closed forever -> every future notification GATED -> guest never re-notified -> never
	// acks -> deadlock (the brew fork-storm hang: id=259 stuck at canSendNotif=0 with a growing queue).
	if (_deferNotification) {
		_notificationRequestedWhileDeferred = true;
		auxlog("sendNotif DEFER id=%llu socket_fd=%d (owed; will re-arm when deferral clears)",
			(unsigned long long)_debugID, _socket ? _socket->fd() : -1);
		return;
	}

	kqchanLog.debug() << *this << ": sending notification " << _notificationCount << kqchanLog.endLog;

	// now that we're sending the notification, we shouldn't send another one until our peer acknowledges this one
	_canSendNotification = false;

	Message msg(sizeof(dserver_kqchan_call_notification_t), 0);

	auto notification = reinterpret_cast<dserver_kqchan_call_notification_t*>(msg.data().data());
	notification->header.number = dserver_kqchan_msgnum_notification;
	notification->header.pid = 0;
	notification->header.tid = 0;

	auxlog("sendNotif PUSH  id=%llu socket_fd=%d count=%llu -> outbox (12B notification)",
		(unsigned long long)_debugID, _socket ? _socket->fd() : -1,
		(unsigned long long)_notificationCount);
	lock.unlock(); // the outbox has its own lock
	_outbox.push(std::move(msg));
};

bool DarlingServer::Kqchan::_hasPendingEvents() {
	// base channels have no server-side event queue to inspect; the mach-port channel drives its own
	// re-notification through _checkForEventsAsync after each read reply, so returning false here keeps
	// its behavior unchanged. The proc channel overrides this to report a non-empty _events queue.
	return false;
}

void DarlingServer::Kqchan::_sendDeferredNotification() {
	std::unique_lock lock(_notificationMutex);

	_deferNotification = false;

	// perf#25a-hang (A0): deferral just cleared. Re-arm a notification if one was requested during the
	// deferral, OR if the channel still has pending events (level-triggered: this recovers a notification
	// that a defer/gate race would otherwise have dropped). We drop _notificationMutex before calling
	// _sendNotification (which re-takes it) and before _hasPendingEvents (which takes _mutex) to avoid
	// self-deadlock / lock-order inversion. A spurious notification is harmless by design: the guest
	// reads, finds nothing, and drops the event (0xdead).
	bool owed = _notificationRequestedWhileDeferred;
	_notificationRequestedWhileDeferred = false;

	// migrate any Message stashed by an older build's path (defensive; the new path never stashes)
	if (_deferredNotification) {
		Message notification(std::move(*_deferredNotification));
		_deferredNotification = std::nullopt;
		auxlog("sendDeferred PUSH id=%llu socket_fd=%d -> outbox (12B legacy deferred notification)",
			(unsigned long long)_debugID, _socket ? _socket->fd() : -1);
		lock.unlock();
		_outbox.push(std::move(notification));
		return;
	}

	lock.unlock();

	if (owed || _hasPendingEvents()) {
		auxlog("sendDeferred REARM id=%llu socket_fd=%d owed=%d (re-driving _sendNotification)",
			(unsigned long long)_debugID, _socket ? _socket->fd() : -1, (int)owed);
		_sendNotification();
	}
};

void DarlingServer::Kqchan::logToStream(Log::Stream& stream) const {
	auto proc = _process.lock();
	stream << "[KQ:" << _debugID << ":";
	if (proc) {
		stream << *proc;
	} else {
		stream << "<null>";
	}
	stream << "]";
};

//
// mach port
//

DarlingServer::Kqchan::MachPort::MachPort(std::shared_ptr<DarlingServer::Process> process, uint32_t port, uint64_t receiveBuffer, uint64_t receiveBufferSize, uint64_t savedFilterFlags):
	Kqchan(process),
	_port(port),
	_receiveBuffer(receiveBuffer),
	_receiveBufferSize(receiveBufferSize),
	_savedFilterFlags(savedFilterFlags)
{
	kqchanMachPortLog.debug() << *this << ": Constructing Mach port kqchan" << kqchanMachPortLog.endLog;
};

DarlingServer::Kqchan::MachPort::~MachPort() {
	kqchanMachPortLog.debug() << *this << ": Destroying Mach port kqchan" << kqchanMachPortLog.endLog;

	if (_dtapeKqchan) {
		auto kqchan = _dtapeKqchan;

		// disable notifications so that `this` won't be used after we're destroyed
		dtape_kqchan_mach_port_disable_notifications(kqchan);

		// and schedule the duct-taped kqchan to be destroyed on a microthread
		auto debugID = _debugID;
		Thread::kernelAsync([=]() {
			kqchanMachPortLog.debug() << "Destroying duct-taped Mach port kqchan with ID " << debugID << kqchanMachPortLog.endLog;
			dtape_kqchan_mach_port_destroy(kqchan);
		});
	}
};

uintptr_t DarlingServer::Kqchan::MachPort::_idForProcess() const {
	return reinterpret_cast<uintptr_t>(this);
};

std::shared_ptr<DarlingServer::Kqchan> DarlingServer::Kqchan::MachPort::sharedFromRoot() {
	return shared_from_this();
};

int DarlingServer::Kqchan::MachPort::setup() {
	auto proc = _process.lock();

	if (!proc) {
		throw std::system_error(ESRCH, std::generic_category());
	}

	kqchanMachPortLog.debug() << *this << ": Setting up Mach port kqchan" << kqchanMachPortLog.endLog;

	// NOTE: the duct-taped kqchan will never notify us after we die
	//       since we disable notifications upon destruction,
	//       so using `this` here is safe
	_dtapeKqchan = dtape_kqchan_mach_port_create(proc->_dtapeTask, _port, _receiveBuffer, _receiveBufferSize, _savedFilterFlags, [](void* context) {
		auto self = reinterpret_cast<MachPort*>(context);
		self->_notify();
	}, this);
	if (!_dtapeKqchan) {
		kqchanMachPortLog.debug() << *this << ": Failed to create duct-taped Mach port kqchan for port " << _port << kqchanMachPortLog.endLog;
		throw std::system_error(ESRCH, std::generic_category());
	}

	int fd = Kqchan::setup();

	if (dtape_kqchan_mach_port_has_events(_dtapeKqchan)) {
		// if we already have an event, notify the kqchan;
		// this will simply enqueue a message to be sent to the peer.
		// since our libkqueue filter uses level-triggered epoll,
		// our peer will immediately see there's an event available when it starts waiting.
		_notify();
	}

	return fd;
};

void DarlingServer::Kqchan::MachPort::_processMessages() {
	while (auto msg = _inbox.pop()) {
		if (msg->data().size() < sizeof(dserver_kqchan_callhdr_t)) {
			throw std::invalid_argument("Message buffer was too small for kqchan call header");
		}

		auto callhdr = reinterpret_cast<dserver_kqchan_callhdr_t*>(msg->data().data());

		switch (callhdr->number) {
			case dserver_kqchan_msgnum_mach_port_modify: {
				if (msg->data().size() < sizeof(dserver_kqchan_call_mach_port_modify_t)) {
					throw std::invalid_argument("Message buffer was too small for dserver_kqchan_msgnum_mach_port_modify");
				}

				auto modify = reinterpret_cast<dserver_kqchan_call_mach_port_modify_t*>(callhdr);

				_modify(modify->receive_buffer, modify->receive_buffer_size, modify->saved_filter_flags, modify->header.tid);
			} break;

			case dserver_kqchan_msgnum_mach_port_read: {
				if (msg->data().size() < sizeof(dserver_kqchan_call_mach_port_read_t)) {
					throw std::invalid_argument("Message buffer was too small for dserver_kqchan_call_mach_port_read");
				}

				auto read = reinterpret_cast<dserver_kqchan_call_mach_port_read_t*>(callhdr);

				_read(read->default_buffer, read->default_buffer_size, read->header.tid);
			} break;

			default:
				throw std::invalid_argument("Unknown/invalid kqchan msgnum");
		}
	}
};

void DarlingServer::Kqchan::MachPort::_modify(uint64_t receiveBuffer, uint64_t receiveBufferSize, uint64_t savedFilterFlags, pid_t nstid) {
	kqchanMachPortLog.debug() << *this << ": Received modification request with {receiveBuffer=" << receiveBuffer << ",receiveBufferSize=" << receiveBufferSize << ",savedFilterFlags=" << savedFilterFlags << "}" << kqchanMachPortLog.endLog;

	auto maybeThread = threadRegistry().lookupEntryByNSID(nstid);

	if (!maybeThread) {
		throw std::runtime_error("No thread for Mach port kqchan modification?");
	}

	auto thread = *maybeThread;

	auto self = shared_from_this();
	Thread::kernelAsync([self, thread, receiveBuffer, receiveBufferSize, savedFilterFlags]() {
		kqchanMachPortLog.debug() << *self << ": Handling modification request in microthread" << kqchanMachPortLog.endLog;

		Thread::currentThread()->impersonate(thread);
		dtape_kqchan_mach_port_modify(self->_dtapeKqchan, receiveBuffer, receiveBufferSize, savedFilterFlags);
		Thread::currentThread()->impersonate(nullptr);

		Message msg(sizeof(dserver_kqchan_reply_mach_port_modify_t), 0, self->_checkForEventsAsyncFactory());

		auto reply = reinterpret_cast<dserver_kqchan_reply_mach_port_modify_t*>(msg.data().data());

		reply->header.number = dserver_kqchan_msgnum_mach_port_modify;
		reply->header.code = 0;

		kqchanMachPortLog.debug() << *self << ": Sending modification reply/acknowledgement" << kqchanMachPortLog.endLog;

		self->_outbox.push(std::move(msg));
	});
};

void DarlingServer::Kqchan::MachPort::_read(uint64_t defaultBuffer, uint64_t defaultBufferSize, pid_t nstid) {
	kqchanMachPortLog.debug() << *this << ": received read request with {defaultBuffer=" << defaultBuffer << ",defaultBufferSize=" << defaultBufferSize << "}" << kqchanMachPortLog.endLog;

	{
		// our peer has acknowledged our notification by asking for the pending messages;
		// we can now send a notification again if we receive more data
		std::unique_lock lock(_notificationMutex);
		kqchanMachPortLog.debug() << *this << ": received acknowledgement (implicitly via read) for notification " << _notificationCount++ << "; notifications may now be sent" << kqchanMachPortLog.endLog;
		_canSendNotification = true;

		// defer future notifications until we send our reply.
		// we do it this way because:
		// 1) if we don't defer them and just send them right now,
		//    a notification may be sent before we send our reply,
		//    leading to out-of-order messages in the channel (which
		//    causes an abort on the client side).
		// 2) if we instead move the `_canSendNotification` update to after
		//    we send the reply, we may miss a notification for an event
		//    that occurred right after we generated the reply but before
		//    we updated `_canSendNotification`.
		// with this approach (notification deferral), channel messages are kept in-order
		// and we don't miss any notifications. worst case scenario, we might send
		// a duplicate notification for an event that occurred right after this update
		// but before we generate the reply; in that case, the client will simply try to read
		// the duplicate event but we won't have anything and we'll tell it to drop the event.
		_deferNotification = true;
	}

	auto maybeThread = threadRegistry().lookupEntryByNSID(nstid);

	if (!maybeThread) {
		throw std::runtime_error("No thread for Mach port kqchan read?");
	}

	auto thread = *maybeThread;

	auto self = shared_from_this();
	Thread::kernelAsync([self, thread, defaultBuffer, defaultBufferSize]() {
		Message msg(sizeof(dserver_kqchan_reply_mach_port_read_t), 0, self->_checkForEventsAsyncFactory());

		kqchanMachPortLog.debug() << *self << ": handling read request in microthread" << kqchanMachPortLog.endLog;

		auto reply = reinterpret_cast<dserver_kqchan_reply_mach_port_read_t*>(msg.data().data());

		reply->header.code = 0;
		reply->header.number = dserver_kqchan_msgnum_mach_port_read;

		Thread::currentThread()->impersonate(thread);
		if (!dtape_kqchan_mach_port_fill(self->_dtapeKqchan, reply, defaultBuffer, defaultBufferSize)) {
			kqchanMachPortLog.debug() << *self << ": no events to read" << kqchanMachPortLog.endLog;
			reply->header.code = 0xdead;
		}
		Thread::currentThread()->impersonate(nullptr);

		self->_outbox.push(std::move(msg));

		// now let's send any deferred notifications we might have
		self->_sendDeferredNotification();
	});
};

void DarlingServer::Kqchan::MachPort::_notify() {
	_sendNotification();
};

void DarlingServer::Kqchan::MachPort::_checkForEventsAsync() {
	Thread::kernelAsync([weakSelf = weak_from_this()]() {
		auto self = weakSelf.lock();
		if (!self) {
			return;
		}
		// if we have more events ready, notify our peer
		if (dtape_kqchan_mach_port_has_events(self->_dtapeKqchan)) {
			self->_notify();
		}
	});
};

std::function<void()> DarlingServer::Kqchan::MachPort::_checkForEventsAsyncFactory() {
	return [weakSelf = weak_from_this()]() {
		auto self = weakSelf.lock();
		if (!self) {
			return;
		}
		self->_checkForEventsAsync();
	};
};

//
// process
//
// TODO: NOTE_REAP and NOTE_SIGNAL
//

DarlingServer::Kqchan::Process::Process(std::shared_ptr<DarlingServer::Process> process, pid_t nspid, uint32_t flags):
	Kqchan(process),
	_nspid(nspid),
	_flags(flags)
{
	kqchanProcLog.debug() << *this << ": Constructing process kqchan" << kqchanProcLog.endLog;
};

DarlingServer::Kqchan::Process::~Process() {
	kqchanProcLog.debug() << *this << ": Destroying process kqchan" << kqchanProcLog.endLog;

	if (auto targetProcess = _targetProcess.lock()) {
		targetProcess->unregisterListeningKqchan(_idForProcess());
	}
};

uintptr_t DarlingServer::Kqchan::Process::_idForProcess() const {
	return reinterpret_cast<uintptr_t>(this);
};

std::shared_ptr<DarlingServer::Kqchan> DarlingServer::Kqchan::Process::sharedFromRoot() {
	return shared_from_this();
};

int DarlingServer::Kqchan::Process::setup() {
	kqchanProcLog.debug() << *this << ": Setting up process kqchan" << kqchanProcLog.endLog;

	auto maybeTargetProcess = processRegistry().lookupEntryByNSID(_nspid);
	if (!maybeTargetProcess) {
		kqchanProcLog.debug() << *this << ": Failed to create process kqchan for PID " << _nspid << kqchanProcLog.endLog;
		throw std::system_error(ESRCH, std::generic_category());
	}

	auto targetProcess = *maybeTargetProcess;

	_targetProcess = targetProcess;

	int fd = Kqchan::setup();

	{
		auto lp = _process.lock();
		auxlog("proc.setup id=%llu server_socket_fd=%d guest_gets_fd=%d listener_nsid=%d target_nspid=%d flags=0x%x",
			(unsigned long long)_debugID, _socket ? _socket->fd() : -1, fd,
			lp ? (int)lp->nsid() : -1, (int)_nspid, (unsigned)_flags);
	}

	{
		std::unique_lock lock(_mutex);
		if (!_attached) {
			targetProcess->registerListeningKqchan(shared_from_this());
			_attached = true;
		}
		if (!_events.empty()) {
			_sendNotification();
		}
	}

	return fd;
};

void DarlingServer::Kqchan::Process::_processMessages() {
	while (auto msg = _inbox.pop()) {
		if (msg->data().size() < sizeof(dserver_kqchan_callhdr_t)) {
			throw std::invalid_argument("Message buffer was too small for kqchan call header");
		}

		auto callhdr = reinterpret_cast<dserver_kqchan_callhdr_t*>(msg->data().data());

		switch (callhdr->number) {
			case dserver_kqchan_msgnum_proc_modify: {
				if (msg->data().size() < sizeof(dserver_kqchan_call_proc_modify_t)) {
					throw std::invalid_argument("Message buffer was too small for dserver_kqchan_call_proc_modify_");
				}

				auto modify = reinterpret_cast<dserver_kqchan_call_proc_modify_t*>(callhdr);

				_modify(modify->flags);
			} break;

			case dserver_kqchan_msgnum_proc_read: {
				if (msg->data().size() < sizeof(dserver_kqchan_call_proc_read_t)) {
					throw std::invalid_argument("Message buffer was too small for dserver_kqchan_call_proc_read");
				}

				auto read = reinterpret_cast<dserver_kqchan_call_proc_read_t*>(callhdr);

				_read();
			} break;

			default:
				throw std::invalid_argument("Unknown/invalid kqchan msgnum");
		}
	}
};

void DarlingServer::Kqchan::Process::_modify(uint32_t flags) {
	std::unique_lock lock(_mutex);

	_flags = flags;

	Message msg(sizeof(dserver_kqchan_reply_proc_modify_t), 0, _checkForEventsAsyncFactory());

	auto reply = reinterpret_cast<dserver_kqchan_reply_proc_modify_t*>(msg.data().data());

	reply->header.number = dserver_kqchan_msgnum_proc_modify;
	reply->header.code = 0;

	kqchanProcLog.debug() << *this << ": Sending modification reply/acknowledgement" << kqchanProcLog.endLog;

	lock.unlock(); // the outbox has its own lock
	_outbox.push(std::move(msg));
};

void DarlingServer::Kqchan::Process::_read() {
	auto listeningProcess = _process.lock();

	if (!listeningProcess) {
		// if the listening process is dead, log it and ignore the request (no one's listening for the reply anyways)
		kqchanProcLog.warning() << *this << ": received read request after listening process died" << kqchanProcLog.endLog;
		return;
	}

	kqchanProcLog.debug() << *this << ": received read request" << kqchanProcLog.endLog;

	{
		// our peer has acknowledged our notification by asking for the pending messages;
		// we can now send a notification again if we receive more data
		std::unique_lock lock(_notificationMutex);
		kqchanProcLog.debug() << *this << ": received acknowledgement (implicitly via read) for notification " << _notificationCount++ << "; notifications may now be sent" << kqchanProcLog.endLog;
		auxlog("proc._read ACK id=%llu socket_fd=%d listener_nsid=%d target_nspid=%d notif#=%llu -> canSendNotif=1 deferNotif=1",
			(unsigned long long)_debugID, _socket ? _socket->fd() : -1,
			listeningProcess ? (int)listeningProcess->nsid() : -1, (int)_nspid,
			(unsigned long long)_notificationCount);
		_canSendNotification = true;

		// see MachPort::_read() for why we defer notifications
		_deferNotification = true;
	}

	std::unique_lock lock(_mutex, std::defer_lock);
	Message msg(sizeof(dserver_kqchan_reply_proc_read_t), 0, _checkForEventsAsyncFactory());
	auto reply = reinterpret_cast<dserver_kqchan_reply_proc_read_t*>(msg.data().data());

	reply->header.number = dserver_kqchan_msgnum_proc_read;
	reply->header.code = 0;
	reply->data = 0;
	reply->fflags = 0;

	while (true) {
		lock.lock();

		if (_events.empty()) {
			lock.unlock();

			// if we don't have any events, tell our peer
			kqchanProcLog.debug() << *this << ": no events to read" << kqchanProcLog.endLog;

			reply->header.code = 0xdead;
			break;
		} else {
			auto event = std::move(_events.front());
			_events.pop_front();

			auto savedFlags = _flags;

			// drop the lock; we don't need it to set up the new kqchan or to discard it, nor to push the message to the outbox.
			// additionally, we don't want to hold it if we decide to drop the new kqchan, since that takes its own set of locks
			// when dying (and the fewer active locks we hold concurrently, the better)
			lock.unlock();

			reply->data = event.data;
			reply->fflags = event.events & savedFlags;

			if (reply->fflags == 0) {
				// if this event contains no events that the user is interested in, drop it
				kqchanProcLog.debug() << *this << ": event does not contain any events the user is interested in; dropping event" << kqchanProcLog.endLog;
				continue;
			}

			if (savedFlags & NOTE_TRACK) {
				if (event.newKqchan) {
					try {
						FD newKqchanSocket(event.newKqchan->setup());

						// no errors SHOULD be thrown from this point forward

						// give the new kqchan the most recent flags we have
						{
							std::unique_lock lock(event.newKqchan->_mutex);
							event.newKqchan->_flags = savedFlags;
						}

						listeningProcess->registerKqchan(event.newKqchan);
						msg.pushDescriptor(newKqchanSocket.extract());

						kqchanProcLog.debug() << *this << ": new process kqchan setup for child process and being returned" << kqchanProcLog.endLog;
					} catch (...) {
						kqchanProcLog.error() << *this << ": failed to setup new kqchan for child process" << kqchanProcLog.endLog;
						reply->fflags |= NOTE_TRACKERR;
					}
				} else if (event.events & NOTE_FORK) {
					kqchanProcLog.error() << *this << ": read NOTE_FORK event and user has requested NOTE_TRACK, but no new kqchan was associated with event" << kqchanProcLog.endLog;
					reply->fflags |= NOTE_TRACKERR;
				}
			} else if (event.newKqchan) {
				kqchanProcLog.info() << *this << ": event contains new kqchan, but user has not requested NOTE_TRACK; dropping new kqchan" << kqchanProcLog.endLog;
			}

			break;
		}
	}

	_outbox.push(std::move(msg));

	// now let's send any deferred notifications we might have
	_sendDeferredNotification();
};

bool DarlingServer::Kqchan::Process::_hasPendingEvents() {
	// perf#25a-hang (A0): report whether the server still holds events the guest hasn't read. Takes
	// _mutex (the event-queue lock). Used by _sendDeferredNotification to re-arm a notification when
	// deferral clears, so a notification lost to a defer/gate race is always recovered.
	std::unique_lock lock(_mutex);
	return !_events.empty();
}

void DarlingServer::Kqchan::Process::_notify(uint32_t event, int64_t data) {
	Event newEvent;

	kqchanProcLog.debug() << *this << ": notified with {event=" << event << ",data=" << data << "}" << kqchanProcLog.endLog;

	newEvent.data = data;
	newEvent.events = event;
	newEvent.newKqchan = nullptr;

	if (event == NOTE_FORK) {
		// NOTE: we always setup a new kqchan regardless of whether or not the user has currently requested NOTE_TRACK or not,
		//       in case the user does have NOTE_TRACK when reading the event.

		auto listeningProcess = _process.lock();

		if (!listeningProcess) {
			return;
		}

		auto maybeChild = processRegistry().lookupEntryByNSID(data & NOTE_PDATAMASK);

		if (!maybeChild) {
			kqchanProcLog.debug() << *this << ": notified with NOTE_FORK (and wanted NOTE_TRACK), but couldn't find child" << kqchanProcLog.endLog;
		} else {
			auto child = *maybeChild;

			auto newKqchan = std::make_shared<Process>(listeningProcess, data & NOTE_PDATAMASK, _flags);

			child->registerListeningKqchan(newKqchan);
			{
				std::unique_lock newLock(newKqchan->_mutex);
				newKqchan->_targetProcess = child;
				newKqchan->_attached = true;
			}

			auto childParent = child->parentProcess();

			newKqchan->_notify(NOTE_CHILD, (childParent ? childParent->nsid() : 0) & NOTE_PDATAMASK);

			newEvent.newKqchan = newKqchan;
		}
	}

	{
		std::unique_lock lock(_mutex);
		_events.push_back(std::move(newEvent));
		auto lp = _process.lock();
		auto tp = _targetProcess.lock();
		auxlog("proc._notify   id=%llu listener_nsid=%d target_nspid=%d event=0x%x data=%lld events_queued=%zu socket_fd=%d canSendNotif=%d",
			(unsigned long long)_debugID,
			lp ? (int)lp->nsid() : -1,
			(int)_nspid,
			(unsigned)event, (long long)data, _events.size(),
			_socket ? _socket->fd() : -1,
			(int)_canSendNotification);
		if (_socket) {
			_sendNotification();
		}
	}
};

void DarlingServer::Kqchan::Process::_checkForEventsAsync() {
	Thread::kernelAsync([weakSelf = weak_from_this()]() {
		auto self = weakSelf.lock();
		if (!self) {
			return;
		}
		// if we have more events ready, tell our peer
		if (!self->_events.empty()) {
			self->_sendNotification();
		}
	});
};

std::function<void()> DarlingServer::Kqchan::Process::_checkForEventsAsyncFactory() {
	return [weakSelf = weak_from_this()]() {
		auto self = weakSelf.lock();
		if (!self) {
			return;
		}
		self->_checkForEventsAsync();
	};
};

void DarlingServer::Kqchan::Process::logToStream(Log::Stream& stream) const {
	auto proc = _targetProcess.lock();

	Kqchan::logToStream(stream);

	stream << ":";
	if (proc) {
		stream << *proc;
	} else {
		stream << "<null>";
	}
};
