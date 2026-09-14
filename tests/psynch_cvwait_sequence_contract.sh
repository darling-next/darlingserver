#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="${DSERVER_SRC_ROOT:-$(git -C "$here" rev-parse --show-toplevel)}"
src="$root/duct-tape/pthread/kern_synch.c"

fail() {
	printf 'psynch_cvwait_sequence_contract: %s\n' "$*" >&2
	exit 1
}

[ -f "$src" ] || fail "kern_synch.c not found at $src"

body="$(
	python3 - "$src" <<'PY'
from pathlib import Path
import sys

text = Path(sys.argv[1]).read_text()
start = text.find("_psynch_cvwait(")
if start < 0:
    raise SystemExit("_psynch_cvwait not found")
next_func = text.find("\nint\n", start + 1)
if next_func < 0:
    next_func = len(text)
print(text[start:next_func])
PY
)"

grep -q 'is_seqhigher(csgen, lockseq)' <<<"$body" ||
	fail "cvwait rejects balanced equal sequence numbers"
if grep -q 'is_seqhigher_eq(csgen, lockseq)' <<<"$body"; then
	fail "cvwait still uses inclusive stale-sequence check"
fi
grep -q '__FAILEDUSERTEST2__' <<<"$body" ||
	fail "invalid-sequence path has no diagnostic context"

printf 'PSYNCH_CVWAIT_SEQUENCE_CONTRACT_OK\n'
