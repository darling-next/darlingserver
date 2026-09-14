#!/usr/bin/env bash
# Regression test for dar-l8k (epic dar-dar6x4-perf-5dq): in Process::notifyCheckin's
# exec/"replace task" branch, darlingserver must publish the NEW dtape thread as the
# microthread's current thread BEFORE releasing the OLD dtape thread + task. Otherwise the
# old task's IPC-space teardown (dtape_task_release -> ipc_space_terminate ->
# ipc_right_terminate -> ipc_port_destroy) reads `current_thread()->ith_assertions`, and
# current_thread() still resolves to the just-freed old dtape_thread -> heap UAF -> SIGSEGV.
#
# Pinned with an AddressSanitizer build of darlingserver, which crashes DETERMINISTICALLY
# at boot: "heap-use-after-free in ipc_port_destroy" (free at src/process.cpp:364
# dtape_thread_release, use at duct-tape/.../ipc/ipc_port.c:936 via current_thread()).
#
# Asserts (under ASan, which makes the UAF deterministic):
#   - RED   (unsafe order: release old thread, then teardown reads current==freed): UAF -> FAIL.
#   - GREEN (fix: publish new thread as current, then release old): clean -> PASS.
#
# HOST test (plain glibc + ASan). Finishes in well under a second. Exit 0 on PASS.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc="${CC:-cc}"

"$cc" -O1 -g -fsanitize=address -fno-omit-frame-pointer \
	"$here/checkin_retask_current_thread_uaf.c" -o "$work/t"

export ASAN_OPTIONS="abort_on_error=1:detect_leaks=0:halt_on_error=1"

# RED proof first: the buggy ordering MUST trip a use-after-free, else the test is blind.
echo "--- RED proof (release-before-republish, expected ASan UAF / FAIL) ---"
if "$work/t" unsafe; then
	echo "FAIL: unsafe arm unexpectedly passed -- test does not catch the use-after-free" >&2
	exit 2
fi
echo "(RED arm tripped the use-after-free as expected)"

# GREEN: publishing the new current thread before the release (the fix) must be clean.
echo "--- GREEN (republish-before-release = the fix, expected to PASS) ---"
"$work/t" safe
echo "PASS"
