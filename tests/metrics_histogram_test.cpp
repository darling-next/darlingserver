/**
 * perf #0 (dar-dar6x4-perf-5dq.6) regression test: the in-server metrics core.
 *
 * RED->GREEN intent (per the standing TDD rule): this test pins the behavior the
 * progress/wedge detector relies on. Before metrics.{hpp,cpp} existed it would not
 * compile/link (RED); after, it asserts:
 *   1. LatencyHistogram buckets values into the right log2 bucket and its quantiles
 *      track the distribution (a fast workload reports low pXX; a slow one high pXX --
 *      this is what lets the watcher tell SLOW from PROGRESSING).
 *   2. Metrics counters increment and snapshotJSON() emits valid, parseable fields
 *      (the wedge/idle/progress decision reads rpcs_serviced + last_reply_age + the
 *      injected gauges).
 *
 * Hermetic: links only metrics.cpp, no darlingserver boot, no sockets. Deterministic.
 * Exit 0 = GREEN (all asserts pass), non-zero = RED.
 */

#include <darlingserver/metrics.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
		++failures; \
	} \
} while (0)

static bool jsonHas(const std::string& s, const std::string& key) {
	return s.find("\"" + key + "\"") != std::string::npos;
}

int main() {
	using namespace DarlingServer;

	// ---- 1. histogram bucketing + quantiles ----
	{
		LatencyHistogram h;
		// 1000 "fast" samples around 5us
		for (int i = 0; i < 1000; ++i) h.record(5);
		CHECK(h.count() == 1000, "histogram count after 1000 fast records");
		CHECK(h.max() == 5, "histogram max == 5 for all-5us");
		CHECK(h.mean() == 5, "histogram mean == 5 for all-5us");
		// quantiles for an all-5us distribution must land in a small bucket (<= 8us
		// upper edge), NOT in the millisecond range.
		CHECK(h.quantile(0.50) <= 8, "p50 of 5us workload is small");
		CHECK(h.quantile(0.99) <= 8, "p99 of 5us workload is small");
	}
	{
		LatencyHistogram h;
		// a "slow" workload: 480ms per op (the dar-l3a per-fork cost)
		for (int i = 0; i < 100; ++i) h.record(480000);
		// p95 must be in the hundreds-of-ms range, i.e. clearly >= the 50ms SLOW threshold
		CHECK(h.quantile(0.95) >= 50000, "p95 of 480ms workload >= 50ms SLOW threshold");
		CHECK(h.max() == 480000, "histogram max == 480ms");
	}
	{
		// mixed: mostly fast, a few very slow -> p50 small, p99 large (tail visible)
		LatencyHistogram h;
		for (int i = 0; i < 990; ++i) h.record(3);
		for (int i = 0; i < 10; ++i) h.record(1000000); // 1s outliers
		CHECK(h.quantile(0.50) <= 8, "mixed p50 stays small");
		CHECK(h.quantile(0.99) >= 500000, "mixed p99 exposes the slow tail");
	}

	// ---- 2. counters + JSON snapshot ----
	{
		Metrics& m = Metrics::shared();
		m.startMonoUs = Metrics::nowMonoUs();
		uint64_t r0 = m.rpcsServiced.load();
		uint64_t c0 = m.checkins.load();
		uint64_t f0 = m.forks.load();
		m.rpcsServiced.fetch_add(7);
		m.checkins.fetch_add(3);
		m.forks.fetch_add(2);
		m.repliesSent.fetch_add(7);
		m.lastReplyMonoUs.store(Metrics::nowMonoUs());
		m.rpcLatency.record(5);
		m.checkinLatency.record(5);
		m.forkLatency.record(480000);

		CHECK(m.rpcsServiced.load() == r0 + 7, "rpcsServiced incremented");
		CHECK(m.checkins.load() == c0 + 3, "checkins incremented");
		CHECK(m.forks.load() == f0 + 2, "forks incremented");

		std::string gauges = "\"workqueue_depth\": 4, \"workers_total\": 1, \"workers_busy\": 1, \"clients_blocked_in_rpc\": 5";
		std::string json = m.snapshotJSON(gauges);

		// the snapshot must carry every field the watcher's decision rule reads
		CHECK(jsonHas(json, "rpcs_serviced"), "snapshot has rpcs_serviced");
		CHECK(jsonHas(json, "last_reply_age_ms"), "snapshot has last_reply_age_ms");
		CHECK(jsonHas(json, "clients_blocked_in_rpc"), "snapshot has clients_blocked_in_rpc (from gauges)");
		CHECK(jsonHas(json, "workqueue_depth"), "snapshot has workqueue_depth (from gauges)");
		CHECK(jsonHas(json, "checkin_latency"), "snapshot has checkin_latency histogram");
		CHECK(jsonHas(json, "fork_latency"), "snapshot has fork_latency histogram");
		CHECK(jsonHas(json, "p95_us"), "snapshot histograms expose p95_us");
		// brace balance sanity (valid-ish JSON)
		size_t opens = 0, closes = 0;
		for (char ch : json) { if (ch == '{') ++opens; else if (ch == '}') ++closes; }
		CHECK(opens == closes && opens >= 4, "snapshot JSON braces balanced");
	}

	if (failures == 0) {
		std::printf("GREEN: metrics histogram + snapshot behave correctly (%d checks)\n", 0);
		return 0;
	}
	std::fprintf(stderr, "RED: %d metrics check(s) failed\n", failures);
	return 1;
}
