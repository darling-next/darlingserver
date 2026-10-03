#ifndef _DARLINGSERVER_PUSH_REPLY_SYNC_PIPE_HPP_
#define _DARLINGSERVER_PUSH_REPLY_SYNC_PIPE_HPP_

#include <unistd.h>

namespace DarlingServer {
	class PushReplySyncPipe {
	public:
		explicit PushReplySyncPipe(int fd): _fd(fd) {}

		~PushReplySyncPipe() {
			if (_fd >= 0) {
				close(_fd);
			}
		}

		PushReplySyncPipe(const PushReplySyncPipe&) = delete;
		PushReplySyncPipe& operator=(const PushReplySyncPipe&) = delete;

		PushReplySyncPipe(PushReplySyncPipe&& other): _fd(other._fd) {
			other._fd = -1;
		}

		PushReplySyncPipe& operator=(PushReplySyncPipe&& other) {
			if (this != &other) {
				if (_fd >= 0) {
					close(_fd);
				}
				_fd = other._fd;
				other._fd = -1;
			}
			return *this;
		}

		explicit operator bool() const {
			return _fd >= 0;
		}

		void acknowledge() const {
			char value = 1;
			(void)!write(_fd, &value, sizeof(value));
		}

	private:
		int _fd;
	};
}

#endif
