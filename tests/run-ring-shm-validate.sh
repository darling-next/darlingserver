#!/usr/bin/env bash
# perf#18 (dar-dar6x4-perf-5dq.30) RED->GREEN gate for the ring control-block trust-boundary
# validator. Hermetic: header-only, host g++, no boot/sockets.
#
#   RED  arm: compile the test against an accept-all stub validator (-DRING_VALIDATE_STUB).
#            Every adversarial case must FAIL (stub accepts malformed input) -> the test is real.
#   GREEN arm: compile against the real dserver_ring_shm_validate(); all checks pass.
#
# Exit 0 only if RED fails (as it must) AND GREEN passes. Run from anywhere.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
INC="$(cd "$HERE/../include" && pwd)"
SRC="$HERE/ring_shm_validate_test.cpp"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CXX="${CXX:-g++}"
CC="${CC:-gcc}"

# The GENERATED rpc.h supplies the dserver_callnum_* values. As of dar-1il.2 item 2 the validator
# (and so the validate/attach/datapath/wake-race tests that exercise it) folds the C2S opcode set
# into a hash that needs those callnums, so these tests now need rpc.h on the include path. Discover
# it once up front; if a tree isn't built, SKIP (don't fail) the rpc.h-dependent arms.
GEN_RPC=""
for cand in "${DSERVER_GEN_INC:-}" \
            "$HERE/../../../../build/src/external/darlingserver/include" \
            "$HOME/work/darling-build/src/external/darlingserver/include"; do
	[ -n "$cand" ] && [ -f "$cand/darlingserver/rpc.h" ] && { GEN_RPC="$cand"; break; }
done
GEN_INC_FLAG=""
[ -n "$GEN_RPC" ] && GEN_INC_FLAG="-I$GEN_RPC"

if [ -z "$GEN_RPC" ]; then
	echo "== validator/attach/datapath gates SKIPPED (generated rpc.h not found; set DSERVER_GEN_INC) =="
	echo
else

echo "== RED arm (accept-all stub: adversarial cases MUST fail) =="
if ! "$CXX" -std=c++17 $GEN_INC_FLAG -I"$INC" -DRING_VALIDATE_STUB -o "$TMP/red" "$SRC"; then
	echo "RED arm failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/red"; then
	echo "RED arm PASSED but must FAIL (stub accepts everything) -- gate not exercising the validator"; exit 1
fi
echo "  RED arm correctly failed."
echo

echo "== GREEN arm (real validator: all checks pass) =="
if ! "$CXX" -std=c++17 $GEN_INC_FLAG -I"$INC" -o "$TMP/green" "$SRC"; then
	echo "GREEN arm failed to COMPILE"; exit 2
fi
if ! "$TMP/green"; then
	echo "GREEN arm FAILED -- validator rejects/accepts incorrectly"; exit 1
fi
echo
echo "ring_shm_validate gate: RED->GREEN OK"
echo

# --- ring_attach decision (real memfd: fstat-size-check + map + validate) ---
ASRC="$HERE/ring_attach_check_test.cpp"
echo "== attach-check RED arm (accept-all stub: adversarial cases MUST fail) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE $GEN_INC_FLAG -I"$INC" -DRING_ATTACH_STUB -o "$TMP/ared" "$ASRC"; then
	echo "attach RED arm failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/ared"; then
	echo "attach RED arm PASSED but must FAIL -- gate not exercising the attach-check"; exit 1
fi
echo "  attach RED arm correctly failed."
echo
echo "== attach-check GREEN arm (real attach-check on real memfds) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE $GEN_INC_FLAG -I"$INC" -o "$TMP/agreen" "$ASRC"; then
	echo "attach GREEN arm failed to COMPILE"; exit 2
fi
if ! "$TMP/agreen"; then
	echo "attach GREEN arm FAILED"; exit 1
fi
echo
echo "ring_attach_check gate: RED->GREEN OK"
echo

# --- ring datapath (P3): SPSC + server C2S service loop + reply convention, on a real
#     memfd-backed ring, with adversarial corruption + backpressure. ASan on both arms. ---
DSRC="$HERE/ring_datapath_test.cpp"
echo "== datapath GREEN arm (real SPSC + service loop, ASan) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -fsanitize=address $GEN_INC_FLAG -I"$INC" -o "$TMP/dp_green" "$DSRC"; then
	echo "datapath GREEN arm failed to COMPILE"; exit 2
fi
if ! "$TMP/dp_green"; then
	echo "datapath GREEN arm FAILED"; exit 1
fi
echo
echo "== datapath RED arm (accept-all stub, ASan: adversarial cases MUST fail) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -fsanitize=address -DRING_DATAPATH_STUB $GEN_INC_FLAG -I"$INC" -o "$TMP/dp_red" "$DSRC"; then
	echo "datapath RED arm failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/dp_red"; then
	echo "datapath RED arm PASSED but must FAIL -- gate not exercising the datapath"; exit 1
fi
echo "  datapath RED arm correctly failed."
echo
echo "ring_datapath gate: RED->GREEN OK"
echo

fi # GEN_RPC present (validator/attach/datapath gates)

# --- ring eligibility ALLOWLIST (P3): the real dserver_callnum_* set that may ride the ring.
#     Needs the GENERATED rpc.h. Find it under a build dir; skip (don't fail) if not built. ---
LSRC="$HERE/ring_allowlist_test.cpp"
GEN_RPC=""
for cand in "${DSERVER_GEN_INC:-}" \
            "$HERE/../../../../build/src/external/darlingserver/include" \
            "$HOME/work/darling-build/src/external/darlingserver/include"; do
	[ -n "$cand" ] && [ -f "$cand/darlingserver/rpc.h" ] && { GEN_RPC="$cand"; break; }
