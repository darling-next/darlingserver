#ifndef _DARLINGSERVER_PROCESSCALL_GUARD_HPP_
#define _DARLINGSERVER_PROCESSCALL_GUARD_HPP_

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <system_error>

namespace DarlingServer {
	template <typename Function>
	inline int processCallBasicReplyCode(Function&& function) {
		try {
			function();
			return 0;
		} catch (const std::system_error& err) {
			return -err.code().value();
		} catch (const std::exception& err) {
			// perf#30 ATTRIBUTION: the checkin page route fails with -EINVAL 102 times out of 108 while the
			// datagram route succeeds, so the exception is what the page route does differently -- but its
			// message was being discarded. Print it (gated) so the reason is a measurement, not a guess.
			if (getenv("DARLING_SERVER_COURIER_LOG") != nullptr) {
				fprintf(stderr, "[processcall-guard] std::exception: %s\n", err.what());
			}
			return -EINVAL;
		} catch (...) {
			if (getenv("DARLING_SERVER_COURIER_LOG") != nullptr) {
				fprintf(stderr, "[processcall-guard] unknown exception\n");
			}
			return -EINVAL;
		}
	}
}

#endif
