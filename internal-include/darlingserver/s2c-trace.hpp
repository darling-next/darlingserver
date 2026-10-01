/**
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling developers
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

#ifndef _DARLINGSERVER_S2C_TRACE_HPP_
#define _DARLINGSERVER_S2C_TRACE_HPP_

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdarg>

namespace DarlingServer {
	/**
	 * Ordering trace for the S2C protocol, used to prove that a control-path upcall is
	 * published before the green-light wait. Enabled by DSERVER_S2C_TRACE; silent otherwise.
	 *
	 * Every line carries CLOCK_MONOTONIC, which the server and the guest share because they
	 * are processes on the same kernel, so the guest's own trace lines are directly
	 * comparable with these. The per-actor sequence number is for reading a single actor's
	 * lines in order; the timestamps establish the order ACROSS actors, which is what the
	 * protocol claim is about.
	 */
	class S2CTrace {
	private:
		static bool enabled() {
			static bool value = []() {
				auto v = getenv("DSERVER_S2C_TRACE");
				return v && v[0] != '\0' && v[0] != '0';
			}();
			return value;
		}

		static std::atomic<uint64_t>& counter() {
			static std::atomic<uint64_t> value{0};
			return value;
		}

		static uint64_t now_ns() {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
		}

	public:
		// Free-form line for the transport/ingress traces, which carry fields the
		// ordered S2C events do not. Same stream and same enable flag, so one run
		// orders every actor's lines on one clock.
		static void line(const char* format, ...) {
			if (!enabled()) {
				return;
			}
			va_list args;
			fprintf(stderr, "S2C_TRACE t=%llu actor=server ", (unsigned long long)now_ns());
			va_start(args, format);
			vfprintf(stderr, format, args);
			va_end(args);
			fprintf(stderr, "\n");
		}

		static void server(const char* event, long tid, int s2cNumber) {
			if (!enabled()) {
				return;
			}
			auto seq = counter().fetch_add(1, std::memory_order_relaxed) + 1;
			fprintf(stderr, "S2C_TRACE t=%llu actor=server seq=%llu event=%s tid=%ld s2c=%d\n",
				(unsigned long long)now_ns(), (unsigned long long)seq, event, tid, s2cNumber);
		}
	};
}

#endif // _DARLINGSERVER_S2C_TRACE_HPP_
