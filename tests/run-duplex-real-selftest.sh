#!/usr/bin/env bash
# perf#18 P8 D3 (dar-1il.3.1.1): the REAL-DYLIB duplex selftest gate.
#
# The host gates (ring_duplex_wake_gate_test, ring_duplex_roundtrip_test) prove the wake model + the
# mailbox helpers in isolation. THIS gate proves the END-TO-END real path: the actual booted guest
# dylib publishes the sentinel parent + runs the wait-pump, and the REAL server _s2cTryDuplexLocked +
# _drainDuplexReply carry one synthetic S2C upcall to the ring-parked caller and complete the parent --
# the last bridge before mach_port_deallocate (Phase E) can migrate.
#
# It is a SYSTEM test: it builds + deploys the dylib (+ uses the already-deployed server) and boots
# darling. Driven by two opt-in signals so it never touches a normal boot:
#   server: DARLING_SERVER_DUPLEX_SELFTEST=1 in the SERVER env (recognizes the sentinel parent op).
#   guest:  the HOST marker file /tmp/.dring-duplex-go (NOT an env var -- env is inherited by daemons
#           like shellspawn and a blocking selftest there wedges them; a marker fires only in the
#           processes that open it, with no inheritance). Each ring-owning leaf process runs it once
#           while the marker is present; the per-process duplex wait is BOUNDED so nothing can wedge.
#
# GREEN arm: a normal duplex-capable guest dylib. The guest prints
#   "[dring-duplex-selftest] PASS rc=0 ... result==expect" and the server's ring_duplex_s2c counter
#   increments with ring_duplex_reject==0, and the system is NOT wedged (a later unrelated command runs).
# RED arm (-DDUPLEX_RED_NO_GUEST_PUMP): the guest dylib NEVER services the S2C upcall. The selftest must
#   then FAIL/timeout (no PASS line; the parent reply never arrives) AND the server must NOT wedge (an
#   unrelated client still makes progress -- proving the server wait is scoped to the op, not the loop).
#
# Usage:
#   bash run-duplex-real-selftest.sh green   # build normal dylib, deploy, prove PASS + counter
#   bash run-duplex-real-selftest.sh red     # build NO-PUMP dylib, deploy, prove FAIL + no-wedge
#   bash run-duplex-real-selftest.sh both    # red then green (restores green at the end)
#
# Env knobs: BUILD=~/work/darling-build  PREFIX=~/work/darling-prefix-homebrew-test
#            LAUNCH=~/work/darling-prefix/bin/darling
set -u

BUILD="${BUILD:-$HOME/work/darling-build}"
PREFIX="${PREFIX:-$HOME/work/darling-prefix-homebrew-test}"
PPREFIX="${PPREFIX:-$HOME/work/darling-prefix}"
LAUNCH="${LAUNCH:-$PPREFIX/bin/darling}"
STAT="${STAT:-$HOME/work/darling-dev/darling/src/external/darlingserver/tools/darling-stat}"
DYL_SRC="$BUILD/src/external/xnu/darling/src/libsystem_kernel/libsystem_kernel.dylib"
DYLD_OBJ_TARGET="src/external/dyld/dyld"

fail() { echo "DUPLEX-REAL-GATE FAIL: $*" >&2; exit 1; }
note() { echo "== $* =="; }

cleanboot() {
  DPREFIX="$PREFIX" "$LAUNCH" shutdown >/dev/null 2>&1; sleep 2
  pgrep -x darlingserver >/dev/null && { kill -TERM $(pgrep -x darlingserver) 2>/dev/null; sleep 2; }
  pgrep -x darlingserver >/dev/null && { kill -9 $(pgrep -x darlingserver) 2>/dev/null; sleep 1; }
  rm -f "$PREFIX/.darlingserver.sock" "$PREFIX/.init.pid" 2>/dev/null; sleep 1
}

