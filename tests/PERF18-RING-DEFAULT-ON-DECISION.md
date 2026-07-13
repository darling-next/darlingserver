# perf#18 — Default-ON Decision Packet (Phase B)

**Bead:** dar-dar6x4-perf-5dq.30 Phase B · **Status:** decision-ready · **Authored:** 2026-06-29 (Opus)
**Branches (LOCAL, not pushed):** darlingserver `perf/shmem-ring-abi-validator` @17fc4d2 · xnu `perf/shmem-ring-guest` @412e15c

> **This packet does NOT flip any default.** It assembles everything needed for product to make ONE
> conscious yes/no: *"Do we default `DSERVER_RING_TRANSPORT` / `DARLING_RING_TRANSPORT` ON for
> dev/nightly?"* The flip itself is a separate, explicitly-instructed step (see §6 — the one-line
> change + redeploy). Duplex lane and further op migration are **out of scope** and unaffected.

---

## 0. The decision, in one line

Turn the simple fast ring ON by default **for dev/nightly builds only**, mode = **balanced**,
keeping every safety valve in §4 as a standing fallback. Release/default-for-all is a *later,
separate* decision (§5 Stage 3), not this one.

**Scope of "the ring" being decided:** the FROZEN simple-ring set only —
`{task_self_trap, mach_reply_port, mach_port_allocate, mach_port_insert_right}` (Lane 1; Tier-2
no-fiber for the two port traps). Nothing destroy-capable, no duplex, no new ops. Phase A
(@17fc4d2) made that set a compile-time invariant: adding a destroy-capable op now fails to build.

---

## 1. What "ON" actually changes (blast radius)

