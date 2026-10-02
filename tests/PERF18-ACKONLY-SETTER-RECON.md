# perf#18 D12 (dar-1il.7) — closed-setter / ACK-ONLY ring migration RECON

**Verdict: STOP. No clean WARM ack-only batch exists. Migrate nothing in D12 v1.**
This is the brief's explicitly-sanctioned valid-completion path ("if recon finds nothing
clean … STOP with the recon doc + the deferred lists; that is a valid, complete D12").

Recon-only. NO code migrated. NO arena built. psynch/duplex untouched. ABI stays v5.

## Method
- Setter-shape inventory from `scripts/generate-rpc-wrappers.py` (the RPC spec): every op whose
  reply is empty/header-only (`[]`) or a small fixed scalar reply.
- Warm-share from `tests/PERF18-RPC-HEATMAP-snapshot.json` (D9 heatmap; workload =
  shellspawn + 90-fork storm + ps, 5057 RPC, representative of build/run, NOT debug/ptrace).
- Canon safety from each op's `processCall()` in `src/call.cpp` (no caller-S2C upcall, no
  destroy, decidable before mutation, generic-fiber safe — the 5-rule membership canon).
- Caller wait/error semantics from each `dserver_rpc_<op>` call site in the guest.

## The two axes that decide it
1. **Warm vs pre-attach/cold.** D11 lesson: an op that is 100% pre-attach (dyld/mldr bootstrap,
   before the ring is even attached) or cold (debug-only) reclaims ~0 even if shape-perfect. Only
   warm-share above the pre-attach residual is worth the ABI-hash churn + correctness surface.
2. **Ack-only-safe vs genuinely-NO_REPLY.** An op whose UDS path ALREADY waits for a status is a
   pure D11-recipe migration (empty reply via `gr_full_trap(...,reply_out=NULL,replylen=0,&code)`).
   A genuinely NO_REPLY op (the guest does NOT wait today) would REGRESS to waiting if made
   ack-only — a real semantic change, not a free migration.

## Per-candidate classification

| Op (callnum) | Req→Reply | Caller waits today? | Warm? (heatmap) | Canon-safe? | Bucket |
|---|---|---|---|---|---|
| `set_dyld_info` (206) | `{u64,u64}→[]` **NO_REPLY** | **NO** (fire-and-forget) | count 83 but **pre-attach** (mldr, before ring attach) | yes | **DEFER** — pre-attach + NO_REPLY (true-async-only) |
| `set_executable_path` (242) | `char*,u64→[]` **NO_REPLY** | NO | count 83 but **pre-attach** (mldr) | needs arena | **DEFER** — arena + NO_REPLY + pre-attach (two+ blockers) |
| `set_tracer` (213) | `{i32,i32}→[]` | **YES** (err observable, abort-on-internal) | **cold** (ptrace PT_PTRACE_ME / PT_ATTACHEXC only) | yes | ack-only-fixed but COLD → no win |
| `ptrace_sigexc` (224) | `{i32,bool}→[]` | YES | **cold** (ptrace attach/detach/sigexc) | yes (try_resume targets the TRACEE, not caller; no caller-S2C) | ack-only-fixed but COLD |
| `ptrace_thupdate` (229) | `{i32,i32}→[]` | YES | **cold** (ptrace PT_THUPDATE) | yes | ack-only-fixed but COLD |
| `stop_after_exec` (211) | `[]→[]` | YES | **cold** (only POSIX_SPAWN_START_SUSPENDED) | yes | ack-only-fixed but COLD |
| `interrupt_exit` (200) | `[]→[]` | YES | **warm (63)** | **NO** | **DISQUALIFIED** (see below) |

### Why `interrupt_exit` is disqualified (the one warm clean-shape op)
`Call::InterruptExit::processCall()` (call.cpp:956) is not a setter — it pops the per-thread
`_interrupts` stack and, if present, **re-sends a stashed `savedReply` over the UDS socket**
(`Server::sharedInstance().sendMessage(...)`). It is one half of the signal-interrupt protocol
(`interrupt_enter` carries `PUSH_UNKNOWN_REPLIES`; the pair preserves per-thread UDS reply matching
during signal delivery — generate-rpc-wrappers.py:90-114). Moving it to the ring would split the
reply-matching state across two transports and desync deferred replies. Stays UDS. Not a candidate.

### Why the genuinely-warm NO_REPLY setters can't be ack-only-migrated as a win
`set_dyld_info` + `set_executable_path` are both called once per exec in **mldr** (the Mach-O
loader / `src/startup/mldr/mldr.c:272,278`), which runs **before** the loaded image's
libsystem_kernel ring-attach handshake. Same class as D11's `started_suspended`/`get_tracer`
(100% pre-attach → never moved). Even ignoring NO_REPLY, the ring isn't attached at that call, so
the request would always take the UDS fallback. Migrating gains nothing and, because they're
NO_REPLY, ack-only conversion would only ADD a wait. `set_executable_path` additionally carries a
`char*` path → arena-class.

## Buckets
- **ack-only-fixed (would-MIGRATE under the D11 recipe, but COLD → deferred for lack of warm share):**
  `set_tracer`, `ptrace_sigexc`, `ptrace_thupdate`, `stop_after_exec`. All already synchronous,
  fixed-shape, canon-safe, header-only reply. If a future workload makes ptrace/start-suspended
  hot, these are drop-in D11-recipe migrations (classify + `gr_full_trap` empty-reply wrapper +
  shape guard + UDS fallback; no server handler). Not worth migrating now.
- **needs-arena (DEFER, do NOT build the arena):** `set_executable_path` (char* path), and the
  D9-surfaced warm tail `vchroot_path` (248), `mldr_path` (82) — all `char*`. These are the real
  warm reclaimable wins but require a variable-length arena lane (separate, larger bead).
- **true-async-only (DEFER, separate publish-only ring-mode bead):** `set_dyld_info` (and
  `set_executable_path` once arena exists) — genuinely NO_REPLY today; pre-attach now, so even a
  publish-only ring mode wins nothing until/unless they're issued post-attach. Record the design
  sketch only; do NOT build a publish-only mode for a pre-attach op.

## Expected reclaimable count
≈ **0** in any normal (build/run) workload. The clean ack-only-fixed ops are cold; the warm
not-yet-migrated ops are either pre-attach (set_dyld_info, set_executable_path, mldr_path),
arena-class (vchroot_path, set_executable_path, mldr_path), or disqualified
(interrupt_enter/exit, checkin/checkout fd-passing, fork_wait_for_child blocking). The remaining
warm fixed-shape ops were ALREADY taken by D10 (self-trap family) and D11 (uidgid,
set_thread_handles, started_suspended, get_tracer, task_is_64_bit). **The closed fixed-shape
ack-only setter seam is exhausted for warm traffic.**

## Recommendation for the NEXT perf lever (not a D12 deliverable)
The warm reclaimable RPC left on UDS is **arena-class** (`vchroot_path` 248 + `mldr_path` 82 +
`set_executable_path` 83 — all `char*`/path, ~8us UDS p50). The next throughput bead should be the
**variable-length arena lane** (Lane-1.5), which unlocks this whole tail at once, rather than more
fixed-shape migrations (that seam is now empty). Arena was explicitly OUT OF SCOPE for D12;
this recon is the evidence that it is the right next investment.

## Gates / regression
No code changed → existing perf#18 gates (lane-class, allowlist, drift, canon static_assert) remain
GREEN unchanged; ring membership set unchanged (D10+D11 = 11 ops); ABI v5 unchanged. No A/B, no
fork-storm needed (no datapath change). The heatmap snapshot used is the D9 artifact (committed).
