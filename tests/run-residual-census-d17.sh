#!/usr/bin/env bash
# perf#18 D17 (dar-1il.12): POST-D16 residual-UDS heatmap + classifier -- live RECON runner.
#
# After per-thread lanes (D16) the eligible-UDS pool dropped 1533->231. D17 classifies that residual to
# decide if it is (A) unavoidable first-eligible-call-before-this-thread's-lane-attaches, (B) a wrapper
# coverage gap, (C) lane exhaustion, or (D) a call site in a no-ring image. It deploys the MATCHED
# server (residual census + attach census + heatmap) + the D16 per-thread dylib (with the guest
# lane-stats hook) + the matched dyld (so attach_rejects ~ 0), warm-boots with all censuses armed, runs
# the SAME warm workload, and dumps the residual_* + attach_census + heatmap snapshot. No new migrations,
# no duplex/psynch, no lane-ownership change -- pure measurement.
#
# Usage: bash run-residual-census-d17.sh
set -u

BUILD="${BUILD:-$HOME/work/darling-build}"
PPREFIX="${PPREFIX:-$HOME/work/darling-prefix}"
PREFIX="${PREFIX:-$HOME/work/darling-prefix-homebrew-test}"
LAUNCH="${LAUNCH:-$PPREFIX/bin/darling}"
STAT="${STAT:-$HOME/work/darling-dev/darling/src/external/darlingserver/tools/darling-stat}"
TMP="${TMP:-/home/ilyagulya/.claude-work/jobs/ae391f52/tmp}"
SRV="$TMP/darlingserver.d17"
DYL="$TMP/libsystem_kernel.d17.dylib"
DYLD="$TMP/dyld.d17"
OUT="$TMP/d17-snapshot.json"

fail() { echo "D17-RUN FAIL: $*" >&2; exit 1; }
note() { echo "== $* =="; }

cleanboot() {
  DPREFIX="$PREFIX" "$LAUNCH" shutdown >/dev/null 2>&1; sleep 2
  pgrep -x darlingserver >/dev/null && { kill -TERM $(pgrep -x darlingserver) 2>/dev/null; sleep 2; }
  pgrep -x darlingserver >/dev/null && { kill -9 $(pgrep -x darlingserver) 2>/dev/null; sleep 1; }
  rm -f "$PREFIX/.darlingserver.sock" "$PREFIX/.init.pid" 2>/dev/null; sleep 1
}

deploy() {
  cleanboot
  cp -a "$SRV" "$PPREFIX/bin/darlingserver" || fail "deploy server"
  cp -a "$DYL" "$PPREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib P"
  cp -a "$DYL" "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib P/libexec"
  cp -a "$DYL" "$PREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib H"
  cp -a "$DYLD" "$PPREFIX/usr/lib/dyld" || fail "deploy dyld P"
  cp -a "$DYLD" "$PPREFIX/libexec/darling/usr/lib/dyld" || fail "deploy dyld P/libexec"
}

warm_and_workload() {
  note "warm boot (residual + attach census + heatmap armed in server env; first boot cold/slow)"
  DPREFIX="$PREFIX" DARLING_SERVER_RESIDUAL_CENSUS=1 DARLING_SERVER_ATTACH_CENSUS=1 DARLING_SERVER_RPC_HEATMAP=1 \
    timeout 200 "$LAUNCH" shell /usr/bin/true >/dev/null 2>&1
  note "exec batches (multi-thread leaves; capture a few guest lane-stats lines)"
  for i in $(seq 1 12); do
    DPREFIX="$PREFIX" DARLING_GUEST_LANE_STATS=1 timeout 90 "$LAUNCH" shell /bin/echo "batch-$i" 2>&1 | grep -E '\[dring-lane-stats\]' | head -1
    DPREFIX="$PREFIX" timeout 90 "$LAUNCH" shell /usr/bin/true >/dev/null 2>&1
  done
  note "fork-storm (30 children, guest-visible path)"
  DPREFIX="$PREFIX" timeout 120 "$LAUNCH" shell /bin/sh -c '
    i=0; while [ $i -lt 30 ]; do ( /usr/bin/true ) & i=$((i+1)); done; wait' >/dev/null 2>&1
}

deploy
warm_and_workload
note "snapshot"
"$STAT" "$PREFIX" > "$OUT" 2>/dev/null || fail "stat snapshot failed"
cleanboot
echo "D17-RUN: snapshot -> $OUT"
python3 "$TMP/d17summary.py" "$OUT"