done
if [ -z "$GEN_RPC" ]; then
	echo "== allowlist gate SKIPPED (generated rpc.h not found; set DSERVER_GEN_INC) =="
	echo
else
	echo "== allowlist GREEN arm (current set, derived from the shared DSERVER_RING_C2S_OPCODES macro) =="
	# -DDSERVER_RING_TRANSPORT so the macro (defined inside that ifdef in rpc-supplement.h) is visible;
	# the gate derives eligibility from the SAME macro the server uses, so it can never drift.
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -I"$GEN_RPC" -I"$INC" -o "$TMP/al_green" "$LSRC"; then
		echo "allowlist GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/al_green"; then
		echo "allowlist GREEN arm FAILED"; exit 1
	fi
	echo
	echo "== allowlist RED arm (stale pre-migration hand-list: task_self_trap only, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DALLOWLIST_OLD -I"$GEN_RPC" -I"$INC" -o "$TMP/al_red" "$LSRC"; then
		echo "allowlist RED arm failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/al_red"; then
		echo "allowlist RED arm PASSED but must FAIL -- gate not exercising the migration"; exit 1
	fi
	echo "  allowlist RED arm correctly failed."
	echo
	echo "ring_allowlist gate: RED->GREEN OK"
	echo

	# --- P6.1 fast-path eligibility + escape hatches (dar-ohp). Reuses $GEN_RPC. ---
	FPSRC="$HERE/ring_fastpath_gate_test.c"
	if [ -f "$FPSRC" ]; then
		echo "== fast-path GREEN arm (hatch-aware: allowlist + env kill-switches) =="
		if ! "$CC" -std=c11 -I"$GEN_RPC" -I"$INC" -o "$TMP/fp_green" "$FPSRC" 2>/dev/null; then
			echo "fast-path GREEN arm failed to COMPILE"; exit 2
		fi
		if ! "$TMP/fp_green"; then
			echo "fast-path GREEN arm FAILED"; exit 1
		fi
		echo
		echo "== fast-path RED arm (-DFASTPATH_NO_HATCH: ignores hatches, MUST fail) =="
		if ! "$CC" -std=c11 -DFASTPATH_NO_HATCH -I"$GEN_RPC" -I"$INC" -o "$TMP/fp_red" "$FPSRC" 2>/dev/null; then
			echo "fast-path RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/fp_red" >/dev/null 2>&1; then
			echo "fast-path RED arm PASSED but must FAIL -- gate not exercising the hatches"; exit 1
		fi
		echo "  fast-path RED arm correctly failed."
		echo
		echo "== fast-path shape RED arm (-DNO_SHAPE_GUARD: drops the direct-dispatch shape guard, MUST fail) =="
		if ! "$CC" -std=c11 -DNO_SHAPE_GUARD -I"$GEN_RPC" -I"$INC" -o "$TMP/fp_shape_red" "$FPSRC" 2>/dev/null; then
			echo "fast-path shape RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/fp_shape_red" >/dev/null 2>&1; then
			echo "fast-path shape RED arm PASSED but must FAIL -- gate not exercising the shape guard"; exit 1
		fi
		echo "  fast-path shape RED arm correctly failed."
		echo
		echo "== mod_refs C2S exclusion RED arm (-DC2S_READD_MODREFS: re-adds the destroy-capable op, MUST fail) =="
		if ! "$CC" -std=c11 -DC2S_READD_MODREFS -I"$GEN_RPC" -I"$INC" -o "$TMP/fp_c2s_red" "$FPSRC" 2>/dev/null; then
			echo "mod_refs C2S RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/fp_c2s_red" >/dev/null 2>&1; then
			echo "mod_refs C2S RED arm PASSED but must FAIL -- gate not pinning mod_refs OUT of the C2S allowlist"; exit 1
		fi
		echo "  mod_refs C2S RED arm correctly failed."
		echo
		echo "ring_fastpath gate: RED->GREEN OK"
		echo
	fi

	# --- P5-bulk (dar-1il.1): no-silent-drop drift gate. Guest "may publish" set == server "will
	#     service" set (both derived from the shared DSERVER_RING_C2S_OPCODES macro). RED arm
	#     -DDRIFT_GUEST_EXTRA gives the guest an opcode the server won't service -> the gate fails. ---
	DRSRC="$HERE/ring_drift_gate_test.c"
	if [ -f "$DRSRC" ]; then
		echo "== drift GREEN arm (guest C2S set == server C2S set) =="
		if ! "$CC" -std=c11 -I"$GEN_RPC" -I"$INC" -o "$TMP/dr_green" "$DRSRC" 2>/dev/null; then
			echo "drift GREEN arm failed to COMPILE"; exit 2
		fi
		if ! "$TMP/dr_green"; then
			echo "drift GREEN arm FAILED -- guest and server C2S allowlists are not equal"; exit 1
		fi
		echo
		echo "== drift RED arm (-DDRIFT_GUEST_EXTRA: guest routes an op the server drops, MUST fail) =="
		if ! "$CC" -std=c11 -DDRIFT_GUEST_EXTRA -I"$GEN_RPC" -I"$INC" -o "$TMP/dr_red" "$DRSRC" 2>/dev/null; then
			echo "drift RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/dr_red" >/dev/null 2>&1; then
			echo "drift RED arm PASSED but must FAIL -- gate not exercising the no-silent-drop invariant"; exit 1
		fi
		echo "  drift RED arm correctly failed."
		echo
		echo "ring_drift gate: RED->GREEN OK"
		echo
	fi

	# --- Phase A (dar-dar6x4-perf-5dq.30.1): THREE-LANE op classification + the static guardrail that
	#     a destroy-capable / caller-S2C op CANNOT enter the simple ring. Two kinds of arm:
	#       (i)  C host gate (ring_lane_class_gate_test.c): GREEN runs; RED -DLANECLASS_READD_DEALLOCATE
	#            and -DLANECLASS_DESTROY_IN_RING fold the canon predicate over a drifted/mis-tagged set
	#            and MUST exit nonzero.
	#       (ii) C++ COMPILE-TIME guardrail: the header's static_assert(dserver_ring_c2s_set_is_canon_safe)
	#            MUST compile clean GREEN and MUST FAIL TO COMPILE under
	#            -DDSERVER_RING_LANECLASS_RED_DESTROY_IN_RING (which OR's DestroyCapable onto a real
	#            simple-ring member). This proves the compile-time guardrail is live, not vacuous. ---
	LCSRC="$HERE/ring_lane_class_gate_test.c"
	if [ -f "$LCSRC" ]; then
		echo "== lane-class GREEN arm (three-lane classification + canon guardrail hold) =="
		if ! "$CC" -std=c11 -I"$GEN_RPC" -I"$INC" -o "$TMP/lc_green" "$LCSRC" 2>/dev/null; then
			echo "lane-class GREEN arm failed to COMPILE"; exit 2
		fi
		if ! "$TMP/lc_green"; then
			echo "lane-class GREEN arm FAILED"; exit 1
		fi
		echo
		echo "== lane-class RED arm A (-DLANECLASS_READD_DEALLOCATE: destroy-capable op in the set, MUST fail) =="
		if ! "$CC" -std=c11 -DLANECLASS_READD_DEALLOCATE -I"$GEN_RPC" -I"$INC" -o "$TMP/lc_redA" "$LCSRC" 2>/dev/null; then
			echo "lane-class RED arm A failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/lc_redA" >/dev/null 2>&1; then
			echo "lane-class RED arm A PASSED but must FAIL -- the canon fold does not reject a destroy-capable member"; exit 1
		fi
		echo "  lane-class RED arm A correctly failed."
		echo
		echo "== lane-class RED arm B (-DLANECLASS_DESTROY_IN_RING: mis-tag a real simple-ring op, MUST fail) =="
		if ! "$CC" -std=c11 -DLANECLASS_DESTROY_IN_RING -I"$GEN_RPC" -I"$INC" -o "$TMP/lc_redB" "$LCSRC" 2>/dev/null; then
			echo "lane-class RED arm B failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/lc_redB" >/dev/null 2>&1; then
			echo "lane-class RED arm B PASSED but must FAIL -- the canon fold does not read the destroy bit"; exit 1
		fi
		echo "  lane-class RED arm B correctly failed."
		echo
		# COMPILE-TIME guardrail: the static_assert in the header.
		echo "== lane-class static_assert GREEN (header must COMPILE in C++) =="
		printf '#define DSERVER_RING_TRANSPORT 1\n#define DSERVER_RING_NO_ATTACH_CHECK 1\n#include <darlingserver/rpc.h>\n#include <darlingserver/rpc-supplement.h>\nint main(){return 0;}\n' > "$TMP/lc_sa.cpp"
		if ! "$CXX" -std=c++17 -I"$GEN_RPC" -I"$INC" -fsyntax-only "$TMP/lc_sa.cpp" 2>/dev/null; then
			echo "lane-class static_assert GREEN FAILED to compile -- the shipped classification violates the canon"; exit 1
		fi
		echo "  static_assert GREEN compiles."
		echo "== lane-class static_assert RED (-DDSERVER_RING_LANECLASS_RED_DESTROY_IN_RING MUST FAIL to compile) =="
		if "$CXX" -std=c++17 -DDSERVER_RING_LANECLASS_RED_DESTROY_IN_RING -I"$GEN_RPC" -I"$INC" -fsyntax-only "$TMP/lc_sa.cpp" 2>/dev/null; then
			echo "lane-class static_assert RED COMPILED but must FAIL -- the compile-time guardrail is vacuous"; exit 1
		fi
		echo "  static_assert RED correctly failed to compile."
		echo
		echo "ring_lane_class gate: RED->GREEN OK"
		echo
	fi