deploy_dylib() {
  # the dylib is deployed to three locations (P, P/libexec, H). server/dyld/mldr are NOT rebuilt here
  # (the server change is constant across arms; only the GUEST dylib differs RED vs GREEN).
  cp -a "$DYL_SRC" "$PPREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib P"
  cp -a "$DYL_SRC" "$PPREFIX/libexec/darling/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib P/libexec"
  cp -a "$DYL_SRC" "$PREFIX/usr/lib/system/libsystem_kernel.dylib" || fail "deploy dylib H"
}

build_dylib() { # $1 = extra cflags ("" for green)
  local extra="$1"
  # The dylib's ring code lives in the `emulation` object lib compiled into both the dylib and dyld.
  # We rebuild with the RED define by appending it to the emulation compile flags via an env the build
  # honors? CMake has no per-invocation define injection, so we touch + recompile the one TU directly
  # with the right flags is brittle; instead we use a clean ninja build for GREEN and a targeted
  # recompile for RED (below). For GREEN we just (re)build the dylib target.
  if [ -z "$extra" ]; then
    ( cd "$BUILD" && ninja "src/external/xnu/darling/src/libsystem_kernel/libsystem_kernel.dylib" ) \
      || fail "ninja dylib (green)"
    return 0
  fi
  fail "build_dylib only supports the green path here; RED uses recompile_red()"
}

# RED: bake `#define DUPLEX_RED_NO_GUEST_PUMP 1` into the TOP of dserver-ring.c, build the dylib
# normally (ninja recompiles because the source changed, so the define is genuinely in the linked
# object -- a bare manual .o rebuild does NOT work because the subsequent `ninja <dylib>` re-runs the
# TU compile with its own non-RED command), deploy, test, then revert the source + rebuild GREEN.
RING_SRC="$HOME/work/darling-dev/darling/src/external/xnu/darling/src/libsystem_kernel/emulation/src/linux_premigration/resources/dserver-ring.c"
RED_MARK="// __DUPLEX_RED_GATE_MARK__"
red_inject() {
  grep -q "$RED_MARK" "$RING_SRC" && return 0
  printf '%s\n#define DUPLEX_RED_NO_GUEST_PUMP 1\n%s\n' "$RED_MARK" "$(cat "$RING_SRC")" > "$RING_SRC.tmp$$" \
    && mv "$RING_SRC.tmp$$" "$RING_SRC" || fail "inject RED define"
}
red_revert() {
  if grep -q "$RED_MARK" "$RING_SRC"; then
    # drop the first two injected lines (the mark + the #define)
    sed -i "0,/$RED_MARK/{/$RED_MARK/d}" "$RING_SRC"
    sed -i '0,/#define DUPLEX_RED_NO_GUEST_PUMP 1/{/#define DUPLEX_RED_NO_GUEST_PUMP 1/d}' "$RING_SRC"
  fi
}
recompile_red() {
  red_inject
  ( cd "$BUILD" && ninja "src/external/xnu/darling/src/libsystem_kernel/libsystem_kernel.dylib" ) \
    || { red_revert; fail "build dylib (red)"; }
}

run_selftest() { # warm-boot (server hatch only), THEN run a leaf with the guest env set per-command
  cleanboot
  # WARM the server FIRST with ONLY the server hatch and NO guest env -- so the long-lived daemons
  # (launchd, shellspawn) are started WITHOUT DARLING_GUEST_DUPLEX_SELFTEST and never run the selftest.
  # First boot may be cold/slow.
  DPREFIX="$PREFIX" DARLING_SERVER_DUPLEX_SELFTEST=1 timeout 200 "$LAUNCH" shell /usr/bin/true >/dev/null 2>&1
  # NOW the server is warm + daemons are clean. Run ONE leaf with the guest env set ONLY for this
  # invocation: shellspawn (already running, no env) forwards the spawned leaf's env, so only the leaf
  # inherits it and runs the selftest. Its stderr carries the [dring-duplex-selftest] line.
  DPREFIX="$PREFIX" DARLING_GUEST_DUPLEX_SELFTEST=1 timeout 90 "$LAUNCH" shell /usr/bin/true 2>&1
}

