#ifndef _DARLINGSERVER_EXEC_COMPLETION_HPP_
#define _DARLINGSERVER_EXEC_COMPLETION_HPP_

#include <cerrno>
#include <memory>
#include <system_error>
#include <utility>
#include <unistd.h>

#include <darlingserver/utility.hpp>

namespace DarlingServer {
	// Serialized by Process::_rwlock. The pipe, not delivery of its epoll
	// notification, determines whether exec completed before a new Checkin.
	class ExecCompletion {
	public:
		enum class Result {
			None,
			Failed,
			Darling,
			Native,
		};

		void begin(std::shared_ptr<FD> listener, bool executingMachO) {
			_listener = std::move(listener);
			_executingMachO = executingMachO;
		}

		Result poll() {
			if (!_listener) {
				return Result::None;
			}

			char failure;
			ssize_t count;
			do {
				count = read(_listener->fd(), &failure, sizeof(failure));
			} while (count < 0 && errno == EINTR);
			if (count < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					return Result::None;
				}
				throw std::system_error(errno, std::generic_category(), "Failed to read from exec listener pipe");
			}

			auto result = count != 0 ? Result::Failed : (_executingMachO ? Result::Darling : Result::Native);
			_listener.reset();
			return result;
		}

		Result poll(const std::shared_ptr<FD>& listener) {
			// A Checkin may already have consumed this completion, or another
			// exec attempt may have started before an old monitor was serviced.
			return listener == _listener ? poll() : Result::None;
		}

	private:
		std::shared_ptr<FD> _listener;
		bool _executingMachO = false;
	};
}

#endif
