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

#include <darlingserver/metrics.hpp>

#include <time.h>
#include <sstream>

DarlingServer::Metrics& DarlingServer::Metrics::shared() {
	static Metrics instance;
	return instance;
}

uint64_t DarlingServer::Metrics::nowMonoUs() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

static void appendHistogram(std::ostringstream& out, const char* name, const DarlingServer::LatencyHistogram& h) {
	out << "  \"" << name << "\": {"
	    << "\"count\": " << h.count()
	    << ", \"mean_us\": " << h.mean()
	    << ", \"p50_us\": " << h.quantile(0.50)
	    << ", \"p95_us\": " << h.quantile(0.95)
	    << ", \"p99_us\": " << h.quantile(0.99)
	    << ", \"max_us\": " << h.max()
	    << "}";
}

std::string DarlingServer::Metrics::snapshotJSON(const std::string& extraGauges) const {
	uint64_t now = nowMonoUs();
	uint64_t uptimeUs = (startMonoUs && now >= startMonoUs) ? (now - startMonoUs) : 0;
	uint64_t lastReply = lastReplyMonoUs.load(std::memory_order_relaxed);
	uint64_t lastReplyAgeMs = (lastReply && now >= lastReply) ? ((now - lastReply) / 1000ULL) : 0;

	std::ostringstream out;
	out << "{\n";
	out << "  \"uptime_s\": " << (uptimeUs / 1000000ULL) << ",\n";
	out << "  \"rpcs_serviced\": " << rpcsServiced.load(std::memory_order_relaxed) << ",\n";
	out << "  \"replies_sent\": " << repliesSent.load(std::memory_order_relaxed) << ",\n";
	out << "  \"messages_received\": " << messagesReceived.load(std::memory_order_relaxed) << ",\n";
	out << "  \"checkins\": " << checkins.load(std::memory_order_relaxed) << ",\n";
	out << "  \"forks\": " << forks.load(std::memory_order_relaxed) << ",\n";
	out << "  \"inline_handled\": " << inlineHandled.load(std::memory_order_relaxed) << ",\n";
	out << "  \"queued_to_pool\": " << queuedToPool.load(std::memory_order_relaxed) << ",\n";
	out << "  \"last_reply_age_ms\": " << lastReplyAgeMs << ",\n";
	if (!extraGauges.empty()) {
		out << "  " << extraGauges << ",\n";
	}
	appendHistogram(out, "rpc_latency", rpcLatency); out << ",\n";
	appendHistogram(out, "checkin_latency", checkinLatency); out << ",\n";
	appendHistogram(out, "fork_latency", forkLatency); out << "\n";
	out << "}\n";
	return out.str();
}
