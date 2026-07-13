# perf#18 D13 (dar-1il.8) — Lane-1.5 string/path "arena" RECON + migration

**STEP 0 verdict: NO ARENA NEEDED. The three "char*/path" ops are NOT variable-payload at the RPC
layer — the path bytes move out-of-band via `process_vm_readv/writev` (/proc/pid/mem). Their RPC
bodies are tiny and FIXED. They are pure D11-recipe INLINE migrations.**

Migrated (this bead): `mldr_path`, `vchroot_path` (warm post-attach sites) as Tier-1 generic-fiber
Lane-1 ops. Deferred: `set_executable_path` (pre-attach + NO_REPLY, unchanged from D12). No arena
built, no ABI bump (opcode-hash handles the C2S-set change, exactly like D10/D11).

## STEP 0 — does the payload need an arena? (the decision that gated everything)
The brief assumed these ops carry a variable-length path in the RPC payload needing a shared-memory
arena. That assumption is FALSE for Darling. Evidence from the code:

1. **Negotiated slot geometry is 128B, not 4096B.** `GR_SLOT_SIZE = 128`, `GR_SLOT_COUNT = 8`
   (dserver-ring.c:47-48). The 4096 in the ABI (`DSERVER_RING_MAX_SLOT_SIZE`) is only the *maximum*.
   So `inlineCap = 128 - sizeof(dserver_ring_slot_t)(24) = 104` bytes. A PATH_MAX(1024) string would
   NOT fit inline — IF the path travelled in the payload. It does not (point 3).

2. **The RPC bodies are fixed and tiny** (generated rpc.h):
   - `vchroot_path`: request `{u64 buffer, u64 buffer_size}` = **16B**; reply `{u64 length}` = **8B**
     (reply hdr 4 + 8 = 12B ≤ 104).
   - `mldr_path`: request **16B**; reply **8B**.
   - `set_executable_path`: request **16B**; reply **header-only** (NO_REPLY).
   All ≤ inlineCap with room to spare — identical shape class to the D11 batch.

3. **The path bytes never enter the RPC payload — they go via /proc/pid/mem.** The `char* buffer` in
   each body is a GUEST VIRTUAL ADDRESS, and the server reads/writes it directly:
   - `VchrootPath::processCall` (call.cpp:585) → `process->writeMemory(_body.buffer, path, len)`.
   - `MldrPath::processCall` (call.cpp:765) → `process->writeMemory(_body.buffer, mldrPath, len)`.
   - `SetExecutablePath::processCall` (call.cpp:1197) → `process->readMemory(_body.buffer, ...)`.
   `Process::_readOrWriteMemory` (process.cpp:203) uses **`process_vm_readv` / `process_vm_writev`**
   — a direct cross-process copy by the server into/out of the parked guest's address space. This is
   **transport-agnostic**: it works byte-identically whether the RPC arrived over UDS or the ring,
   and the guest does not participate (it is parked the whole time).

**=> No arena. No variable inline body. No ABI region. These are fixed-shape closed ops; the D11
declarative recipe applies verbatim.** "Two inline migrations, no arena" is exactly the cheapest,
safest STEP-0 outcome the brief flagged as valid — and it is the correct one here.

## Canon check (all 5 membership rules) — PASS for the migrated ops
- **Rule 1 (no caller-S2C):** `process_vm_readv/writev` is a server-side syscall on the guest's
  /proc/mem; it does NOT drive an mmap/munmap/mprotect/msync upcall to the caller. The guest thread
  stays parked on the ring reply; no S2C is needed. (Contrast deallocate/mod_refs which DO drive a
  munmap S2C.)
- **Rule 2 (no destroy):** vchroot_path/mldr_path are pure reads of server-side config
  (`process->vchrootPath()`, `Config::defaultMldrPath`) written into the guest. No teardown.
- **Rule 3 (decidable pre-mutation):** fixed shape; eligibility is the callnum alone.
- **Rule 4 (ABI negotiation):** added to `DSERVER_RING_C2S_OPCODES` → folded into the opcode-hash; a
  skewed guest is rejected at attach (all-UDS), no silent drop.
- **Rule 5 (side-effect A/B):** acceptance asserts the path actually delivered to the guest matches
  UDS (the written bytes + returned length), not just the return code.
Tagged `DSERVER_RING_CLASS_SIMPLE_C2S` (Tier-1 generic fiber, NOT NoFiberFast — they touch
process/config state via the Call path, not pure mint). The compile-time canon static_assert holds.

## Pre-attach split (the D11/D12 lesson)
- **mldr_path** — sole caller `sys_execve` (execve.c:49). A running process calling execve = WARM,
  post-attach. MIGRATE.
- **vchroot_path** — TWO callers:
  - `init_vchroot_path` (vchroot_userspace.c:1672, in the libsystem_kernel dylib) — runs once per
    process to cache the prefix path. WARM, post-attach. MIGRATE (this site rides the ring).
  - mldr.c:940 — in the **mldr** binary (`src/startup/mldr/`), a SEPARATE executable that links the
    generated `dserver_rpc_*` directly and has NO ring at all. Stays UDS BY CONSTRUCTION (untouched).
