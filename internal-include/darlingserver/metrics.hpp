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

#ifndef _DARLINGSERVER_METRICS_HPP_
#define _DARLINGSERVER_METRICS_HPP_

// perf #0 (dar-dar6x4-perf-5dq.6): in-server progress + timing metrics.
//
// Why this exists: from OUTSIDE the server you cannot reliably tell "the Darling
// workload is slow but progressing" from "it is genuinely wedged" -- log-file size,
// CPU%, and single /proc snapshots all conflate the two (this misdiagnosed dar-l3a as
// a stall when gettext was simply building slowly). The robust signal is a monotonic
// count of work actually done plus the latency of that work, read straight from the
// server over a dedicated UNIX socket.
//
// Design constraints:
//  - hot-path increments must be cheap and lock-free -> std::atomic<uint64_t> with
//    relaxed ordering (these are statistics, not synchronization).
//  - latency is recorded into a fixed log2-bucketed histogram (no per-sample storage,
//    no allocation); quantiles are computed from the buckets at snapshot time.
//  - the snapshot is produced on the main event-loop thread (the stat socket is just
//    another epoll fd), so reading atomics there is naturally consistent enough.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <array>

namespace DarlingServer {
	// A lock-free latency histogram with log2 buckets.
	// bucket i counts samples whose value (in microseconds) is in [2^(i-1), 2^i)
	// (bucket 0 = [0,1)). With 32 buckets we cover up to ~2^31 us (~35 min), which
	// is far beyond any sane per-RPC latency.
	class LatencyHistogram {
	public:
		static constexpr size_t kBucketCount = 32;

		void record(uint64_t microseconds) {
			size_t idx = bucketFor(microseconds);
			_buckets[idx].fetch_add(1, std::memory_order_relaxed);
			_count.fetch_add(1, std::memory_order_relaxed);
			_sum.fetch_add(microseconds, std::memory_order_relaxed);
			// monotonic running max
			uint64_t prev = _max.load(std::memory_order_relaxed);
			while (microseconds > prev && !_max.compare_exchange_weak(prev, microseconds, std::memory_order_relaxed)) {
				// prev was reloaded by compare_exchange_weak; loop
			}
		}

		uint64_t count() const { return _count.load(std::memory_order_relaxed); }
		uint64_t sum()   const { return _sum.load(std::memory_order_relaxed); }
		uint64_t max()   const { return _max.load(std::memory_order_relaxed); }
		uint64_t mean()  const {
			uint64_t c = count();
			return (c == 0) ? 0 : (sum() / c);
		}

		// Approximate quantile (in microseconds) from the buckets. We return the upper
		// edge of the bucket in which the requested rank falls -- an over-estimate
		// bounded by the bucket width (<=2x), which is fine for "is it 5us or 500ms".
		uint64_t quantile(double q) const {
			uint64_t c = count();
			if (c == 0) {
				return 0;
			}
			uint64_t target = static_cast<uint64_t>(q * static_cast<double>(c));
			if (target >= c) {
				target = c - 1;
			}
			uint64_t cumulative = 0;
			for (size_t i = 0; i < kBucketCount; ++i) {
				cumulative += _buckets[i].load(std::memory_order_relaxed);
				if (cumulative > target) {
					return bucketUpperEdge(i);
				}
			}
			return bucketUpperEdge(kBucketCount - 1);
		}

	private:
		static size_t bucketFor(uint64_t microseconds) {
			if (microseconds == 0) {
				return 0;
			}
			// index = floor(log2(us)) + 1, clamped to the last bucket
			size_t idx = 0;
			uint64_t v = microseconds;
			while (v > 0) {
				++idx;
				v >>= 1;
			}
			// now idx == floor(log2(us)) + 1
			return (idx >= kBucketCount) ? (kBucketCount - 1) : idx;
		}

		static uint64_t bucketUpperEdge(size_t i) {
			if (i == 0) {
				return 1;
			}
			if (i >= kBucketCount - 1) {
				return UINT64_C(1) << (kBucketCount - 1);
			}
			return UINT64_C(1) << i;
		}

		std::array<std::atomic<uint64_t>, kBucketCount> _buckets {};
		std::atomic<uint64_t> _count {0};
		std::atomic<uint64_t> _sum {0};
		std::atomic<uint64_t> _max {0};
	};

	class Metrics {
	public:
		static Metrics& shared();

		// ---- monotonic counters (hot path) ----
		std::atomic<uint64_t> rpcsServiced {0};   // RPC calls fully processed
		std::atomic<uint64_t> repliesSent {0};    // reply datagrams sent
		std::atomic<uint64_t> checkins {0};       // checkin RPCs (process/thread registration)
		std::atomic<uint64_t> forks {0};          // fork-checkins specifically
		std::atomic<uint64_t> messagesReceived {0};
		// perf #2b (dar-dar6x4-perf-5dq.8): inline fast-path accounting. A call run to
		// completion directly on the main event loop (no worker wakeup) increments
		// inlineHandled; a call that suspended its microthread (so it must finish on the
		// worker pool when resumed) increments queuedToPool. Under a healthy fast-path the
		// vast majority of cheap RPCs (checkin etc.) should be inlineHandled.
		std::atomic<uint64_t> inlineHandled {0};
		std::atomic<uint64_t> queuedToPool {0};

		// ---- gauges sampled at snapshot time (set by the owner) ----
		// These are filled in by Server when producing a snapshot, not on the hot path.

		// ---- latency histograms ----
		LatencyHistogram rpcLatency;       // service time of any RPC (worker entry -> reply)
		LatencyHistogram checkinLatency;   // service time of checkin RPCs
		LatencyHistogram forkLatency;      // service time of fork-checkin RPCs (incl. parent coordination)

		// last reply timestamp (CLOCK_MONOTONIC microseconds), for last_reply_age_ms
		std::atomic<uint64_t> lastReplyMonoUs {0};

		// process start timestamp (CLOCK_MONOTONIC microseconds)
		uint64_t startMonoUs {0};

		// Current monotonic time in microseconds.
		static uint64_t nowMonoUs();

		// Build a JSON snapshot string. `extra` lets the caller inject gauges it owns
		// (workqueue_depth, workers_busy/total, clients_blocked_in_rpc) as already-
		// formatted "\"key\": value" fragments, comma-separated, no surrounding braces.
		std::string snapshotJSON(const std::string& extraGauges) const;

	private:
		Metrics() = default;
	};
};

#endif // _DARLINGSERVER_METRICS_HPP_
