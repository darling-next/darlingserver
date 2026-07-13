#!/usr/bin/env bash
# M1 spike gate for dar-dar6x4-perf-5dq.23 (server-side posix_spawn).
#
# Pins the namespace primitive the M1 spawn-helper depends on, in isolation:
#   RED   ("outside"): a process forked from OUTSIDE the guest PID ns (like a
#                      darlingserver worker fork) is NOT in the guest ns. This is
#                      WHY the current server cannot place a spawn into the guest.
#   GREEN ("helper") : a process forked by a MEMBER of the guest PID ns (the
#                      spike-helper, a child of the PID-1 clone) IS in the guest ns.
#
# HOST test (plain glibc). Needs PID-namespace creation: run as root, or this
# harness re-execs itself inside an unprivileged user namespace via `unshare`.
# Exit 0 on PASS.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cc="${CC:-cc}"

# If we can't create a PID ns directly (unprivileged + no prior user ns), try to
# re-enter inside a user+pid namespace. Guard against infinite re-exec.
if [ "${_SPAWN_NS_REEXEC:-0}" != "1" ]; then
	if [ "$(id -u)" != "0" ]; then
		if command -v unshare >/dev/null 2>&1; then
			export _SPAWN_NS_REEXEC=1
			exec unshare --user --map-root-user --pid --fork --mount-proc \
				bash "$here/$(basename "${BASH_SOURCE[0]}")" "$@"
		fi
	fi
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$cc" -O1 -g "$here/spawn_helper_pidns_placement.c" -o "$work/t"

# each arm returns 0 when its EXPECTED polarity holds (see the .c header):
#   outside -> exit 0 means "correctly NOT in guest ns"
#   helper  -> exit 0 means "correctly IN guest ns"
echo "--- RED proof (fork from OUTSIDE the guest ns must NOT land in it) ---"
if out_red="$("$work/t" outside)"; then
	echo "  $out_red"
	echo "  (RED arm: a from-outside fork is correctly NOT in the guest ns)"
else
	echo "  ${out_red:-<no output>}"
	echo "FAIL: a from-outside fork landed in the guest ns -- premise wrong, test is blind" >&2
	exit 2
fi

echo "--- GREEN (fork by an in-ns helper MUST land in the guest ns) ---"
if out_green="$("$work/t" helper)"; then
	echo "  $out_green"
	echo "  (GREEN arm: the in-ns helper's child is correctly in the guest ns)"
else
	echo "  ${out_green:-<no output>}"
	echo "FAIL: the in-ns helper's child did NOT land in the guest ns -- primitive broken" >&2
	exit 1
fi

echo "PASS"
