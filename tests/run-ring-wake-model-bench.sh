#!/usr/bin/env bash
# perf#18 .31 (dar-dar6x4-perf-5dq.31): build + run the isolated ring WAKE-MODEL A/B, and
# assert the P4 design (v3) beats the current syscall-per-call model (v0) by a large factor.
#
# This is a host-only diagnostic (two pinned processes over a real shm SPSC ring, no Mach /
# dispatch / darlingserver) that isolates the WAKEUP variable -- the thing the live mach_reply_port
# A/B proved is ~90% of cost. It doubles as a regression GATE on the P4 wake model (.33): if a
# future change reintroduces a per-call syscall/sleep on the hot path, v3's p50 collapses toward
# v0's and this gate fails.
#
# GATE: v3 (cold) p50 must be >= 50x faster than v0 (cold) p50. Empirically the gap is ~300x
# (v0 ~58000ns vs v3 ~180ns), so 50x is a wide, non-flaky margin.
#
# USAGE: run-ring-wake-model-bench.sh [N] [CPU_SRV] [CPU_CLI]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/ring_wake_model_bench.c"
N="${1:-100000}"
CSRV="${2:-2}"
CCLI="${3:-4}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
BIN="$TMP/wakebench"

CC="${CC:-gcc}"
echo "== building host wake-model bench =="
if ! "$CC" -O2 -std=c11 -pthread -o "$BIN" "$SRC"; then
	echo "build FAILED"; exit 2
fi

p50_of() { # variant [--hot] -> echoes p50 ns
	local v="$1"; shift
	"$BIN" "$v" --n "$N" --cpu "$CSRV,$CCLI" "$@" 2>/dev/null | grep -oE 'p50=[0-9]+' | head -1 | cut -d= -f2
}

echo "== A/B matrix (cold = server uses its sleep path; hot = server busy-spins) =="
echo "-- COLD --"
for v in v0 v1 v2 v3; do "$BIN" "$v" --n "$N" --cpu "$CSRV,$CCLI"; done
echo "-- HOT --"
for v in v0 v1 v2 v3; do "$BIN" "$v" --hot --n "$N" --cpu "$CSRV,$CCLI"; done
echo

echo "== GATE: v3 cold must be >= 50x faster than v0 cold =="
V0=$(p50_of v0)
V3=$(p50_of v3)
if [ -z "$V0" ] || [ -z "$V3" ] || [ "$V3" -le 0 ]; then
	echo "GATE could not parse p50 (v0=$V0 v3=$V3)"; exit 2
fi
RATIO=$(( V0 / V3 ))
echo "  v0 cold p50 = ${V0} ns ; v3 cold p50 = ${V3} ns ; ratio = ${RATIO}x"
if [ "$RATIO" -lt 50 ]; then
	echo "GATE FAILED: v3 is only ${RATIO}x faster than v0 (expected >= 50x) -- the wake model regressed"
	exit 1
fi
echo "  GATE PASSED (${RATIO}x >= 50x): the polling/spin wake model eliminates the per-call syscall/sleep."
echo

# perf #18 P4 (.33): the integration GATE. The bench above proves the MODEL is fast in the
# abstract; this proves the REAL shared predicates the server + guest compile (from
# rpc-supplement.h) are conditional, not the v0 always-wake. RED arm (-DWAKE_MODEL_OLD) must fail.
echo "== GATE: wake-model decision predicates are conditional (RED->GREEN) =="
PRED_SRC="$HERE/ring_wake_predicates_test.c"
INC="$HERE/../include"
if [ ! -f "$PRED_SRC" ]; then
	echo "GATE skipped: $PRED_SRC missing"; exit 2
fi
GREEN_BIN="$TMP/wp_green"; RED_BIN="$TMP/wp_red"
if ! "$CC" -O2 -std=c11 -I"$INC" -o "$GREEN_BIN" "$PRED_SRC" 2>"$TMP/wp_green.log"; then
	echo "GATE FAILED: GREEN arm did not compile:"; cat "$TMP/wp_green.log"; exit 1
fi
if ! "$GREEN_BIN"; then
	echo "GATE FAILED: GREEN arm (real predicates) does not satisfy the wake-model invariants"; exit 1
fi
if ! "$CC" -O2 -std=c11 -DWAKE_MODEL_OLD -I"$INC" -o "$RED_BIN" "$PRED_SRC" 2>/dev/null; then
	echo "GATE FAILED: RED arm did not compile"; exit 1
fi
if "$RED_BIN" >/dev/null 2>&1; then
	echo "GATE FAILED: RED arm (v0 always-wake) PASSED -- the test does not actually exercise conditionality"; exit 1
fi
echo "  GATE PASSED: real predicates are conditional; the v0 always-wake arm fails as required."
echo
echo "ring_wake_model bench+gate: OK"
