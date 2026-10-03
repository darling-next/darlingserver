#!/usr/bin/env bash
# perf#18 D17-rerun (bead dar-1il.13): re-measure the post-D16 residual-UDS census on a REAL HEAVY
# multi-thread/multi-process workload + a MEANINGFUL fork-storm + a >64-concurrent-thread exhaustion test.
#
# D17's verdict (residual = B/D per-image lane split, NOT A/C) is structural and stands; this rerun only
# replaces D17's startup-dominated true/echo micro-workload so the RELATIVE residual/top-UDS SHARES are
# representative of a server that has serviced many thousands of RPCs.
#
# ENV NOTE (found this run): a `darling shell` wrapping a `make -j` build does NOT cleanly tear down in
# this environment -- the outer leaf shell hangs after the build's objects are all produced (reproduced
# IDENTICALLY on the prod dylib: 20/20 objects, then the wrapper hung past its own `timeout`). This is a
# harness/teardown limitation, NOT a transport defect and NOT D17-specific. The census is unaffected: each
# `clang`/`make` SUBPROCESS completes its full RPC lifecycle (objects get built), and the counters live
# server-side -- so we run the build BACKGROUNDED for a bounded window, snapshot the stat socket
# (independent of the leaf exiting), and harvest guest [dring-lane-stats] from the LIGHT leaves
# (fork-storm children + the threads probe) which DO exit cleanly via sys_exit.
#
# THREE workloads under the same armed censuses (RESIDUAL + ATTACH + HEATMAP):
#   (1) PRIMARY heavy: `make -j8` of 200 tiny .c via the prefix's clang = a swarm of forked compiler
#       subprocesses, backgrounded, given a bounded BUILD_WINDOW, then snapshotted (leaf may hang -> killed).
#   (2) meaningful fork-storm: 48 children that each do real ring work (mach ops + execs) before exit.
#   (3) lane-exhaustion: ONE process spawning 96 concurrent THREADS each hammering ring-eligible
#       self-trap ops (GR_MAX_LANES=64 is PER-PROCESS -> the real exhaustion test; D17 saw max 3).
#
# RECON-only: no new migrations, no duplex/psynch, no lane-ownership change. Default-OFF instrumentation.
# Usage: bash run-residual-census-d17-rerun.sh
set -u

PPREFIX="${PPREFIX:-$HOME/work/darling-prefix}"
PREFIX="${PREFIX:-$HOME/work/darling-prefix-homebrew-test}"
LAUNCH="${LAUNCH:-$PPREFIX/bin/darling}"
STAT="${STAT:-$HOME/work/darling-dev/darling/src/external/darlingserver/tools/darling-stat}"
TMP="${TMP:-/home/ilyagulya/.claude-work/jobs/ae391f52/tmp}"
SRV="$TMP/darlingserver.d17"
DYL="$TMP/libsystem_kernel.d17.dylib"
DYLD="$TMP/dyld.d17"
PRODDYL="$PREFIX/usr/lib/system/libsystem_kernel.dylib.prod-bak"   # 6bd251 real prod dylib
OUT="$TMP/d17rr-snapshot.json"
GH="$PREFIX/Users/ilyagulya"
BUILD_WINDOW="${BUILD_WINDOW:-150}"   # seconds to let the backgrounded make build drive RPC traffic

fail() { echo "D17RR-RUN FAIL: $*" >&2; exit 1; }
note() { echo "== $* =="; }

cleanboot() {
  DPREFIX="$PREFIX" "$LAUNCH" shutdown >/dev/null 2>&1; sleep 2
  pgrep -x darlingserver >/dev/null && { kill -9 $(pgrep -x darlingserver) 2>/dev/null; sleep 1; }
  # kill any wedged leaf darling shells from a prior build window
  pkill -9 -f 'd17rr-makebuild' 2>/dev/null
  rm -f "$PREFIX/.darlingserver.sock" "$PREFIX/.init.pid" 2>/dev/null; sleep 1
}

deploy() {
  cleanboot
  cp -a "$SRV"  "$PPREFIX/bin/darlingserver" || fail "deploy server"
  cp -a "$DYL"  "$PPREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib P"
  cp -a "$DYL"  "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib P/libexec"
  cp -a "$DYL"  "$PREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib H"
  cp -a "$DYLD" "$PPREFIX/usr/lib/dyld" || fail "deploy dyld P"
  cp -a "$DYLD" "$PPREFIX/libexec/darling/usr/lib/dyld" || fail "deploy dyld P/libexec"
}

