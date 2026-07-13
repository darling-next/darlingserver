#!/usr/bin/env bash
# perf#18 D16 (dar-1il.11): PER-THREAD ring lanes -- live A/B measurement runner.
#
# D15a proved the reclaimable UDS pool is ~2053 POST-attach eligible calls scattered to UDS by the
# SINGLE-OWNER ring (one TID owns the one process ring; every other thread falls back to UDS). D16 gives
# each guest thread its own SPSC lane. This runner measures the headline win: post_attach_eligible_UDS
# on the eligible ops should DROP sharply once non-owner threads ride their own lanes.
#
# It deploys a MATCHED server (built from this HEAD, with the D14 heatmap + D15a attach census armed) and
# a chosen guest dylib (pre-D16 single-owner baseline OR the D16 per-thread build), warm-boots, runs the
# SAME warm workload (exec batches + a fork-storm), and dumps the attach-census + heatmap snapshot. A
# matched dylib+server keeps attach_rejects ~0 so the post-attach pool is the clean signal (D15a's 29%
# ABI-reject was a homebrew-test dylib-version-skew artifact, not architectural).
#
# Usage:
#   bash run-per-thread-lanes-d16.sh baseline   # deploy the PRE-D16 dylib, measure (confirm ~2053)
#   bash run-per-thread-lanes-d16.sh d16         # deploy the D16 per-thread dylib, measure (the win)
#
# Env: BUILD, PPREFIX (host prefix w/ server+launcher), PREFIX (homebrew-test guest prefix),
#      TMP (where the staged dylibs/server live), STAT (darling-stat).
set -u

BUILD="${BUILD:-$HOME/work/darling-build}"
PPREFIX="${PPREFIX:-$HOME/work/darling-prefix}"
PREFIX="${PREFIX:-$HOME/work/darling-prefix-homebrew-test}"
LAUNCH="${LAUNCH:-$PPREFIX/bin/darling}"
STAT="${STAT:-$HOME/work/darling-dev/darling/src/external/darlingserver/tools/darling-stat}"
TMP="${TMP:-/home/ilyagulya/.claude-work/jobs/ae391f52/tmp}"
SRV_SRC="$TMP/darlingserver.d16srv"            # matched census server (built this HEAD)
DYL_PRE="$TMP/libsystem_kernel.preD16.dylib"   # single-owner baseline dylib
DYL_D16="$TMP/libsystem_kernel.d16.dylib"      # per-thread lane dylib

fail() { echo "D16-RUN FAIL: $*" >&2; exit 1; }
note() { echo "== $* =="; }

cleanboot() {
  DPREFIX="$PREFIX" "$LAUNCH" shutdown >/dev/null 2>&1; sleep 2
  pgrep -x darlingserver >/dev/null && { kill -TERM $(pgrep -x darlingserver) 2>/dev/null; sleep 2; }
  pgrep -x darlingserver >/dev/null && { kill -9 $(pgrep -x darlingserver) 2>/dev/null; sleep 1; }
  rm -f "$PREFIX/.darlingserver.sock" "$PREFIX/.init.pid" 2>/dev/null; sleep 1
}

deploy_server() {
  cleanboot
  cp -a "$SRV_SRC" "$PPREFIX/bin/darlingserver" || fail "deploy server"
}

deploy_dylib() { # $1 = dylib path
  local d="$1"
  cp -a "$d" "$PPREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib P"
  cp -a "$d" "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib P/libexec"
  cp -a "$d" "$PREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib H"
}

# dyld also links the ring-attach code (emulation_dyld). The deployed prod dyld predates ABI v5, so its
# early (pre-exec mldr-phase) attach is rejected reason=abi(2) -> a constant reject floor that pollutes
# the post-attach signal. Deploy a MATCHED dyld (built from this HEAD, ABI v5 + the D16 lane code) so
# rejects fall toward ~0 and the post-attach pool is the clean A/B signal. Optional ($DEPLOY_DYLD=1).
deploy_dyld() { # $1 = dyld path
  local d="$1"
  cp -a "$d" "$PPREFIX/usr/lib/dyld" || fail "deploy dyld P"
  cp -a "$d" "$PPREFIX/libexec/darling/usr/lib/dyld" || fail "deploy dyld P/libexec"
}

# The warm workload: WARM the server first (census + heatmap armed in the SERVER env so launchd/shellspawn
# inherit the arming -- these are pure-measurement env vars, safe to inherit, unlike a stalling test hook).
# Then run several exec batches (each `darling shell` spawns a multi-threaded leaf that issues the eligible
# ops on >1 thread -- exactly the traffic the single-owner ring scattered to UDS) + a fork-storm.
warm_and_workload() {
  note "warm boot (census + heatmap armed in server env; first boot is cold/slow)"
  DPREFIX="$PREFIX" DARLING_SERVER_ATTACH_CENSUS=1 DARLING_SERVER_RPC_HEATMAP=1 \
    timeout 200 "$LAUNCH" shell /usr/bin/true >/dev/null 2>&1
  note "exec batches (multi-thread leaves -> eligible ops across threads)"
  for i in $(seq 1 12); do
    DPREFIX="$PREFIX" timeout 90 "$LAUNCH" shell /bin/echo "batch-$i" >/dev/null 2>&1
    DPREFIX="$PREFIX" timeout 90 "$LAUNCH" shell /usr/bin/true >/dev/null 2>&1
  done
  note "fork-storm (30 children, inside the guest -- guest-visible path only)"
  DPREFIX="$PREFIX" timeout 120 "$LAUNCH" shell /bin/sh -c '
    i=0; while [ $i -lt 30 ]; do ( /usr/bin/true ) & i=$((i+1)); done; wait' >/dev/null 2>&1
}

snapshot() { # $1 = out file
  "$STAT" "$PREFIX" > "$1" 2>/dev/null || fail "stat snapshot failed (server down / metrics off?)"
}

ARM="${1:-d16}"
case "$ARM" in
  baseline) DYL="$DYL_PRE"; OUT="$TMP/d16-snapshot-baseline.json" ;;
  d16)      DYL="$DYL_D16"; OUT="$TMP/d16-snapshot-d16.json" ;;
  *) fail "usage: $0 {baseline|d16}" ;;
esac

note "ARM=$ARM  dylib=$(basename "$DYL")  DEPLOY_DYLD=${DEPLOY_DYLD:-0}"
deploy_server
deploy_dylib "$DYL"
[ "${DEPLOY_DYLD:-0}" = "1" ] && deploy_dyld "$TMP/dyld.d16"
warm_and_workload
snapshot "$OUT"
cleanboot
echo "D16-RUN $ARM: snapshot -> $OUT"
python3 "$TMP/elig.py" "$OUT"