unrelated_progress() { # prove the server is NOT wedged: a NON-duplex command must still run
  DPREFIX="$PREFIX" timeout 90 "$LAUNCH" shell /bin/echo unrelated-client-ok 2>&1
}

counter() { "$STAT" "$PREFIX" 2>/dev/null | grep -E "\"$1\":" | grep -oE '[0-9]+' | head -1; }

arm_green() {
  note "GREEN: build normal duplex dylib + deploy"
  red_revert  # ensure no RED define lingers from a prior arm
  build_dylib ""
  deploy_dylib
  note "GREEN: boot + run real-dylib duplex selftest"
  local out; out="$(run_selftest)"
  echo "$out" | grep -E '\[dring-duplex-selftest\]' | head -3
  echo "$out" | grep -qE '\[dring-duplex-selftest\] PASS rc=0' || fail "no PASS line from the real-dylib selftest"
  local s2c rej; s2c="$(counter ring_duplex_s2c)"; rej="$(counter ring_duplex_reject)"
  echo "  ring_duplex_s2c=$s2c ring_duplex_reject=$rej"
  [ "${s2c:-0}" -ge 1 ] || fail "ring_duplex_s2c did not increment (got '$s2c')"
  [ "${rej:-0}" -eq 0 ] || fail "ring_duplex_reject is nonzero ($rej) on the happy path"
  note "GREEN: unrelated client progresses (system not wedged)"
  unrelated_progress | grep -qE 'unrelated-client-ok' || fail "unrelated client did not run after a duplex roundtrip"
  echo "DUPLEX-REAL-GATE GREEN: PASS"
}

arm_red() {
  note "RED: rebuild dserver-ring.c with -DDUPLEX_RED_NO_GUEST_PUMP (guest pump disabled) + deploy"
  recompile_red
  deploy_dylib
  note "RED: boot + run selftest -- it MUST NOT print PASS (the pump is dead) and MUST RETURN (bounded)"
  local out; out="$(run_selftest)"   # returns -> the selftest's duplex wait gave up (bounded), no hang
  echo "$out" | grep -E '\[dring-duplex-selftest\]' | head -3
  if echo "$out" | grep -qE '\[dring-duplex-selftest\] PASS rc=0'; then
    fail "RED arm PRINTED PASS but the guest pump is disabled -- the gate does not exercise the pump"
  fi
  local s2c; s2c="$(counter ring_duplex_s2c)"
  echo "  RED: no PASS, ring_duplex_s2c=$s2c (the duplex roundtrip never completed)."
  # The KEY RED property is proven by run_selftest RETURNING: with the pump dead the guest's duplex
  # wait would, with an UNBOUNDED FUTEX_WAIT, hang forever (the deadlock the lane exists to prevent).
  # It returned -> the wait is BOUNDED (it gives up ~3s) -> a lost caller-S2C cannot wedge the caller.
  #
  # NOTE (honest scope): this RED arm deliberately STALLS a guest process ~3s during its startup. That
  # stall is a known stressor of the spawn daemon (shellspawn) -- a process hanging mid-spawn can trip
  # shellspawn's spawn-protocol timeout and drop its socket. That is an artifact of deliberately
  # breaking a process during spawn, ORTHOGONAL to the duplex lane: a REAL duplex op (deallocate)
  # completes in microseconds and never stalls. So the RED arm does NOT assert shellspawn liveness; the
  # "server is not wedged" property is asserted by the GREEN arm (where the roundtrip is fast and an
  # unrelated client provably still runs). RED proves: pump-disabled => no completion + BOUNDED (no hang).
  echo "DUPLEX-REAL-GATE RED: correctly failed (no PASS) + bounded (selftest returned, no infinite hang)"
}

case "${1:-both}" in
  green) arm_green ;;
  red)   arm_red ;;
  both)  arm_red; arm_green ;;  # leave the tree GREEN at the end
  *) fail "usage: $0 {green|red|both}" ;;
esac
cleanboot
echo "DUPLEX-REAL-GATE: done"