fi

# --- P7 wake-model LOST-WAKE race gate (dar-my8). Hermetic: header-only, real shm, two threads.
#     GREEN: exhaustive ordering enumeration finds ZERO lost-wake interleavings on both the
#     request and response sides + the concurrent stress is fully live. RED arms break each
#     safety mechanism (server critical recheck / guest set-waiter-bit-before-recheck) so the
#     enumeration finds lost > 0 and the binary exits nonzero. ---
WRSRC="$HERE/ring_wake_race_test.cpp"
WR_ITERS="${RING_WAKE_RACE_ITERS:-8000}"
echo "== wake-race GREEN arm (no lost-wake interleaving; stress live) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -O2 -pthread $GEN_INC_FLAG -I"$INC" -o "$TMP/wr_green" "$WRSRC"; then
	echo "wake-race GREEN arm failed to COMPILE"; exit 2
fi
if ! "$TMP/wr_green" "$WR_ITERS"; then
	echo "wake-race GREEN arm FAILED -- a lost-wake invariant does not hold"; exit 1
fi
echo
echo "== wake-race RED arm A (-DRACE_NO_SERVER_RECHECK: server skips critical recheck, MUST fail) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -O2 -pthread -DRACE_NO_SERVER_RECHECK $GEN_INC_FLAG -I"$INC" -o "$TMP/wr_redA" "$WRSRC"; then
	echo "wake-race RED arm A failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/wr_redA" "$WR_ITERS" >/dev/null 2>&1; then
	echo "wake-race RED arm A PASSED but must FAIL -- gate not exercising the server critical recheck"; exit 1
