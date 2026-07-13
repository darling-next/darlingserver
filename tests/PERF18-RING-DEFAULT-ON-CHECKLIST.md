# perf#18 ring transport — Default-ON release checklist (dar-my8 / gist e0c1e862)

This list separates the **engineering gate** (done, below) from the **product decision** to flip
the ring transport on by default. The transport is gated `DSERVER_RING_TRANSPORT` (server) /
`DARLING_RING_TRANSPORT` (guest), default **OFF**. Everything below is a precondition for even
*discussing* default-ON seriously — it does not itself authorize the flip.

## Engineering gate (status as of perf#18 P7)

- [x] **P7 GREEN committed** — `tests/ring_wake_race_test.cpp` (exhaustive lost-wake interleaving
      enumeration over the real predicates, both request + response side; RED arms
      `-DRACE_NO_SERVER_RECHECK` / `-DRACE_WAITERBIT_AFTER_RECHECK` both fail). Wired into
      `run-ring-shm-validate.sh`. Commit darlingserver `8707a80`.
- [x] **NOTICE visible at the default log cutoff** — one-time `[NOTICE]` from
      `Server::_resolveRingSpinBudget()` (error-level, because the default cutoff is Error) on first
      ring attach. Fires once per server (guarded by `_ringSpinResolved`, called only from the
      single main-loop site), NOT once per client attach — verified safe + live in
      `$DPREFIX/private/var/log/dserver.log`. Commits `adf4e1a` + the bug-report line.
- [x] **`DSERVER_RING_TRANSPORT=OFF` build path byte-identical** — the OFF build compiles all ring
      code out (ON dylib +~13KB / 4 ring strings; OFF dylib 0). Prod (OFF) boots + builds clean.
- [x] **`DARLING_SERVER_FAST_OPS=0` disables only the opcode fast paths** — ring still used; ops
      route through the generic `callFromMessage` path (`ring_fast_hit=0`), identical results.
      Live A/B (P6.1 step 2): fast ON ~0.95µs vs OFF ~4.5µs, both `null=0`.
- [x] **`DARLING_SERVER_FAST_MACH_REPLY_PORT=0` disables only that op** — `task_self_trap` fast path
      stays on. Pinned by `ring_fastpath_gate_test.c` hatch assertions.
- [x] **ABI mismatch falls back to UDS** — `dserver_ring_shm_validate()` returns
      `dserver_ring_reject_abi` when `abi_version != DSERVER_RING_ABI_VERSION`; the server logs once
      and falls that thread back to UDS. A v1 peer can never half-speak v2.
- [x] **Bug-report instruction** — the startup NOTICE tells operators: on a suspected
      transport-related hang/crash, re-run with `DARLING_SERVER_FAST_OPS=0` (or a non-ring build)
      and report whether it reproduces.
- [x] **balanced-mode p50/p99 documented, not just the hot p50** — pinned/hot p50 ~740–830 ns;
      **balanced p50 ~741 ns, p99 ~8.6–28 µs** (tail = host scheduler preemption on a shared
      desktop, not the fast path). Always report both, never just the hot number.
- [x] **Known slow-path coverage caveat documented honestly** — `ring_s2c_full` / `ring_fast_fail`
      / `ring_fast_suspend` are **architecturally near-zero** in normal single-in-flight SPSC use
      (one reply in flight per ring thread): `s2c_full` ~never occurs naturally, `suspend` *must*
      stay 0 by the allowlist contract, `fail` only on a dispatch throw. Their correctness comes
      from the deterministic datapath retry-not-loss gate + the publish-fail→UDS-fallback (no
      double-mint) path + review — **not** from natural stress frequency over 1M ops.

## Product decision (NOT an engineering task)

Flipping the default to ON is a separate call (which mode default — balanced is the safe pick —
who gets it first, etc.). The gist's recommended rollout sequence:

1. dev / nightly builds first, with telemetry (the per-op counters + the startup NOTICE).
2. Keep the env kill-switches and the ABI auto-fallback as standing safety valves.
3. Only then consider a broader default-ON, mode = balanced.

The next *engineering* move is orthogonal: **P5 (dar-1il)** — migrate the next hot op
(`mach_port_mod_refs`) onto the ring with a stricter per-op bar. See the P5 agent brief
(`memory/dar-perf18-p5-agent-brief.md`).
