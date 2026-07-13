# perf#18 D7 — mach_msg_overwrite RECON / DESIGN (the future OOL carrier)

**Bead:** dar-1il.3.2.3 · **Status:** RECON + D8 CENSUS COMPLETE — **verdict: STOP, do not migrate** (§9) · **No production change.**
**HEADs:** darlingserver `perf/shmem-ring-abi-validator` @24f222b · xnu `perf/shmem-ring-guest` @b412c28.
**Reads:** PERF18-DUPLEX-LANE-DESIGN.md (the duplex/Lane-2 model), the C2S membership canon
(rpc-supplement.h `DSERVER_RING_C2S_OPCODES`), the D6 caller-S2C sideband brief.

> **TL;DR recommendation: DO NOT migrate mach_msg_overwrite to any ring lane now.** It is the single
> hottest RPC (≈19% of all calls), so the perf prize is real — but it is *also* the single most
> dangerous op to migrate: it has a **blocking receive** (disqualifies the simple Lane-1 req→reply
> model outright), it drives **mmap, munmap, mprotect AND msync caller-S2C upcalls** (D6's sideband
> covers only munmap today), it carries **variable-length complex descriptors** (no fixed inline body),
> and it moves **port rights / OOL memory across a process boundary** (name-space + refcount + cross-task
> mmap effects). The correct next step is a **scoped Lane-2 (duplex) design for the SEND-only,
> simple-message subset**, gated behind a full mmap-S2C sideband extension — not a Lane-1 migration and
> not a blind whole-op migration. Concrete go/no-go and the safe subset are in §5–§6.

---

## 1. Why this op is the recon target (the D6 finding)

D6 (caller-S2C sideband, committed darlingserver @24f222b / xnu @b412c28, default-OFF) reframed the duplex
lane as a **general caller-S2C sideband for ring-parked parents** and proved it live (`ring_duplex_s2c>0`
end-to-end via a synthetic warm vm_deallocate proof). Its Step-1 attribution measured **every** boot-time
caller-S2C munmap upcall and found they all land under a **UDS parent** — and that parent is
**`mach_msg_overwrite`** (dserver_callnum 38), the OOL-message teardown path. Today that is harmless: the
caller is parked in `recvmsg` (UDS), so it services the S2C inline — no ring deadlock. It becomes a real
ring deadlock **only if msg_overwrite is ever moved to a ring lane**, and the D6 sideband is precisely the
prerequisite that would make such a migration caller-S2C-safe.

So msg_overwrite is the *one* production op that carries the caller-S2C hazard onto a future ring. This
doc recons it as that future carrier. **It does not migrate it.**

---

## 2. What the op is — shape & call path

### 2.1 Guest side
`mach_msg_overwrite_trap_impl` (xnu `…/mach/impl/mach_traps.c:86`) is the single funnel for **all** Mach
IPC: `mach_msg_trap_impl` (the everyday `mach_msg`) tail-calls it with `rcv_msg = msg`. It issues one RPC:

```
dserver_rpc_mach_msg_overwrite(msg, option, send_size, rcv_size, rcv_name, timeout, notify, rcv_msg)
```

RPC arg shape (generate-rpc-wrappers.py:325, flags `XNU_TRAP_CALL | XNU_TRAP_NOPREFIX | ALLOW_INTERRUPTIONS`):

| arg        | type             | meaning |
|------------|------------------|---------|
| `msg`      | `void*`/`uint64_t`| **pointer into the caller's address space** — the message buffer, NOT inlined |
| `option`   | `int32_t`        | MACH_SEND_MSG / MACH_RCV_MSG / timeout / interrupt bits |
| `send_size`| `uint32_t`       | bytes to send |
| `rcv_size` | `uint32_t`       | receive buffer capacity (0 ⇒ send-only) |
| `rcv_name` | `uint32_t`       | receive port name |
| `timeout`  | `uint32_t`       | MACH_*_TIMEOUT value |
| `priority` | `uint32_t`       | (named `notify` in the guest signature) |
| `rcv_msg`  | `void*`/`uint64_t`| pointer for the overwrite-receive buffer |