restore_prod() {
  cleanboot
  cp -a "$PPREFIX/bin/darlingserver.prod-bak" "$PPREFIX/bin/darlingserver"
  cp -a "$PRODDYL" "$PPREFIX/usr/lib/system/libsystem_kernel.dylib"
  cp -a "$PRODDYL" "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib"
  cp -a "$PRODDYL" "$PREFIX/usr/lib/system/libsystem_kernel.dylib"
  cp -a "$PPREFIX/usr/lib/dyld.prod-bak" "$PPREFIX/usr/lib/dyld"
  cp -a "$PPREFIX/libexec/darling/usr/lib/dyld.prod-bak" "$PPREFIX/libexec/darling/usr/lib/dyld"
  note "restore verify (all must match prod md5s: srv 835946.., dyl 6bd251.., dyld 79b227..)"
  md5sum "$PPREFIX/bin/darlingserver" "$PPREFIX/usr/lib/system/libsystem_kernel.dylib" \
         "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib" \
         "$PREFIX/usr/lib/system/libsystem_kernel.dylib" \
         "$PPREFIX/usr/lib/dyld" "$PPREFIX/libexec/darling/usr/lib/dyld"
}

warm_and_workload() {
  note "warm boot (residual + attach census + heatmap armed; first boot cold/slow)"
  DPREFIX="$PREFIX" DARLING_SERVER_RESIDUAL_CENSUS=1 DARLING_SERVER_ATTACH_CENSUS=1 DARLING_SERVER_RPC_HEATMAP=1 \
    timeout 300 "$LAUNCH" shell /usr/bin/true >/dev/null 2>&1

  note "compile the 96-thread exhaustion probe inside the guest"
  DPREFIX="$PREFIX" timeout 200 "$LAUNCH" shell /bin/sh -c '
    CC=/Library/Developer/CommandLineTools/usr/bin/clang
    SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX11.sdk
    "$CC" -isysroot "$SDK" -O2 -o /Users/ilyagulya/d17rr-threads /Users/ilyagulya/d17rr-threads.c -lpthread 2>&1
    ls -la /Users/ilyagulya/d17rr-threads 2>&1' 2>&1 | tail -2

  note "PRIMARY heavy: make -j8 build, BACKGROUNDED, ${BUILD_WINDOW}s window (leaf may hang on teardown; that's fine)"
  DPREFIX="$PREFIX" timeout $((BUILD_WINDOW + 60)) "$LAUNCH" shell /bin/bash /Users/ilyagulya/d17rr-makebuild.sh >/dev/null 2>&1 &
  BUILD_PID=$!
  sleep "$BUILD_WINDOW"
  note "build window elapsed; objects so far: $(ls "$GH/d17rr-build/"*.o 2>/dev/null | wc -l)"
  # snapshot DURING the heavy traffic (counters are server-side; independent of leaf exit)
  "$STAT" "$PREFIX" > "$TMP/d17rr-snapshot-heavy.json" 2>/dev/null && note "heavy-phase snapshot captured"
  # stop the (possibly-wedged) build leaf
  kill -9 "$BUILD_PID" 2>/dev/null
  pkill -9 -f 'd17rr-makebuild' 2>/dev/null
  pkill -9 -f 'CommandLineTools/usr/bin/clang' 2>/dev/null
  sleep 2

  note "meaningful fork-storm (48 children doing real ring work; capture lane-stats)"
  DPREFIX="$PREFIX" DARLING_GUEST_LANE_STATS=1 timeout 200 "$LAUNCH" shell /bin/bash /Users/ilyagulya/d17rr-forkstorm.sh 2>&1 | grep -E '\[d17rr-forkstorm\]|\[dring-lane-stats\]' | head -10

  note "lane-exhaustion: ONE process, 96 concurrent threads on ring-eligible self-traps (capture lane-stats)"
  DPREFIX="$PREFIX" DARLING_GUEST_LANE_STATS=1 timeout 200 "$LAUNCH" shell /Users/ilyagulya/d17rr-threads 96 2>&1 | grep -E '\[d17rr-threads\]|\[dring-lane-stats\]'
}

deploy
warm_and_workload
note "FINAL snapshot (post all workloads)"
"$STAT" "$PREFIX" > "$OUT" 2>/dev/null || fail "stat snapshot failed"
restore_prod
echo
echo "===================== HEAVY-PHASE SNAPSHOT (during make build) ====================="
[ -f "$TMP/d17rr-snapshot-heavy.json" ] && python3 "$TMP/d17summary.py" "$TMP/d17rr-snapshot-heavy.json"
echo
echo "===================== FINAL SNAPSHOT (all workloads) ====================="
python3 "$TMP/d17summary.py" "$OUT"
echo "D17RR-RUN: snapshots -> $TMP/d17rr-snapshot-heavy.json , $OUT"
