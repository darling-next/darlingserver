#!/usr/bin/env bash
# perf#18 D18a (task #68): GR_MAX_LANES scaling A/B. For each lane-cap value, deploy the matched guest
# images (server is GR_MAX_LANES-agnostic -> reuse the D17 census server) + run the 96-thread hot-RPC
# workload + a fork-storm against ONE live armed server, capture guest lane-stats + a stat snapshot.
#
# GR_MAX_LANES is a GUEST-ONLY cap (server _ringThreads is an unbounded vector). So the A/B varies only
# the dylib+dyld; the D17 census server is reused unchanged for every value.
#
# No source edits at runtime (variants were built ahead of time + source reverted), no reclaim, no opcode
# change, no duplex/psynch/mach_msg_overwrite. Default-OFF census instrumentation reused from D17.
# Usage: bash run-d18a-lanes-ab.sh [VALUES...]   (default: 64 96 128 256)
set -u

PPREFIX="${PPREFIX:-$HOME/work/darling-prefix}"
PREFIX="${PREFIX:-$HOME/work/darling-prefix-homebrew-test}"
LAUNCH="${LAUNCH:-$PPREFIX/bin/darling}"
STAT="${STAT:-$HOME/work/darling-dev/darling/src/external/darlingserver/tools/darling-stat}"
TMP="${TMP:-/home/ilyagulya/.claude-work/jobs/ae391f52/tmp}"
SRV="$TMP/darlingserver.d17"                       # GR_MAX_LANES-agnostic
PRODDYL="$PREFIX/usr/lib/system/libsystem_kernel.dylib.prod-bak"   # 6bd251 real prod
GH="$PREFIX/Users/ilyagulya"
VALUES="${*:-64 96 128 256}"
NTHREADS="${NTHREADS:-96}"   # the hot-RPC workload's thread fan-out (the exhaustion dimension)

fail() { echo "D18A FAIL: $*" >&2; exit 1; }
note() { echo "== $* =="; }

cleanboot() {
  DPREFIX="$PREFIX" "$LAUNCH" shutdown >/dev/null 2>&1; sleep 2
  pgrep -x darlingserver >/dev/null && { kill -9 $(pgrep -x darlingserver) 2>/dev/null; sleep 1; }
  pkill -9 -f 'd17rr-threads' 2>/dev/null; pkill -9 -f 'd17rr-forkstorm' 2>/dev/null
  rm -f "$PREFIX/.darlingserver.sock" "$PREFIX/.init.pid" 2>/dev/null; sleep 1
}

deploy_variant() {
  local V=$1
  local DYL="$TMP/libsystem_kernel.lanes${V}.dylib"
  local DYLD="$TMP/dyld.lanes${V}"
  [ -f "$DYL" ]  || fail "missing $DYL"
  [ -f "$DYLD" ] || fail "missing $DYLD"
  cleanboot
  cp -a "$SRV"  "$PPREFIX/bin/darlingserver" || fail "deploy server"
  cp -a "$DYL"  "$PPREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "dylib P"
  cp -a "$DYL"  "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib" || fail "dylib P/libexec"
  cp -a "$DYL"  "$PREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "dylib H"
  cp -a "$DYLD" "$PPREFIX/usr/lib/dyld" || fail "dyld P"
  cp -a "$DYLD" "$PPREFIX/libexec/darling/usr/lib/dyld" || fail "dyld P/libexec"
}

run_one() {
  local V=$1
  echo
  echo "########################## GR_MAX_LANES=$V (threads=$NTHREADS) ##########################"
  deploy_variant "$V"
  note "warm boot (censuses armed)"
  DPREFIX="$PREFIX" DARLING_SERVER_RESIDUAL_CENSUS=1 DARLING_SERVER_ATTACH_CENSUS=1 DARLING_SERVER_RPC_HEATMAP=1 \
    timeout -s KILL 300 "$LAUNCH" shell /usr/bin/true >/dev/null 2>&1
  local SRVPID=$(pgrep -x darlingserver | head -1)
  note "server pid=$SRVPID; recompile probe under this dylib (clean leaf)"
  DPREFIX="$PREFIX" timeout -s KILL 200 "$LAUNCH" shell /bin/sh -c '
    /Library/Developer/CommandLineTools/usr/bin/clang -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX11.sdk -O2 \
      -o /Users/ilyagulya/d17rr-threads /Users/ilyagulya/d17rr-threads.c -lpthread 2>&1 && echo COMPILE_OK' 2>&1 | tail -1
  note "fork-storm (48 children, lane-stats)"
  DPREFIX="$PREFIX" DARLING_GUEST_LANE_STATS=1 timeout -s KILL 200 "$LAUNCH" shell /bin/bash /Users/ilyagulya/d17rr-forkstorm.sh 2>&1 \
    | grep -E '\[d17rr-forkstorm\]|\[dring-lane-stats\]' | head -4
  note "${NTHREADS}-thread hot-RPC probe x2 (lane-stats: acquired/exhausted/held_now/max)"
  for r in 1 2; do
    DPREFIX="$PREFIX" DARLING_GUEST_LANE_STATS=1 timeout -s KILL 200 "$LAUNCH" shell /Users/ilyagulya/d17rr-threads "$NTHREADS" 2>&1 \
      | grep -E '\[d17rr-threads\]|exhausted='
  done
  note "server pid now=$(pgrep -x darlingserver | head -1) (must == $SRVPID = no respawn)"
  note "SNAPSHOT live armed server"
  "$STAT" "$PREFIX" > "$TMP/d18a-lanes${V}.json" 2>/dev/null && echo "snapshot -> d18a-lanes${V}.json"
}

for V in $VALUES; do run_one "$V"; done

note "RESTORE PROD"
cleanboot
cp -a "$PPREFIX/bin/darlingserver.prod-bak" "$PPREFIX/bin/darlingserver"
cp -a "$PRODDYL" "$PPREFIX/usr/lib/system/libsystem_kernel.dylib"
cp -a "$PRODDYL" "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib"
cp -a "$PRODDYL" "$PREFIX/usr/lib/system/libsystem_kernel.dylib"
cp -a "$PPREFIX/usr/lib/dyld.prod-bak" "$PPREFIX/usr/lib/dyld"
cp -a "$PPREFIX/libexec/darling/usr/lib/dyld.prod-bak" "$PPREFIX/libexec/darling/usr/lib/dyld"
note "restore verify (srv 835946.., dyl 6bd251.. x3, dyld 79b227.. x2)"
md5sum "$PPREFIX/bin/darlingserver" "$PPREFIX/usr/lib/system/libsystem_kernel.dylib" \
       "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib" "$PREFIX/usr/lib/system/libsystem_kernel.dylib" \
       "$PPREFIX/usr/lib/dyld" "$PPREFIX/libexec/darling/usr/lib/dyld"
echo "D18A done. snapshots: $TMP/d18a-lanes*.json"