| Aspect | OFF (today's prod) | ON (proposed dev/nightly) |
|---|---|---|
| Transport for the 4 frozen ops | UDS sendmsg/recvmsg round-trip | shmem SPSC ring + futex wake |
| Everything else (checkin, fd-passing, S2C, all other RPCs) | UDS | **UDS — unchanged** |
| Per-thread cost | n/a | one memfd + 2 small rings, lazily attached by the first eligible call |
| Wire protocol | callnum ≤ 80 | callnum 81 (`ring_attach`) present unconditionally even when OFF (additive, harmless) |
| Result semantics | — | byte-identical (proven, §2) |

The ring is **opt-in per process at runtime**: a process only attaches a ring when it first issues
one of the 4 ops; everything else keeps using UDS in the same process.

---

## 2. Evidence summary (why this is safe enough to discuss)

**Performance (the reason to do it at all).** Measured in-situ, `null=0` across all runs:

| Op | UDS p50 | ring p50 | speedup |
|---|---|---|---|
| mach_reply_port (Tier-2 no-fiber) | ~29.7 µs | **~0.74–0.95 µs** | ~31× |
| mach_port_allocate (Tier-1 fiber) | ~14.4 µs | ~2.8 µs | ~5.1× |
| mach_port_insert_right (Tier-1) | ~13.8 µs | ~2.1 µs | ~6.4× |
| task_self_trap | ~UDS class | ~ring class | — |

- **Always report p50 AND p99, never the hot number alone.** Balanced p50 ~741 ns;
  **balanced p99 ~8.6–28 µs** — the tail is host-scheduler preemption on a shared desktop, not the
  fast path. On a quiet machine the tail collapses. Product should treat p99 as workload-dependent.
- Hot path is **~0 syscalls/call** both sides (~95% spin-serviced, ~97% no FUTEX_WAKE, ~3% doorbell).

**Correctness / safety (the reason it won't wedge):**
- **Trust boundary**: guest is untrusted; server copies-in + bounds-checks every field, dereferences
  no guest pointer. Adversarial RED→GREEN validator/attach/datapath gates (garbage callnum/length/
  arena OOB/tail-jump/magic-corrupt → reject + UDS-fallback, no crash/OOB).
- **Lost-wake**: P7 exhaustive interleaving enumeration over the real wake predicates, 0 lost; RED
  arms break each recheck and fail as required (`ring_wake_race_test.cpp`).
- **No silent drop**: guest "may publish" set == server "will service" set (shared X-macro +
  `ring_drift_gate_test`); cross-binary skew rejected at attach via the FNV opcode-set hash → all-UDS.
- **Membership canon is machine-checked** (Phase A @17fc4d2): destroy-capable / caller-S2C ops
  **cannot compile** into the simple-ring set. deallocate + mod_refs are UDS-only by construction.
- **System stress** (booted, ~1M fast ops): many-hot / idle+hot / client-death-mid-request all
  `null=0`, server not wedged post-death; teardown-with-active-ring = guest clean ECONNREFUSED (no
  hang); idle CPU 0% in low-power/balanced/latency; counters `fail=0 s2c_full=0 suspend=0
  clients_blocked=0`. Fork-storm 3/3.
- **OFF build unaffected**: compiles all ring code out; prod (OFF) boots + builds clean.

**Honest caveat (must be in the decision):** the slow-path counters `ring_s2c_full` /
`ring_fast_fail` / `ring_fast_suspend` are **architecturally near-zero** in normal single-in-flight
SPSC use — their correctness comes from the deterministic datapath retry-not-loss gate + the
publish-fail→UDS-fallback path + review, **NOT** from natural stress frequency. We are not claiming
1M-op stress "covered" them; we are claiming they cannot fire by construction and the deterministic
gates prove the paths.

Full engineering gate (all `[x]`): `PERF18-RING-DEFAULT-ON-CHECKLIST.md` (sibling file).

---

## 3. Open risks the product decision must weigh

1. **p99 tail on loaded/shared hosts** (~8–28 µs). Lower-bounded by host scheduling, not the ring.
   *Mitigation:* dev/nightly first; telemetry watches it; balanced mode (not latency) is the default.
2. **No runtime whole-ring kill-switch on the guest** (see §4, knob gap). Today, fully disabling the
   ring without the per-op fast-path hatch requires a **rebuild+redeploy** of the OFF binaries. The
   env hatches disable the *fast paths* (ring still used via generic fiber), and ABI-fallback is
   automatic — but there is no `DARLING_RING=0`-style "don't attach at all" guest env gate.
   *This is the single most material gap for a default-ON posture.* Product may (a) accept it
   because the compile-time OFF binaries are one redeploy away and the fast-path hatch + ABI
   fallback cover the likely failure modes, or (b) require a small follow-up bead to add a guest
   runtime gate BEFORE flipping. **This packet does not implement it** (would be code, not a packet).
3. **Slow-path counters can't be stress-validated** (see §2 caveat) — accepted-by-construction, not
   accepted-by-frequency. A reviewer who rejects that reasoning should block the flip.
4. **Wire callnum 81 is present even when OFF.** Additive/harmless, but a packaging note: an OFF
   build is *not* byte-identical to a pre-perf#18 build (it gains the unused `ring_attach` callnum).

---

## 4. Rollback knobs (the standing safety valves)

Ordered from "no human action" to "rebuild required". An operator hitting a suspected
transport-related hang/crash escalates down this list.

| # | Knob | Scope | Action | Takes effect | Requires |
|---|---|---|---|---|---|
| 0 | **ABI / opcode-hash auto-fallback** | per-thread | none (automatic) | at attach | nothing — a version/ABI/opcode-set skew rejects the ring → that thread uses UDS for everything |
| 1 | `DARLING_SERVER_FAST_MACH_REPLY_PORT=0` | server, runtime | set env, restart server | next boot | server restart |
| 2 | `DARLING_SERVER_FAST_OPS=0` | server, runtime | set env, restart server | next boot | server restart — disables ALL Tier-2 fast paths; **ring still used** via generic fiber (`ring_fast_hit=0`), results identical |
| 3 | **Rebuild OFF** (`DSERVER_RING_TRANSPORT=OFF` / `DARLING_RING_TRANSPORT=OFF`) | whole transport | rebuild + redeploy the 8-file set | after redeploy | full rebuild+deploy (the true off switch) |

**Knob gap (see §3.2):** there is no runtime equivalent of knob #3 on the guest — disabling the
*entire* ring (not just the fast ops) needs a rebuild. Knobs #1/#2 keep the ring transport but route
ops through the proven generic path; knob #0 is automatic. For dev/nightly this is acceptable
(redeploy is cheap); for broader rollout product may require closing the gap first.

**Telemetry to watch during rollout** (per-op counters via
`tools/darling-stat $DPREFIX`, polled while the guest runs):
`ring_fast_hit` / `ring_fast_fallback` / `ring_fast_fail` / `ring_s2c_full` / `ring_fast_suspend` /
`clients_blocked_in_rpc`. Expected healthy steady state: `fail=0 s2c_full=0 suspend=0 blocked=0`,
`fallback` low. Any nonzero `suspend` is a contract violation → escalate to knob #2/#3. The one-time
startup `[NOTICE]` (error-level, visible at the default cutoff) announces ring ACTIVE + mode +
fast_ops state + the full disable matrix, and carries the bug-report instruction (re-run with
`DARLING_SERVER_FAST_OPS=0` or a non-ring build and report whether it reproduces).

---

## 5. Staged rollout plan

Each stage has an **entry gate** (preconditions) and an **exit gate** (what must hold to advance).
**This decision authorizes Stage 1 only.** Stages 2–3 are separate go/no-go calls.

### Stage 1 — dev / nightly default-ON, mode = balanced  ← *the decision in this packet*
- **Entry gate:** engineering checklist all `[x]`; this packet reviewed; product accepts the §3 risks
  (notably the §3.2 runtime-kill-switch gap) for dev/nightly.
- **Action (separate, instructed step — §6):** flip the two cmake defaults to ON in the dev/nightly
  build config; redeploy. Keep all §4 knobs.
- **Bake time:** ≥ 1–2 weeks of normal dev/nightly use.
- **Exit gate → Stage 2:** across the bake window — no transport-attributed wedge/crash; counters
  steady (`suspend=0 fail=0 s2c_full=0`); `fallback` rate explained; p99 within expectations on dev
  hardware; zero rollback-to-knob-#3 events. If any fails → roll back (knob #3) and file a bug.

### Stage 2 — broaden within dev (all dev machines / CI guest-smoke), still balanced
- **Entry gate:** Stage 1 exit gate met; (recommended) the §3.2 guest runtime kill-switch landed if
  product required it.
- **Action:** extend the default-ON dev/nightly config to all dev surfaces incl. CI guest-smoke.
- **Exit gate → Stage 3:** sustained clean telemetry at higher concurrency; CI guest-smoke green with
  ring ON; no new fork-storm regressions (dar-l8k / dar-6x4 / dar-6x4.1 suites green).

### Stage 3 — release default-ON (broad)  ← *a future, separate product decision; NOT this packet*
- **Entry gate:** Stage 2 exit gate met; runtime kill-switch present; p50/p99 documented on release-
  representative hardware; sign-off that the §2 "near-zero counters" reasoning is acceptable for
  release.
- **Action:** flip the release default. Mode = balanced unless a measured case justifies latency.
- **Rollback:** same §4 ladder; knob #3 ships as a documented one-liner.

---

## 6. Exact flip mechanics (for whoever is instructed to do Stage 1)

> Do **not** run this as part of Phase B. It is the Stage-1 action, gated on an explicit product yes.

The default lives in two cmake `option(... OFF)` lines (verified 2026-06-29):
- server: `DSERVER_RING_TRANSPORT` — `src/external/darlingserver/CMakeLists.txt:65`
- guest:  `DARLING_RING_TRANSPORT` — `src/external/xnu/darling/src/libsystem_kernel/emulation/CMakeLists.txt:401`
  (a single option drives both targets: `emulation` → dylib, `emulation_dyld` → dyld, lines 402–404)

Stage-1 flip = change those two option defaults to `ON` **for the dev/nightly build configuration
only**, then rebuild + redeploy the 8-file set (server×1, mldr×2, dylib×3, dyld×2). The OFF binaries
remain the instant rollback (knob #3). No source logic changes — only the default of the gate.
Verify post-flip: the startup `[NOTICE]` appears once; counters clean; fork-storm 3/3.

---

## 7. Explicit non-goals of Phase B (do NOT do these here)

- Do **not** flip any default (that is the instructed Stage-1 action, not Phase B).
- Do **not** implement the guest runtime kill-switch (surface it as a decision input only).
- Do **not** migrate any new op onto the ring.
- Do **not** touch the duplex lane (dar-1il.3 / .3.1 / .3.2).
- Do **not** add deallocate/mod_refs back (Phase A makes it a compile error anyway).

---

## 8. Decision matrix — fill this in to close Phase B

| Question | Product answer |
|---|---|
| Default-ON for **dev/nightly**, mode balanced? (Stage 1) | ☐ yes ☐ no |
| Accept the §3.2 runtime-kill-switch gap for dev/nightly, or require it first? | ☐ accept ☐ require-first |
| Accept the §2 "near-zero counters proven by construction, not stress" reasoning? | ☐ accept ☐ reject |
| If yes: who executes the Stage-1 flip (§6) and when? | __________ |

A `yes` to row 1 (with rows 2–3 resolved) closes Phase B and authorizes the Stage-1 flip step.
A `no` closes Phase B with the ring staying OFF by default; the engineering work stands and the
decision can be revisited without redoing this packet.
