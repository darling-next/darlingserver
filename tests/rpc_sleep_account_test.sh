#!/usr/bin/env bash
# perf #9-decompose (dar-dar6x4-perf-5dq): GUEST-SIDE per-callnum recvmsg-sleep
# accounting.
#
# perf #6 profiled a real build: the guest spends ~57% of wall-clock asleep in
# recvmsg waiting for an RPC reply. perf #9 measured only the SERVER side (service
# time, tiny everywhere), so we still don't know WHICH call numbers the guest sleeps
# on longest -- and that is exactly what we must know to attack the 57% instead of
# guessing one call at a time.
#
# The fix is a guest-side accountant that times the recv (reply-wait) and attributes
# the elapsed time to the call number being waited on. The call number is a
# compile-time constant ONLY at the wrapper's receive site (dserver_callnum_<name>),
# not inside the shared receive_message hook -- so the timing must be emitted by the
# GENERATOR around the receive call, not buried in the hook (which would have no
# callnum). To stay zero-cost by default and avoid touching the hook signature (which
# has 3 divergent copies), the timing is wrapped in #ifdef DARLING_RPC_SLEEP_ACCOUNT:
# a normal build emits byte-identical wrappers; only an instrumented diagnostic build
# defines the macro.
#
# RED->GREEN: this drives the generator change. Before it, the generated wrapper has
# no per-callnum accounting hooks around the receive (RED). After, a reply-bearing
# call's wrapper brackets its receive with __darling_rpc_sleep_account_begin() ...
# __darling_rpc_sleep_account_end(dserver_callnum_<name>), all under the
# DARLING_RPC_SLEEP_ACCOUNT guard, and a NO_REPLY (fire-and-forget) call -- which
# never receives -- gets NO accounting.
#
# Hermetic: runs the python generator into a temp dir and greps the output. No build,
# no server. Exit 0 = GREEN.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
GEN="$HERE/../scripts/generate-rpc-wrappers.py"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 "$GEN" "$TMP/public.h" "$TMP/internal.h" "$TMP/library.c" "test-import" || {
	echo "FAIL: generator did not run"; exit 1; }

LIB="$TMP/library.c"
fails=0

# Extract the generated wrapper body for a call (the explicit variant carries the
# real send/receive logic; the thin dserver_rpc_<name> just forwards).
wrapper_body() { # $1 = call name
	awk -v fn="int dserver_rpc_explicit_$1(" '
		index($0, fn) == 1 { inb=1 }
		inb { print }
		inb && ($0 == "}" || $0 == "};") { exit }
	' "$LIB"
}

# 1) A normal reply-bearing call must bracket its receive with the accountant. The
#    begin must precede the receive_message and the end must name the call number.
CALL="checkin"
body="$(wrapper_body "$CALL")"

if ! grep -q "__darling_rpc_sleep_account_begin" <<<"$body"; then
	echo "FAIL: '$CALL' wrapper has no __darling_rpc_sleep_account_begin around its receive"
	fails=$((fails+1))
fi
if ! grep -q "__darling_rpc_sleep_account_end(__sleep_acct_start, dserver_callnum_$CALL" <<<"$body"; then
	echo "FAIL: '$CALL' wrapper does not attribute its recv time to dserver_callnum_$CALL"
	fails=$((fails+1))
fi

# 2) The accounting must be compile-time gated so default builds are byte-identical:
#    the begin/end must appear only inside a DARLING_RPC_SLEEP_ACCOUNT #ifdef block.
if ! grep -q "DARLING_RPC_SLEEP_ACCOUNT" <<<"$body"; then
	echo "FAIL: '$CALL' accounting is not guarded by DARLING_RPC_SLEEP_ACCOUNT (would cost on default builds)"
	fails=$((fails+1))
fi

# 3) begin must come BEFORE the receive_message call, end AFTER it (we time the wait).
begin_ln="$(grep -n "__darling_rpc_sleep_account_begin" <<<"$body" | head -1 | cut -d: -f1)"
recv_ln="$(grep -n "dserver_rpc_hooks_receive_message" <<<"$body" | head -1 | cut -d: -f1)"
end_ln="$(grep -n "__darling_rpc_sleep_account_end" <<<"$body" | tail -1 | cut -d: -f1)"
if [ -n "$begin_ln" ] && [ -n "$recv_ln" ] && [ -n "$end_ln" ]; then
	if ! { [ "$begin_ln" -lt "$recv_ln" ] && [ "$recv_ln" -lt "$end_ln" ]; }; then
		echo "FAIL: '$CALL' accounting does not bracket the receive (begin=$begin_ln recv=$recv_ln end=$end_ln)"
		fails=$((fails+1))
	fi
fi

# 4) A NO_REPLY (fire-and-forget) call never receives, so it must have NO accounting.
NR_CALL="set_dyld_info"
if wrapper_body "$NR_CALL" | grep -q "__darling_rpc_sleep_account"; then
	echo "FAIL: NO_REPLY call '$NR_CALL' has sleep accounting but it never receives"
	fails=$((fails+1))
fi

if [ "$fails" -eq 0 ]; then
	echo "GREEN: reply-bearing wrappers bracket their receive with guarded per-callnum sleep accounting; NO_REPLY calls have none"
	exit 0
fi
echo "RED: $fails sleep-accounting check(s) failed"
exit 1