fi
echo "  wake-race RED arm A correctly failed."
echo
echo "== wake-race RED arm B (-DRACE_WAITERBIT_AFTER_RECHECK: guest sets waiter bit too late, MUST fail) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -O2 -pthread -DRACE_WAITERBIT_AFTER_RECHECK $GEN_INC_FLAG -I"$INC" -o "$TMP/wr_redB" "$WRSRC"; then
	echo "wake-race RED arm B failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/wr_redB" "$WR_ITERS" >/dev/null 2>&1; then
	echo "wake-race RED arm B PASSED but must FAIL -- gate not exercising the guest waiter-bit ordering"; exit 1
fi
echo "  wake-race RED arm B correctly failed."
echo
echo "ring_wake_race gate: RED->GREEN OK"
echo

# --- D16 (dar-1il.11): PER-THREAD lane table active-bit/generation gate. The dar-my8 exhaustive-
#     interleaving lesson applied to the multi-lane case: a lane freed by a dying/forking thread and
#     reused by a new (possibly tid-recycled) thread must never let a reader act on a half-initialized
#     lane (publish active LAST) or false-match a stale epoch (bump the generation on every reclaim).
#     Hermetic, pure model logic (no rpc.h needed). GREEN: zero violations across all enumerated
#     interleavings. RED arms break each invariant so the enumeration finds violations -> nonzero. ---
MLSRC="$HERE/ring_multilane_gate_test.c"
if [ -f "$MLSRC" ]; then
	echo "== multilane GREEN arm (active-bit/generation bookkeeping race-free) =="
	if ! "$CC" -std=c11 -o "$TMP/ml_green" "$MLSRC" 2>/dev/null; then
		echo "multilane GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/ml_green"; then
		echo "multilane GREEN arm FAILED -- a lane active-bit/generation invariant does not hold"; exit 1
	fi
	echo
	echo "== multilane RED arm A (-DRACE_PUBLISH_ACTIVE_BEFORE_INIT: active published before init, MUST fail) =="
	if ! "$CC" -std=c11 -DRACE_PUBLISH_ACTIVE_BEFORE_INIT -o "$TMP/ml_redA" "$MLSRC" 2>/dev/null; then
		echo "multilane RED arm A failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/ml_redA" >/dev/null 2>&1; then
		echo "multilane RED arm A PASSED but must FAIL -- gate not exercising the publish-active-LAST invariant"; exit 1
	fi
	echo "  multilane RED arm A correctly failed."
	echo
	echo "== multilane RED arm B (-DRACE_REUSE_WITHOUT_GEN_BUMP: reuse without gen bump, MUST fail) =="
	if ! "$CC" -std=c11 -DRACE_REUSE_WITHOUT_GEN_BUMP -o "$TMP/ml_redB" "$MLSRC" 2>/dev/null; then
		echo "multilane RED arm B failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/ml_redB" >/dev/null 2>&1; then
		echo "multilane RED arm B PASSED but must FAIL -- gate not exercising the generation-disambiguates-reuse invariant"; exit 1
	fi
	echo "  multilane RED arm B correctly failed."
	echo
	echo "ring_multilane gate: RED->GREEN OK"
	echo
fi

# --- D16 follow-up (dar-j7e7): postfork_reset RESOURCE-OWNERSHIP gate. The D16 gate pins the active-bit/
#     generation bookkeeping but NOT what __dserver_ring_postfork_reset() frees per lane. The shipped impl
#     gated teardown on `wake_fd >= 0`; because the lane table is `static` (zero-init, wake_fd==0), the fork
#     child ran close(0) once per untouched lane -> stdin destroyed -> "cannot duplicate fd 0". This gate
#     models the teardown decision and asserts it frees resources for EXACTLY the owning (active==1) lanes,
#     never a stdio fd, and restores the -1 sentinel. Hermetic, pure model (no rpc.h). ---
PFSRC="$HERE/ring_postfork_reset_gate_test.c"
if [ -f "$PFSRC" ]; then
	echo "== postfork-reset GREEN arm (ownership-gated teardown; no stdio fd closed; -1 sentinel) =="
	if ! "$CC" -std=c11 -o "$TMP/pf_green" "$PFSRC" 2>/dev/null; then
		echo "postfork-reset GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/pf_green"; then
		echo "postfork-reset GREEN arm FAILED -- teardown frees a non-owning lane or drops the sentinel"; exit 1
	fi
	echo
	echo "== postfork-reset RED arm A (-DRESET_GATE_ON_WAKEFD_GE0: the shipped close(0) bug, MUST fail) =="
	if ! "$CC" -std=c11 -DRESET_GATE_ON_WAKEFD_GE0 -o "$TMP/pf_redA" "$PFSRC" 2>/dev/null; then
		echo "postfork-reset RED arm A failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/pf_redA" >/dev/null 2>&1; then
		echo "postfork-reset RED arm A PASSED but must FAIL -- gate not catching close(0) on zero-init lanes"; exit 1
	fi
	echo "  postfork-reset RED arm A correctly failed."
	echo
	echo "== postfork-reset RED arm B (-DRESET_LEAVES_WAKEFD_ZERO: sentinel not restored, MUST fail) =="
	if ! "$CC" -std=c11 -DRESET_LEAVES_WAKEFD_ZERO -o "$TMP/pf_redB" "$PFSRC" 2>/dev/null; then
		echo "postfork-reset RED arm B failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/pf_redB" >/dev/null 2>&1; then
		echo "postfork-reset RED arm B PASSED but must FAIL -- gate not checking the -1 sentinel post-condition"; exit 1
	fi
	echo "  postfork-reset RED arm B correctly failed."
	echo
	echo "ring_postfork_reset gate: RED->GREEN OK"
	echo