- **set_executable_path** — sole caller mldr.c:278 (the mldr binary, pre-attach) AND it is NO_REPLY.
  DEFER: pre-attach wins nothing (ring not attached there), and converting NO_REPLY→ack-only is a
  semantic change (D12 verdict). Not migrated.

## Heatmap split to report at acceptance (per op): total / pre_attach_UDS / ring / shape_fallback
(There is no oversize_fallback bucket — no variable payload exists. shape_fallback covers a
wrong-shaped reply → UDS, which should be ~0.)

## Expected reclaimable
mldr_path (~82/workload) + vchroot_path warm site (subset of ~248; the rest is the mldr-binary
pre-attach site that stays UDS). ~8µs p50 UDS → ~ring p50 (~3-4µs like the other Tier-1 ops).
Modest but real, and essentially free given the D11 recipe (no new server code, no arena).

## What was NOT built (per non-goals + STEP 0)
- NO arena / general allocator (not needed — paths go via /proc/mem).
- NO ABI bump (opcode-hash handles the set change).
- NO psynch, NO duplex changes, NO NoFiberFast for these.
- set_executable_path NOT migrated (pre-attach + NO_REPLY).

## LIVE A/B RESULTS (homebrew-test prefix, D13 dylib+server, heatmap armed)
Workload: shell boot + exec batches + 6×10 concurrent `ls` + fork-storm 3/3. Final: 12683 RPCs,
308 forks, 0 clients_blocked, ring health pristine (fast_hit 1188; fast_fail/fallback/s2c_full/
suspend all 0).
- **mldr_path**: total 285, **ring 284 / uds 1**, `used_fiber == total` (Tier-1 confirmed), verdict
  (auto) `duplex-only` — see the false-latch note below; the single UDS is the pre-attach/early call.
- **vchroot_path**: total 857, **ring 285 / uds 572**, `used_fiber == total`, verdict
  `already-on-ring`. The 572 UDS are the mldr-binary pre-attach site (mldr.c:940, no ring) — exactly
  the expected pre-attach residual; the warm dylib site (init_vchroot_path) rides the ring.
- **Side-effect A/B (canon rule 5):** over the ring, `uname=Darwin`, exec of multiple binaries works
  (`exec-ok`), and vchroot path translation is correct (PWD resolved to `/Volumes/SystemRoot/...`) —
  a wrong vchroot_path reply (delivered via writeMemory) would corrupt ALL path translation. The UDS
  baseline arm (prod-bak binaries) produced the identical working shell. Byte-identical behavior.
- **Lane-1 fixed ops UNCHANGED:** mach_reply_port/task_self_trap still Tier-2 no-fiber
  (`class_nofiber=1`) on the ring; uidgid + the D11 batch unchanged (`class_nofiber=0`, Tier-1).
- Fork-storm 3/3 GREEN (30 children each, all rc=0); no wedge; prod binaries restored.

## CANON RULE-1 PROOF + a heatmap false-latch finding (IMPORTANT)
The per-op heatmap reported `mldr_path caller_s2c=3`, and its auto-verdict flipped to `duplex-only`.
This is a **false attribution**, NOT a real caller-S2C — proven by the authoritative transport-aware
counter: across 284 ring-routed mldr_path calls, **`s2c_munmap_ring_parent` stayed 0** (all 5 boot
munmap S2Cs had a UDS parent — the server-internal vm_map_copyout exec-teardown, D6 attribution). The
tell: `caller_s2c` FROZE at 3 while ring calls grew 25→175→284 — a real per-op S2C would track op
volume. ROOT CAUSE: `_heatmapCallDidS2c` (thread.cpp:1443) is a sticky per-thread bool latched in
`_s2cPerform`; on an exec'ing thread the exec-teardown munmap S2C latches adjacent to the mldr_path
call in the same recording window. mldr_path's own handler (`writeMemory` = `process_vm_writev`, a
server-side syscall on the guest /proc/mem) drives NO S2C. **Canon rule 1 holds** — measured directly
via `s2c_munmap_ring_parent=0`, not inferred. LESSON for future migrations: trust
`s2c_munmap_ring_parent` (transport-correct) over the per-op `caller_s2c` sticky latch, which
over-attributes on exec/teardown threads. (A future heatmap refinement could scope the latch to the
active call, but that is out of scope for D13.)

## Future note
If a FUTURE op genuinely carried a variable-length payload IN the RPC body (not via /proc/mem) and
exceeded the 104B inline cap, THEN the copy-sealed arena design in the D13 brief would apply. No such
warm op exists in the measured workload; Darling's path ops sidestep it entirely via /proc/mem. The
arena remains a correctly-scoped FUTURE bead, not needed for the path tail.
