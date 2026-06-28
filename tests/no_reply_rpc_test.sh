#!/usr/bin/env bash
# perf #10 (dar-dar6x4-perf-5dq.17) regression test: the NO_REPLY ("fire-and-forget")
# RPC class.
#
# The per-call RPC profile (perf #9) showed ~40% of build RPCs are once-per-process
# pure SETTERS with an EMPTY reply (set_dyld_info, set_executable_path,
# set_thread_handles, ...). Each still pays a full synchronous round-trip: send + a
# ~200us recvmsg sleep, only to read a status code the guest discards. That sleep IS
# the 57% guest wall-clock the profile (perf #6) measured.
#
# A NO_REPLY call must:
#   GUEST  (library source wrapper): send the message and RETURN immediately -- no
#          receive_message call, so the thread never sleeps for an ack.
#   SERVER (internal header _sendReply): be a NO-OP -- the call still runs its work in
#          processCall(), but emits NO reply datagram (a stray reply would desync the
#          next synchronous RPC's reply matching on the same per-thread socket).
#
# RED->GREEN: this drives a generator change. Before the NO_REPLY flag exists, the
# generated wrapper for a NO_REPLY call still contains receive_message (RED). After,
# the wrapper for a NO_REPLY call omits receive_message AND its _sendReply body is
# empty, while a normal reply-less call (e.g. a non-flagged setter) STILL receives.
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
INT="$TMP/internal.h"
fails=0

# Extract the generated wrapper body for a call. The real send/receive logic lives in
# the `dserver_rpc_explicit_<name>(int server_socket, ...)` variant (the thin
# `dserver_rpc_<name>` just forwards to it), so scope the grep to the explicit wrapper.
wrapper_body() { # $1 = call name
	awk -v fn="int dserver_rpc_explicit_$1(" '
		index($0, fn) == 1 { inb=1 }
		inb { print }
		inb && ($0 == "}" || $0 == "};") { exit }
	' "$LIB"
}

# A NO_REPLY call: wrapper must NOT wait for a reply.
NR_CALL="set_dyld_info"
if wrapper_body "$NR_CALL" | grep -q "receive_message"; then
	echo "FAIL: NO_REPLY call '$NR_CALL' wrapper still calls receive_message (does NOT fire-and-forget)"
	fails=$((fails+1))
fi

# Its server-side _sendReply must be a no-op (no Message/pushCallReply emitted for it).
# The generated _sendReply is inside the per-call class macro. Scope to the
# `class Call::SetDyldInfo:` definition (NOT the `class SetDyldInfo;` forward decl),
# ending at the start of the next class definition.
awk '
	index($0, "class Call::SetDyldInfo:") > 0 { inblk=1 }
	inblk && index($0, "class Call::") > 0 && index($0, "SetDyldInfo") == 0 { exit }
	inblk { print }
' "$INT" > "$TMP/sdi.int"
# Its _sendReply must NOT emit a reply (no pushCallReply / Call::sendReply) and MUST
# carry the fire-and-forget no-op marker.
if grep -q "pushCallReply\|Call::sendReply" "$TMP/sdi.int"; then
	echo "FAIL: NO_REPLY call '$NR_CALL' server _sendReply still emits a reply datagram"
	fails=$((fails+1))
fi
if ! grep -q "NO_REPLY: fire-and-forget" "$TMP/sdi.int"; then
	echo "FAIL: NO_REPLY call '$NR_CALL' server _sendReply missing the no-op marker"
	fails=$((fails+1))
fi

# CONTROL: a normal (non-NO_REPLY) call MUST still receive, so we didn't break the
# default path. checkin returns data and must stay synchronous.
if ! wrapper_body "checkin" | grep -q "receive_message"; then
	echo "FAIL: normal call 'checkin' wrapper no longer receives (default path broken)"
	fails=$((fails+1))
fi

if [ "$fails" -eq 0 ]; then
	echo "GREEN: NO_REPLY calls fire-and-forget (no recv, no server reply); normal calls still receive"
	exit 0
fi
echo "RED: $fails NO_REPLY check(s) failed"
exit 1