fi

# --- Phase C/D (P8, dar-1il.3 / .3.1): DUPLEX-lane wake-model gate. The hard gate the duplex lane
#     is built on -- pure logic, hermetic. GREEN: the duplex wake model is lost-wake-free (guest park
#     watches the S2C-upcall stream; server park watches the upcall-reply stream) + correlation-safe.
#     RED arms break each mechanism so the binary exits nonzero. Needs rpc.h (callnum hash). ---
DXSRC="$HERE/ring_duplex_wake_gate_test.c"
if [ -f "$DXSRC" ] && [ -n "$GEN_RPC" ]; then
	echo "== duplex-wake GREEN arm (lost-wake-free + correlation-safe) =="
	if ! "$CC" -std=c11 -I"$GEN_RPC" -I"$INC" -o "$TMP/dx_green" "$DXSRC" 2>/dev/null; then
		echo "duplex-wake GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/dx_green"; then
		echo "duplex-wake GREEN arm FAILED -- the duplex wake model is not lost-wake-free"; exit 1
	fi
	echo
	echo "== duplex-wake RED arm A (-DDUPLEX_NO_GUEST_PUMP: pump drains one not all, MUST fail) =="
	if ! "$CC" -std=c11 -DDUPLEX_NO_GUEST_PUMP -I"$GEN_RPC" -I"$INC" -o "$TMP/dx_redA" "$DXSRC" 2>/dev/null; then
		echo "duplex-wake RED arm A failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/dx_redA" >/dev/null 2>&1; then
		echo "duplex-wake RED arm A PASSED but must FAIL -- gate not exercising the drain-all pump"; exit 1
	fi
	echo "  duplex-wake RED arm A correctly failed."
	echo
	echo "== duplex-wake RED arm B (-DDUPLEX_WAITERBIT_AFTER_RECHECK: bit set too late, MUST fail) =="
	if ! "$CC" -std=c11 -DDUPLEX_WAITERBIT_AFTER_RECHECK -I"$GEN_RPC" -I"$INC" -o "$TMP/dx_redB" "$DXSRC" 2>/dev/null; then
		echo "duplex-wake RED arm B failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/dx_redB" >/dev/null 2>&1; then
		echo "duplex-wake RED arm B PASSED but must FAIL -- gate not exercising the waiter-bit ordering"; exit 1
	fi
	echo "  duplex-wake RED arm B correctly failed."
	echo
	echo "== duplex-wake RED arm C (-DDUPLEX_WRONG_CORRELATION: accepts mismatched id, MUST fail) =="
	if ! "$CC" -std=c11 -DDUPLEX_WRONG_CORRELATION -I"$GEN_RPC" -I"$INC" -o "$TMP/dx_redC" "$DXSRC" 2>/dev/null; then
		echo "duplex-wake RED arm C failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/dx_redC" >/dev/null 2>&1; then
		echo "duplex-wake RED arm C PASSED but must FAIL -- gate not exercising correlation"; exit 1
	fi
	echo "  duplex-wake RED arm C correctly failed."
	echo
	echo "ring_duplex_wake gate: RED->GREEN OK"
	echo

	# --- Phase D4 (P8, dar-1il.3.2.1): the DEALLOCATE-via-duplex shape gate. Adds the two properties
	#     SPECIFIC to migrating a real DESTROY-CAPABLE op: (4) the destroy side effect is applied EXACTLY
	#     ONCE -- no partial-mutation-then-UDS double-effect; (5) a dropped/lost upcall reply FAILS CLOSED
	#     + BOUNDED -- never a fabricated success, never a wedge. GREEN before any real wiring. ---
	DDSRC="$HERE/ring_duplex_dealloc_gate_test.c"
	if [ -f "$DDSRC" ]; then
		echo "== duplex-dealloc GREEN arm (once-only + fail-closed-bounded) =="
		if ! "$CC" -std=c11 -I"$GEN_RPC" -I"$INC" -o "$TMP/dd_green" "$DDSRC" 2>/dev/null; then
			echo "duplex-dealloc GREEN arm failed to COMPILE"; exit 2
		fi
		if ! "$TMP/dd_green"; then
			echo "duplex-dealloc GREEN arm FAILED -- deallocate-via-duplex is not once-only/fail-closed"; exit 1
		fi
		echo
		echo "== duplex-dealloc RED arm A (-DDUPLEX_PARTIAL_THEN_UDS: mutate then UDS re-run, MUST fail) =="
		if ! "$CC" -std=c11 -DDUPLEX_PARTIAL_THEN_UDS -I"$GEN_RPC" -I"$INC" -o "$TMP/dd_redA" "$DDSRC" 2>/dev/null; then
			echo "duplex-dealloc RED arm A failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/dd_redA" >/dev/null 2>&1; then
			echo "duplex-dealloc RED arm A PASSED but must FAIL -- gate not exercising the double-effect guard"; exit 1
		fi
		echo "  duplex-dealloc RED arm A correctly failed."
		echo
		echo "== duplex-dealloc RED arm B (-DDUPLEX_FABRICATE_ON_DROP: fake success on dropped reply, MUST fail) =="
		if ! "$CC" -std=c11 -DDUPLEX_FABRICATE_ON_DROP -I"$GEN_RPC" -I"$INC" -o "$TMP/dd_redB" "$DDSRC" 2>/dev/null; then
			echo "duplex-dealloc RED arm B failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/dd_redB" >/dev/null 2>&1; then
			echo "duplex-dealloc RED arm B PASSED but must FAIL -- gate not exercising the fail-closed guard"; exit 1
		fi
		echo "  duplex-dealloc RED arm B correctly failed."
		echo
		echo "ring_duplex_dealloc gate: RED->GREEN OK"
		echo
	fi

	# --- Phase D5 (P8, dar-1il.3.2.2): the VM_DEALLOCATE-via-duplex shape gate. vm_deallocate is the op
	#     that ACTUALLY drives a caller munmap S2C in Darling (D4's mach_port_deallocate does not). Its
	#     munmap IS the op, so the properties are sharper: (A) the munmap S2C MUST fire -- a parent that
	#     completes-success without it lies about a still-mapped range; (B) the caller range is munmap'd
	#     EXACTLY ONCE -- no partial-then-UDS second munmap (a re-mapped address would be corrupted);
	#     (C) a dropped reply fails closed + bounded. Also pins the new CAP bit distinct (compile-time
	#     #error). GREEN before any vm_deallocate wiring. ---
	VMDSRC="$HERE/ring_duplex_vm_dealloc_gate_test.c"
	if [ -f "$VMDSRC" ]; then
		echo "== duplex-vm-dealloc GREEN arm (S2C-required + munmap-once + fail-closed-bounded) =="
		if ! "$CC" -std=c11 -I"$GEN_RPC" -I"$INC" -o "$TMP/vmd_green" "$VMDSRC" 2>/dev/null; then
			echo "duplex-vm-dealloc GREEN arm failed to COMPILE"; exit 2
		fi
		if ! "$TMP/vmd_green"; then
			echo "duplex-vm-dealloc GREEN arm FAILED -- vm_deallocate-via-duplex is not S2C-required/once-only"; exit 1
		fi
		echo
		echo "== duplex-vm-dealloc RED arm A (-DDUPLEX_VM_SKIP_S2C: complete without the munmap S2C, MUST fail) =="
		if ! "$CC" -std=c11 -DDUPLEX_VM_SKIP_S2C -I"$GEN_RPC" -I"$INC" -o "$TMP/vmd_redA" "$VMDSRC" 2>/dev/null; then
			echo "duplex-vm-dealloc RED arm A failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/vmd_redA" >/dev/null 2>&1; then
			echo "duplex-vm-dealloc RED arm A PASSED but must FAIL -- gate not requiring the munmap S2C"; exit 1
		fi
		echo "  duplex-vm-dealloc RED arm A correctly failed."
		echo
		echo "== duplex-vm-dealloc RED arm B (-DDUPLEX_VM_DOUBLE_MUNMAP: second munmap of reused range, MUST fail) =="
		if ! "$CC" -std=c11 -DDUPLEX_VM_DOUBLE_MUNMAP -I"$GEN_RPC" -I"$INC" -o "$TMP/vmd_redB" "$VMDSRC" 2>/dev/null; then
			echo "duplex-vm-dealloc RED arm B failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/vmd_redB" >/dev/null 2>&1; then
			echo "duplex-vm-dealloc RED arm B PASSED but must FAIL -- gate not exercising the double-munmap guard"; exit 1
		fi
		echo "  duplex-vm-dealloc RED arm B correctly failed."
		echo
		echo "== duplex-vm-dealloc RED arm C (-DDUPLEX_VM_FABRICATE_ON_DROP: fake success on dropped reply, MUST fail) =="
		if ! "$CC" -std=c11 -DDUPLEX_VM_FABRICATE_ON_DROP -I"$GEN_RPC" -I"$INC" -o "$TMP/vmd_redC" "$VMDSRC" 2>/dev/null; then
			echo "duplex-vm-dealloc RED arm C failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/vmd_redC" >/dev/null 2>&1; then
			echo "duplex-vm-dealloc RED arm C PASSED but must FAIL -- gate not exercising the fail-closed guard"; exit 1
		fi
		echo "  duplex-vm-dealloc RED arm C correctly failed."
		echo
		echo "ring_duplex_vm_dealloc gate: RED->GREEN OK"
		echo
	fi

	# --- P8 D1/D2 (dar-1il.3.1): LIVE synthetic duplex roundtrip over the REAL mailbox helpers.
	#     Two real threads (server publishes upcall + scope-waits; guest pumps + replies) on a real
	#     mailbox. Proves the transport (not just the model): roundtrip completes, caller-thread
	#     context, mismatch rejected. RED -DDUPLEX_RT_IGNORE_CORRELATION drops the correlation check
	#     -> the mismatch case is wrongly accepted -> nonzero. ---
	RTSRC="$HERE/ring_duplex_roundtrip_test.c"
	if [ -f "$RTSRC" ]; then
		echo "== duplex-roundtrip GREEN arm (live synthetic S2C roundtrip) =="
		if ! "$CC" -std=c11 -D_GNU_SOURCE -pthread -O2 -I"$GEN_RPC" -I"$INC" -o "$TMP/rt_green" "$RTSRC" 2>/dev/null; then
			echo "duplex-roundtrip GREEN arm failed to COMPILE"; exit 2
		fi
		if ! "$TMP/rt_green"; then
			echo "duplex-roundtrip GREEN arm FAILED -- the live duplex transport does not complete"; exit 1
		fi
		echo
		echo "== duplex-roundtrip RED arm (-DDUPLEX_RT_IGNORE_CORRELATION: accepts mismatched id, MUST fail) =="
		if ! "$CC" -std=c11 -D_GNU_SOURCE -pthread -O2 -DDUPLEX_RT_IGNORE_CORRELATION -I"$GEN_RPC" -I"$INC" -o "$TMP/rt_red" "$RTSRC" 2>/dev/null; then
			echo "duplex-roundtrip RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/rt_red" >/dev/null 2>&1; then
			echo "duplex-roundtrip RED arm PASSED but must FAIL -- gate not exercising correlation"; exit 1
		fi
		echo "  duplex-roundtrip RED arm correctly failed."
		echo
		echo "ring_duplex_roundtrip gate: RED->GREEN OK"
		echo
	fi
