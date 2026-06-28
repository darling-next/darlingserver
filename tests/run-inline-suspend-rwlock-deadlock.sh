#!/usr/bin/env bash
# Regression test for dar-6x4 (epic dar-dar6x4-perf-5dq): darlingserver must NOT hold a
# per-process rwlock (Process::_rwlock) across a microthread suspension, because perf #2b
# can run that microthread inline on the MAIN thread while it later resumes on the WORKER
# -- leaving the OS-thread-owned lock stranded and deadlocking a sibling's fork-checkin
# reader on the single worker. (Live proof: gdb __cur_writer == main tid, worker blocked
# in pthread_rwlock_rdlock, whole server wedged with rpcs_serviced flat.)
#
# Asserts:
#   - RED   (held-across-suspend): writer parks holding the write lock -> reader on the
#                                  other OS thread never proceeds -> watchdog -> FAILS.
#   - GREEN (released before suspend = the fix): reader proceeds -> both finish -> PASSES.
#
# HOST test (plain glibc + pthreads). Finishes in <5s. Exit 0 on PASS.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc="${CC:-cc}"

"$cc" -O2 -g "$here/inline_suspend_rwlock_deadlock.c" -o "$work/isr" -lpthread

# RED proof first: holding the lock across the suspend MUST deadlock, else the test is blind.
echo "--- RED proof (process lock held across suspend, expected to DEADLOCK/FAIL) ---"
if "$work/isr" heldacross; then
	echo "FAIL: held-across-suspend arm unexpectedly passed -- test does not catch the deadlock" >&2
	exit 2
fi
echo "(RED arm deadlocked as expected)"

# GREEN: releasing the lock before the suspend (the fix) must pass.
echo "--- GREEN (lock released before suspend, expected to PASS) ---"
"$work/isr" released
echo "PASS"
