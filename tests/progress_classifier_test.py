#!/usr/bin/env python3
# perf #0 (dar-dar6x4-perf-5dq.6) regression test: the progress/wedge DECISION RULE.
#
# This is the part that l3a-catch.sh got wrong ("log file frozen" => assumed stall,
# when the build was merely slow). The rule must distinguish four states from two
# consecutive metric snapshots. RED before classify() exists / if it regresses; GREEN
# when every case below maps to the documented state.
#
# Runs the real classify() from the darling-progress-watch tool. Exit 0 = GREEN.
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "darling-progress-watch")

spec = importlib.util.spec_from_loader("dpw", loader=None)
mod = importlib.util.module_from_spec(spec)
with open(TOOL) as f:
    exec(compile(f.read(), TOOL, "exec"), mod.__dict__)
classify = mod.__dict__["classify"]

SLOW_US = 50000      # 50ms
WEDGE_MS = 10000     # 10s

failures = []


def check(name, prev, cur, expect):
    state, detail = classify(prev, cur, SLOW_US, WEDGE_MS)
    if state != expect:
        failures.append(f"{name}: expected {expect}, got {state} ({detail})")


def snap(rpcs, blocked=0, age=0, checkin_p95=0, fork_p95=0):
    return {
        "rpcs_serviced": rpcs,
        "clients_blocked_in_rpc": blocked,
        "last_reply_age_ms": age,
        "checkin_latency": {"p95_us": checkin_p95},
        "fork_latency": {"p95_us": fork_p95},
    }


# PROGRESSING: rpcs up, latencies low (a healthy fast workload)
check("healthy",
      snap(1000), snap(1500, checkin_p95=5, fork_p95=20),
      "PROGRESSING")

# SLOW: rpcs up but fork p95 above threshold == the dar-l3a state (must NOT be WEDGED)
check("dar-l3a-slow",
      snap(1000), snap(1010, checkin_p95=2000, fork_p95=480000),
      "SLOW")

# SLOW via checkin latency alone
check("slow-checkin",
      snap(1000), snap(1200, checkin_p95=90000, fork_p95=0),
      "SLOW")

# WEDGED: rpcs flat, clients blocked, replies stale -> genuinely stuck
check("wedged",
      snap(1000, blocked=5, age=30000), snap(1000, blocked=5, age=45000),
      "WEDGED")

# IDLE: rpcs flat but nobody is waiting -> just quiescent, NOT a wedge
check("idle",
      snap(1000, blocked=0, age=60000), snap(1000, blocked=0, age=75000),
      "IDLE")

# flat + blocked but age below the wedge threshold -> not yet WEDGED (server may just
# be momentarily between replies); rule requires a stale last-reply too.
check("blocked-but-fresh",
      snap(1000, blocked=3, age=100), snap(1000, blocked=3, age=500),
      "IDLE")

if failures:
    for f in failures:
        sys.stderr.write("FAIL: " + f + "\n")
    sys.stderr.write(f"RED: {len(failures)} classifier case(s) wrong\n")
    sys.exit(1)
print("GREEN: progress classifier maps all 6 cases correctly")
sys.exit(0)