fi

# --- P8 D3 (dar-1il.3.1.1): the END-TO-END real-dylib duplex selftest is a SYSTEM test (it must boot
#     darling + use the deployed guest dylib + real server), so it cannot run inside this hermetic host
#     suite. It lives in run-duplex-real-selftest.sh and is part of the D3 acceptance run, NOT this
#     suite. Pointer only:
echo "note: the real-dylib duplex selftest (D3 end-to-end) is a SYSTEM gate -- run separately:"
echo "      bash tests/run-duplex-real-selftest.sh both   # RED (no-pump, no-wedge) then GREEN (PASS + counter)"
echo

# --- P8 D8 (dar-1il.3.2.x): mach_msg_overwrite SHAPE CENSUS classifier gate. Links the real
#     metrics.cpp + internal-include (the Metrics class lives there). Skip if no generated rpc.h /
#     internal-include is found (don't fail). ---
ININC=""
for cand in "$HERE/../internal-include" ; do
	[ -f "$cand/darlingserver/metrics.hpp" ] && { ININC="$cand"; break; }
done
MCSRC="$HERE/msg_overwrite_census_test.cpp"
MCIMPL="$HERE/../src/metrics.cpp"
if [ -z "$GEN_RPC" ] || [ -z "$ININC" ] || [ ! -f "$MCSRC" ] || [ ! -f "$MCIMPL" ]; then
	echo "== msg_overwrite census gate SKIPPED (generated rpc.h / internal-include / sources not found) =="
	echo