The `ALLOW_INTERRUPTIONS` flag and the guest `retry:`/EINTR loop are the tell: **this RPC can block on the
server for an unbounded time** (a receive with no/long timeout parks until a message arrives).

### 2.2 Server side
There is **no** generated `Call::MachMsgOverwrite::processCall` — the op runs through the dtape `mach_msg`
machinery. The server reads the guest message body via `Process::readMemory` (process.cpp:276 →
`/proc/<pid>/mem` / `process_vm_readv`), which is **S2C-free** (one-way server→guest-memory read, no
upcall). The S2C upcalls happen later, during **descriptor materialization** (§3).

---

## 3. The danger map — why migration is hard

### 3.1 Blocking receive (the hard disqualifier for Lane 1)
`mach_msg_overwrite` "possibly sends; possibly receives" (osfmk/ipc/mach_msg.c:503). A receive parks the
**server-side** dtape thread until a message is available or the timeout fires. Lane 1 is a *closed
req→reply* model — the guest publishes, the server services to completion, publishes one reply, done. A
blocking receive breaks that contract: the server cannot promptly produce a reply, the ring slot stays
in-flight, and (worse) a parked receiver is exactly the thread a *sender's* caller-S2C would need. **The
receive path cannot ride Lane 1 at all.** Only the **send-only** shape (`rcv_size == 0`, no MACH_RCV_MSG)
is even a candidate, and only on Lane 2.

### 3.2 Caller-S2C upcalls — and NOT just munmap
The S2C enum has four members (rpc-supplement.h:1051): **mmap, munmap, mprotect, msync**. A Mach message
carrying an **OOL memory descriptor** drives the *receiver's* address space on copyout:

- **copyout (receive side):** `ipc_kmsg_copyout_ool_descriptor` → `vm_map_copyout` → `vm_map_copyout_kernel_buffer`
  (duct-tape/src/memory.c:424) → for a non-kernel map calls `dtape_hooks->task_allocate_pages` →
  `dtape_hook_task_allocate_pages` (server.cpp:316) → **mmap S2C** into the receiver.
- **teardown / send-destroy:** `vm_map_remove` (memory.c:496) → `task_free_pages` → `Process::freePages`
  → `Thread::_munmap` → `_s2cPerform` → **munmap S2C** (the D6-attributed path).

**D6's sideband only covers munmap.** Ring-migrating an OOL-carrying message would also need an **mmap-S2C
sideband** — a strict superset of D6. mprotect/msync S2C come from `change_protection`/`sync_memory`
(reachable via vm ops, not the common message path) — lower priority but part of the same hazard class.

### 3.3 Cross-process, not just self
The OOL mmap/munmap S2C goes to the **receiver** of the message, which is generally a **different process**
than the sender. So the hazard is not the self-targeted special case D4/D6 explored — the sideband must be
able to deliver an S2C to *whatever task* is the message receiver, and only when that task happens to be
ring-parked. This widens the parent-tracking surface considerably.

### 3.4 Port-right descriptors — name-space & refcount effects
`ipc_kmsg_copyout_object` / `copyout_port_descriptor` (ipc_kmsg.c:4931/4982) move or copy **port rights**
across the boundary: receive-right transfer, send-right insertion, name allocation in the receiver's
namespace, urefs/refcount adjustments, and the OOL-ports descriptor variant (an array of rights). These are
destroy-/mutate-capable side effects on the *receiver's* namespace and fail membership-canon rule #2/#3
(destroy-capable, not decidable purely pre-mutation) for anything but a no-descriptor message.

