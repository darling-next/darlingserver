#ifndef _DARLINGSERVER_PROCESSCALL_GUARD_HPP_
#define _DARLINGSERVER_PROCESSCALL_GUARD_HPP_

#include <cerrno>
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
		} catch (const std::exception&) {
			return -EINVAL;
		} catch (...) {
			return -EINVAL;
		}
	}
}

#endif