else
	echo "== msg_overwrite census GREEN arm (classify send/recv/blocking + send-only descriptor split) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/mc_green" "$MCSRC" "$MCIMPL" 2>/dev/null; then
		echo "census GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/mc_green"; then
		echo "census GREEN arm FAILED -- the shape classifier mis-buckets"; exit 1
	fi
	echo
	echo "== census RED arm 1 (-DRED_BREAK_BLOCKING: counts bounded receives as blocking, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_BLOCKING -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/mc_red1" "$MCSRC" "$MCIMPL" 2>/dev/null; then
		echo "census RED arm 1 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/mc_red1" >/dev/null 2>&1; then
		echo "census RED arm 1 PASSED but must FAIL -- blocking-receive rule not exercised"; exit 1
	fi
	echo "  census RED arm 1 correctly failed."
	echo
	echo "== census RED arm 2 (-DRED_BREAK_SENDONLY_PARTITION: double-counts OOL as simple, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_SENDONLY_PARTITION -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/mc_red2" "$MCSRC" "$MCIMPL" 2>/dev/null; then
		echo "census RED arm 2 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/mc_red2" >/dev/null 2>&1; then
		echo "census RED arm 2 PASSED but must FAIL -- send-only partition not exercised"; exit 1
	fi
	echo "  census RED arm 2 correctly failed."
	echo
	echo "msg_overwrite census gate: RED->GREEN OK"
	echo
fi

# --- P8 D9 (dar-1il.4): global RPC heatmap + lane-eligibility census gate. Links the real metrics.cpp
#     + internal-include (Metrics + the static lane-class table via rpc-supplement.h). Pins: default-OFF
#     no-op when disarmed, transport split, used_fiber/caller_s2c accumulation, and the lane-verdict
#     derivation. RED arms break the disarmed-noop invariant and the caller-S2C-forces-duplex rule. ---
HMSRC="$HERE/rpc_heatmap_gate_test.cpp"
HMIMPL="$HERE/../src/metrics.cpp"
if [ -z "$GEN_RPC" ] || [ -z "$ININC" ] || [ ! -f "$HMSRC" ] || [ ! -f "$HMIMPL" ]; then
	echo "== RPC heatmap gate SKIPPED (generated rpc.h / internal-include / sources not found) =="
	echo
else
	echo "== RPC heatmap GREEN arm (transport split + flags + lane verdicts) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/hm_green" "$HMSRC" "$HMIMPL" 2>/dev/null; then
		echo "heatmap GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/hm_green"; then
		echo "heatmap GREEN arm FAILED -- transport/flags/verdict logic wrong"; exit 1
	fi
	echo
	echo "== heatmap RED arm 1 (-DRED_BREAK_DISARMED_NOOP: disarmed recording must change counts, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_DISARMED_NOOP -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/hm_red1" "$HMSRC" "$HMIMPL" 2>/dev/null; then
		echo "heatmap RED arm 1 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/hm_red1" >/dev/null 2>&1; then
		echo "heatmap RED arm 1 PASSED but must FAIL -- default-OFF no-op invariant not exercised"; exit 1
	fi
	echo "  heatmap RED arm 1 correctly failed."
	echo
	echo "== heatmap RED arm 2 (-DRED_BREAK_VERDICT_S2C: caller-S2C op claimed tier2, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_VERDICT_S2C -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/hm_red2" "$HMSRC" "$HMIMPL" 2>/dev/null; then
		echo "heatmap RED arm 2 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/hm_red2" >/dev/null 2>&1; then
		echo "heatmap RED arm 2 PASSED but must FAIL -- caller-S2C->duplex verdict rule not exercised"; exit 1
	fi
	echo "  heatmap RED arm 2 correctly failed."
	echo
	echo "== heatmap RED arm 3 (-DRED_BREAK_PERCALL_LEAK: D14 per-call attribution must NOT leak S2C onto bystander, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_PERCALL_LEAK -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/hm_red3" "$HMSRC" "$HMIMPL" 2>/dev/null; then
		echo "heatmap RED arm 3 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/hm_red3" >/dev/null 2>&1; then
		echo "heatmap RED arm 3 PASSED but must FAIL -- per-call S2C attribution (no cross-op leak) not exercised"; exit 1
	fi
	echo "  heatmap RED arm 3 correctly failed."
	echo
	echo "RPC heatmap gate: RED->GREEN OK"
	echo