### 3.5 Complex / variable layout (no fixed inline body)
A complex message (`MACH_MSGH_BITS_COMPLEX`) is header + descriptor count + a variable descriptor array +
inline trailer. There is no fixed-size inline struct the ring arena can carry the way Lane-1 ops do — the
body is read from guest memory by length and parsed. A ring migration would have to either (a) keep using
`readMemory` from within the ring service (fine — it's S2C-free) or (b) marshal a variable arena. Either
way the op is *not* a tiny fixed-shape trap like the Lane-1 set.

### 3.6 Message semantics
At-most-once delivery, reply-port handling, voucher consumption, and ordering are all preserved by the
dtape path today. A ring migration must not reorder or duplicate sends. This is a correctness constraint on
any A/B (must be a true side-effect A/B, not a return-code A/B — canon rule #5).

---

## 4. Hotness / perf potential

### 4.1 Frequency (authoritative, committed)
perf#9-decompose attributed the guest's reply-wait time per callnum and found, on a **real build
workload**: **mach_msg_overwrite ≈ 19%** of all reply-bearing RPC — the **single largest** contributor,
ahead of mach_reply_port (8.7%) and mach_port_deallocate (8.4%). This figure is cited in four committed
sources: `dserver-rpc-defs.h:95`, the guest recv-spin patch (`generalize-recv-spin-guest.patch`),
`emulation/CMakeLists.txt:414`, and `per_call_metrics_test.cpp` (which uses it as *the* canonical "slow
call"). It is the reason the guest recv-spin (perf#7/#14) exists.

### 4.2 Live measurement this session (idle profile only — see caveat)
The only healthy booted server this session was the homebrew-test prefix (PID 2985458). A warm
`darling-stat` snapshot (uptime 1289s, 2353 RPCs) showed an **idle** steady-state profile dominated by
`pthread_canceled` polling (76%) with **zero** msg_overwrite — i.e. with no active guest workload there is
no Mach IPC, which is consistent with the 19% being a *build-workload* figure, not an idle one. A fresh
build-workload re-measurement was **not** captured: the build-tree launcher is not setuid-root and the one
root-setuid launcher targets a prefix whose overlay tree is broken ("too many levels of symbolic links").
Repairing that boot infra is out of scope for a recon deliverable, and the committed 19% figure is the
authoritative hotness number. **Honest status: hotness = 19% (committed, build workload); not
re-measured live this session.** A future executor wanting a fresh number should boot a clean prefix and
run `darling-stat` after a `make`/`clang` workload (per the perf#9 method), reading the `per_call`
`mach_msg_overwrite` histogram.

### 4.3 Reclaimable ceiling — wait vs service
perf#25.1 found startup RPC is **~90% empty round-trip** (the guest sleeps in recvmsg waiting for a reply
that the server produces quickly). For msg_overwrite specifically, the *send-only* shape is a short
server-side op dominated by round-trip wait — exactly the profile the ring reclaims (the Lane-1 wins were
~5–30× on round-trip-bound ops). The **receive** shape is genuinely blocking (the wait is real work, not
reclaimable overhead) and must NOT be spun/migrated. So the reclaimable prize is concentrated in the
send-only fraction of the 19%. Quantifying the send-only vs receive split is the **one measurement worth
taking before committing to a build** (method: a per-callnum send-only counter keyed on
`rcv_size==0 && !(option&MACH_RCV_MSG)`, emitted next to the existing per-call histogram).

---

## 5. Safe-subset analysis vs the membership canon (5 rules)

Candidate subset: **simple (non-complex) send-only message, no descriptors, `rcv_size==0`,
`!(option & MACH_RCV_MSG)`.**

| # | Canon rule | Whole op | Simple send-only subset |
|---|------------|----------|--------------------------|
| 1 | No caller-S2C upcall (or rides duplex) | ✗ (OOL ⇒ mmap+munmap S2C) | ✓ no OOL ⇒ no mmap/munmap S2C |
| 2 | No destroy-capable side effect | ✗ (port-right move, OOL free) | ✓ no descriptors ⇒ no right move |
| 3 | Decidable before mutation | ✗ (descriptor effects undecidable pre-parse) | ✓ shape known from header bits + option before any mutation |
| 4 | In ABI/hash opcode negotiation | n/a | ✓ (would join via the shape-gated opcode set) |
| 5 | Side-effect A/B (not return-code) | required | required — must A/B the *delivered message + namespace*, not just `kr` |

**Result:** the simple send-only subset passes rules 1–3 *if and only if* the ring service decodes the
header bits + option and **declines to UDS** anything that is complex, descriptor-bearing, or has
MACH_RCV_MSG set. That pre-dispatch shape gate is exactly the D4/D6 "decline before mutation" pattern. But
even this subset is **blocking-capable** if the message send itself can block (a full destination queue
with no MACH_SEND_TIMEOUT), so the gate must also require a send timeout or accept that a full-queue send
parks the ring slot. **This subset belongs on Lane 2 (duplex), not Lane 1** — because a send can still
provoke a notification/reply path, and because the moment any descriptor appears we need the sideband.

---

## 6. Recommendation (go / no-go)

1. **NO-GO on Lane-1 migration** of msg_overwrite — categorically. The blocking receive and
   variable/complex/descriptor body violate the closed-req→reply contract.
2. **NO-GO on whole-op migration** to any lane — the receive path must stay UDS (its wait is real work),
   and descriptor/OOL handling needs sideband machinery D6 does not yet provide.
3. **CONDITIONAL-GO, as a future scoped bead, on a Lane-2 (duplex) migration of the SEND-ONLY,
   SIMPLE-MESSAGE subset**, gated by:
   - a pre-dispatch shape gate (decline → UDS unless `MACH_SEND_MSG && !MACH_RCV_MSG && rcv_size==0 &&
     !MACH_MSGH_BITS_COMPLEX && no descriptors`);
   - **first** extending the D6 sideband from munmap-only to an **mmap-S2C sideband** (prerequisite — even
     a "simple" send can, on the *receive* side elsewhere, drive copyout; and the safe-subset gate must be
     proven to actually exclude every S2C-driving path);
   - a **true side-effect A/B** (delivered message bytes + receiver port namespace + refcounts identical
     ring-vs-UDS), per canon rule #5;
   - the standard fork-storm 3/3 + isolation + Lane-1-no-regress regression.
4. **Measure first (cheap, no migration):** add the send-only vs receive per-callnum split counter (§4.3)
   to size the *actual* reclaimable fraction of the 19% before investing in the Lane-2 build. If the
   send-only fraction is small, even the conditional-go isn't worth it.

**D6 is the architectural prerequisite recorded here:** the caller-S2C hazard that msg_overwrite carries is
*already closed for ring parents* by the D6 sideband (munmap), and extending it to mmap is the gating work
item for any future OOL-message ring migration. No OOL-carrying message can ride a ring lane until that
sideband superset exists.

---

## 7. Non-goals (held)
- No ring wiring for mach_msg_overwrite (this is recon only).
- No change to the frozen Lane-1 op-set {task_self_trap, mach_reply_port, mach_port_allocate,
  mach_port_insert_right}.
- No resurrection of mach_port_deallocate / mach_port_mod_refs onto any lane.
- No launchd-global / default-ON duplex.
- No change to D6 behavior (committed, default-OFF).

## 8. Pointers
- Guest trap: xnu `…/mach/impl/mach_traps.c:86` `mach_msg_overwrite_trap_impl`; RPC wrapper spec
  `generate-rpc-wrappers.py:325`; callnum `dserver_callnum_mach_msg_overwrite = 38`.
- Server body read: `Process::readMemory` (process.cpp:276) — S2C-free.
- OOL copyout (mmap S2C): duct-tape/src/memory.c `vm_map_copyout*` (405–487) → `task_allocate_pages`
  (server.cpp:316).
- OOL teardown (munmap S2C): memory.c `vm_map_remove` (496) → `task_free_pages` → Process::freePages →
  Thread::_munmap → `_s2cPerform` (thread.cpp:1352).
- Descriptor copyout: osfmk/ipc/ipc_kmsg.c `ipc_kmsg_copyout_object`/`_port_descriptor`/
  `_ool_descriptor`/`_ool_ports_descriptor` (4931/4982/5020/5168).
- S2C enum: rpc-supplement.h:1051 {mmap, munmap, mprotect, msync}.
- D6 sideband (munmap-only): thread.cpp `_s2cTryDuplexMunmapLocked` / `_drainDuplexReply`; mailbox/caps in
  rpc-supplement.h; attribution counters `s2c_munmap_{ring,uds,no}_parent`.
- Hotness 19%: dserver-rpc-defs.h:95 + generalize-recv-spin-guest.patch + per_call_metrics_test.cpp.

---

## 9. D8 — shape census MEASURED (the decisive number)

The §4.3 "cheap next measurement" was BUILT and RUN. A diagnostic, default-OFF shape census
(`Metrics::recordMsgOverwriteCensus`, armed by `DARLING_SERVER_MSG_CENSUS=1`, classified in
`Call::callFromMessage` — one chokepoint catching every msg_overwrite, ring or UDS) was deployed to a
warm homebrew-test server and a representative boot + shell workload was run. **No behavior change**
(off by default; when off the classification + its header readMemory are skipped entirely). Gate:
`tests/msg_overwrite_census_test.cpp` (GREEN + 2 RED arms), wired into `run-ring-shm-validate.sh`;
compiles + passes in BOTH ring-ON and ring-OFF builds.

**Measured (msg_total = 362, one warm boot + `true`/`ls`/`uname` workload):**

| shape | count | % of msg_overwrite |
|-------|------:|-------------------:|
| send+receive (MIG round-trip) | 276 | **76.2%** |
| **blocking receive** (RCV, no finite timeout) | 317 | **87.6%** |
| rcv_size != 0 | 317 | 87.6% |
| receive-only | 41 | 11.3% |
| send-only (total) | 45 | 12.4% |
| **send-only SIMPLE** (no descriptors — the Lane-2 candidate) | 30 | **8.3%** |
| send-only complex | 15 | 4.1% |
| send-only OOL (the stopper hazard) | 2 | 0.6% |
| send-only port descriptors | 13 | 3.6% |
| header-read failures | 0 | 0.0% |

**Verdict (data-confirmed): the reclaimable fraction is small.** The dominant shape (76% send+receive,
88% blocking receive) is a *real* wait — a thread blocked in `ipc_mqueue_receive` waiting for a peer's
reply — NOT reclaimable round-trip overhead the way the Lane-1 self-trap wins were. The only Lane-2
candidate, simple send-only, is **8.3% of msg_overwrite** ≈ **8.3% × 19% ≈ 1.6% of all RPC**. The OOL
hazard that would force the mmap-sideband prerequisite is real but rare (0.6%).

**Roadmap decision (per the §6 fork): STOP — do NOT build a msg_overwrite Lane-2 subset.** Chasing
~1.6% of total RPC is not worth a duplex-subset build + the mmap-sideband prerequisite. The receive
shape stays UDS/generic (correct — its wait is real work). The architectural STOPPER stands (no OOL
ring migration until the sideband does mmap), but it is now moot for msg_overwrite specifically. **Look
elsewhere for hot CLOSED ops** — the next perf lever is NOT this op. (Caveat: one warm sample; the
shares are stable enough to decide, but a longer real-build workload would refine the absolute counts.
The per_call histogram showed 0 msg_overwrite in this same warm window because per_call records only
UDS-serviced calls in a different sampling window; the census's 362 is the authoritative count for the
shape question.)
