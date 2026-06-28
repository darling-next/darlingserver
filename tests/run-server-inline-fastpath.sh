#!/usr/bin/env bash
# Regression test for perf #2b (dar-dar6x4-perf-5dq.8): the darlingserver main loop must
# run cheap RPCs inline rather than waking a worker thread per message.
#
# Builds the dispatch-cost model and asserts:
#   - RED  (handoff path):  per-item worker wakeup -> ~1 ctx-switch/item -> FAILS (exit 1)
#   - GREEN (inline path):  no wakeup -> ~0 ctx-switch/item -> PASSES (exit 0)
#
# HOST test (plain glibc, no Darling runtime). Finishes in well under 1s. Exit 0 on PASS.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc="${CC:-cc}"

"$cc" -O2 -g "$here/server_inline_fastpath.c" -o "$work/sif" -lpthread

# RED proof first: the per-item handoff path MUST fail, else the test is blind.
echo "--- RED proof (per-message worker handoff, expected to FAIL) ---"
if "$work/sif" handoff; then
	echo "FAIL: handoff path unexpectedly passed -- test does not discriminate the wakeup tax" >&2
	exit 2
fi
echo "(RED path failed as expected)"

# GREEN: the inline fast-path must pass.
echo "--- GREEN (inline dispatch, expected to PASS) ---"
"$work/sif"