fi

# --- perf #18 D15a (dar-1il.10): ring-attach TIMELINE / reclaimability census gate. Links metrics.cpp.
#     Pins: default-OFF no-op when disarmed, pre/post-attach UDS split, eligible-pool accounting, and
#     attach attempt/reject tallies. RED arms break the disarmed-noop and the eligible-split rules. ---
ACSRC="$HERE/attach_census_gate_test.cpp"
ACIMPL="$HERE/../src/metrics.cpp"
if [ -z "$GEN_RPC" ] || [ -z "$ININC" ] || [ ! -f "$ACSRC" ] || [ ! -f "$ACIMPL" ]; then
	echo "== attach-census gate SKIPPED (generated rpc.h / internal-include / sources not found) =="
	echo
else
	echo "== attach-census GREEN arm (pre/post split + eligible pool + attach outcomes) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/ac_green" "$ACSRC" "$ACIMPL" 2>/dev/null; then
		echo "attach-census GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/ac_green"; then
		echo "attach-census GREEN arm FAILED -- pre/post split or eligible accounting wrong"; exit 1
	fi
	echo
	echo "== attach-census RED arm 1 (-DRED_BREAK_DISARMED_NOOP: disarmed recording must change counts, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_DISARMED_NOOP -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/ac_red1" "$ACSRC" "$ACIMPL" 2>/dev/null; then
		echo "attach-census RED arm 1 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/ac_red1" >/dev/null 2>&1; then
		echo "attach-census RED arm 1 PASSED but must FAIL -- default-OFF no-op invariant not exercised"; exit 1
	fi
	echo "  attach-census RED arm 1 correctly failed."
	echo
	echo "== attach-census RED arm 2 (-DRED_BREAK_ELIGIBLE_SPLIT: ineligible call counted as eligible, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_ELIGIBLE_SPLIT -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/ac_red2" "$ACSRC" "$ACIMPL" 2>/dev/null; then
		echo "attach-census RED arm 2 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/ac_red2" >/dev/null 2>&1; then
		echo "attach-census RED arm 2 PASSED but must FAIL -- eligible-pool accounting not exercised"; exit 1
	fi
	echo "  attach-census RED arm 2 correctly failed."
	echo
	echo "attach-census gate: RED->GREEN OK"
	echo
fi

# --- perf #18 D17 (dar-1il.12): POST-D16 residual-UDS classifier gate. Links metrics.cpp. Pins the
#     A-vs-B/D classification: an eligible UDS call on a thread with NO ring is reason A (first-before-
#     lane); on a thread that HAS a live ring it is the B/D signal (thread_has_ring + uds_despite_lane).
#     RED arms break the disarmed-noop and the A/B separation rules. ---
RCSRC="$HERE/residual_census_gate_test.cpp"
RCIMPL="$HERE/../src/metrics.cpp"
if [ -z "$GEN_RPC" ] || [ -z "$ININC" ] || [ ! -f "$RCSRC" ] || [ ! -f "$RCIMPL" ]; then
	echo "== residual-census gate SKIPPED (generated rpc.h / internal-include / sources not found) =="
	echo
else
	echo "== residual-census GREEN arm (reason buckets + A-vs-B/D separation) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/rc_green" "$RCSRC" "$RCIMPL" 2>/dev/null; then
		echo "residual-census GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/rc_green"; then
		echo "residual-census GREEN arm FAILED -- reason classification wrong"; exit 1
	fi
	echo
	echo "== residual-census RED arm 1 (-DRED_BREAK_DISARMED_NOOP: disarmed recording must change counts, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_DISARMED_NOOP -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/rc_red1" "$RCSRC" "$RCIMPL" 2>/dev/null; then
		echo "residual-census RED arm 1 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/rc_red1" >/dev/null 2>&1; then
		echo "residual-census RED arm 1 PASSED but must FAIL -- default-OFF no-op invariant not exercised"; exit 1
	fi
	echo "  residual-census RED arm 1 correctly failed."
	echo
	echo "== residual-census RED arm 2 (-DRED_BREAK_AB_SEPARATION: B/D call counted as reason A, MUST fail) =="
	if ! "$CXX" -std=c++17 -DDSERVER_RING_TRANSPORT -DRED_BREAK_AB_SEPARATION -I"$GEN_RPC" -I"$INC" -I"$ININC" -o "$TMP/rc_red2" "$RCSRC" "$RCIMPL" 2>/dev/null; then
		echo "residual-census RED arm 2 failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/rc_red2" >/dev/null 2>&1; then
		echo "residual-census RED arm 2 PASSED but must FAIL -- A-vs-B/D separation not exercised"; exit 1
	fi
	echo "  residual-census RED arm 2 correctly failed."
	echo
	echo "residual-census gate: RED->GREEN OK"
	echo
fi

echo "all perf#18 ring gates: OK"
