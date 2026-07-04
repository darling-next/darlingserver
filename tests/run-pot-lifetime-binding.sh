#!/usr/bin/env bash
# dar-pot RED->GREEN gate driver for pot_lifetime_binding_test.c.
#
# Models darlingserver(host ns) -> launchd(clone CLONE_NEWPID, ns pid1) -> guest.
# Asserts the lifetime-binding predicate: a guest must NOT survive its server.
#
#   RED  arm ("nobind"): launchd sets no PDEATHSIG. Killing the server leaves the
#            guest orphaned/alive. The arm MUST report GUEST-SURVIVED (exit 0);
#            if the guest died anyway, the model is blind -> gate fails.
#   GREEN arm ("bind"):  launchd sets PR_SET_PDEATHSIG(SIGKILL). Killing the server
#            cascade-kills the guest. The arm MUST report GUEST-REAPED (exit 0).
#
# Needs pid-namespace creation. If unavailable, the test exits 77 (SKIP) and we
# retry inside an unprivileged user namespace via `unshare -Ur`. Exit 0 only if
# BOTH arms meet their expectation (or both SKIP).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/pot_lifetime_binding_test.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CC="${CC:-gcc}"

BIN="$TMP/pot"
if ! "$CC" -O2 -Wall -o "$BIN" "$SRC"; then
	echo "gate FAILED to COMPILE"; exit 2
fi

# Run one arm. pid-ns creation needs privilege. Preference order:
#   1. direct (works if already root / already in a user ns)
#   2. sudo -n (darlingserver is itself setuid-root when it clones launchd, so
#      running the model as root is faithful, not a cheat)
#   3. unprivileged user ns via `unshare -Ur` (only where userns is enabled)
# A run that SKIPs (exit 77) means pid-ns was denied; fall through to the next.
run_arm() {
	local arm="$1" out rc
	out="$("$BIN" "$arm" 2>&1)"; rc=$?
	if [ "$rc" -eq 77 ] && command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
		out="$(sudo -n "$BIN" "$arm" 2>&1)"; rc=$?
	fi
	if [ "$rc" -eq 77 ] && command -v unshare >/dev/null 2>&1; then
		out="$(unshare -Ur "$BIN" "$arm" 2>&1)"; rc=$?
	fi
	echo "$out"
	return "$rc"
}

echo "== dar-pot lifetime-binding gate =="
overall=0
skipped=0

for arm in nobind bind; do
	echo "-- arm: $arm --"
	out="$(run_arm "$arm")"; rc=$?
	echo "  $out"
	if [ "$rc" -eq 77 ]; then
		echo "  SKIP (no pid-ns; run as root or where user namespaces are enabled)"
		skipped=$((skipped+1))
		continue
	fi
	if [ "$rc" -eq 0 ]; then
		echo "  OK ($arm met its expectation)"
	else
		echo "  FAIL ($arm did NOT meet its expectation, rc=$rc)"
		overall=1
	fi
done

echo
if [ "$skipped" -eq 2 ]; then
	echo "== dar-pot gate SKIPPED (pid-ns unavailable in this environment) =="
	exit 0
fi
if [ "$overall" -eq 0 ]; then
	echo "== dar-pot gate GREEN: RED(nobind) reproduces the orphan, GREEN(bind) closes it =="
else
	echo "== dar-pot gate RED: a predicate arm failed =="
fi
exit "$overall"
