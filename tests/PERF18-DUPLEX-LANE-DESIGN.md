# perf#18 Phase C/D (P8) — Duplex ring lane: design + the wake-model hard gate

**Beads:** dar-1il.3 (Phase C, design) → dar-1il.3.1 (Phase D, prototype).
**Branches (LOCAL):** darlingserver `perf/shmem-ring-abi-validator` · xnu `perf/shmem-ring-guest`.

> This is the DESIGN + the load-bearing correctness artifact for P8. The simple Lane-1 fast ring is
> frozen and untouched. Lane 2 is a SEPARATE protocol: a destroy-capable / caller-S2C op can ride a
> ring without the parked-caller deadlock that sank deallocate (dar-1il.1) and mod_refs (dar-1il.2).

## 1. The problem, precisely (from the real code)

`Thread::_s2cPerform()` (thread.cpp ~1351) sends the S2C call over `Server::sharedInstance().sendMessage()`
— **UDS** — and blocks the microthread on `_s2cReplySempahore`. The guest services that S2C only while it
is sitting in a UDS `recvmsg` (the `interrupt_enter`/S2C-signal path). A guest parked on the **simple ring**
(`gr_wait_reply`, dserver-ring.c ~233) is in a bounded spin then `FUTEX_WAIT(s2c_futex)` watching ONLY the
s2c REPLY ring — it is NOT in recvmsg, so it cannot answer the S2C → server microthread blocks on
`_s2cReplySempahore`, guest blocks on the futex → **deadlock**. mach_port_deallocate of a mapped-region-backed
port drives exactly this (vm-munmap S2C to the caller).

## 2. Lane 2 minimal protocol (hard limits, by design)

One parent request in flight **per calling thread**; one outstanding S2C upcall at a time; **caller-thread
pump** (no helper thread); fallback to UDS on any unexpected shape; NO broad migration; NO change to Lane 1.

Four message classes on a duplex ring pair (separate from the Lane-1 rings; same memfd/control-block region OK):
- `C2S_PARENT_REQUEST` — guest → server, the op (e.g. deallocate) + a `parent_id`.
- `S2C_UPCALL` — server → guest, mid-op, carries `parent_id` + `upcall_id` + the S2C op (mmap/munmap/...).
- `C2S_UPCALL_REPLY` — guest → server, the upcall result, correlated by `upcall_id`.
- `S2C_FINAL_REPLY` — server → guest, the parent op's result, correlated by `parent_id`.

Guest wait-pump (the shape the bead specifies):
```
send_duplex_request(parent_id, op, args);
for (;;) {
    if (final_reply_ready(parent_id)) return final_reply(parent_id);
    while (s2c_upcall_available()) {
        up = pop_s2c_upcall();                       // server → guest request ring
        result = handle_upcall_on_this_thread(up);   // SAME thread → correct current_task/dtape ctx
        publish_c2s_upcall_reply(up.id, result);     // guest → server reply ring
    }
    adaptive_spin_then_futex();                       // park on the duplex s2c futex
}
```
Server: `run_parent_request`; if it needs a caller S2C, `publish_s2c_upcall(parent,upcall)` then
`wait_for_c2s_upcall_reply(upcall)` (NOT assuming the caller is in recvmsg); then `publish_final_reply`.

## 3. The wake model — TWO waiter relationships on ONE parked thread (the hard gate)

The simple ring had ONE: guest waits for its reply; server wakes it. The duplex lane has the guest, while
parked, simultaneously (a) waiting for its FINAL reply AND (b) obligated to wake for an S2C UPCALL request.
So the parked guest watches TWO producer streams (the s2c-upcall ring and the s2c-final-reply ring), and the
server, while waiting for a C2S UPCALL reply, watches the c2s-upcall-reply ring. Each park point must be
lost-wake-free against the producer that can feed it.

**Invariants (must hold for every interleaving):**
- **DUPLEX-GUEST:** a guest that commits to parking is woken if EITHER an S2C upcall OR the final reply is
  (or becomes) available — it must not sleep through either. Modeled with ONE waiter bit the guest sets
  before its pre-park recheck of BOTH rings; the server sets a pending-wake when it publishes to either ring
  and reads the bit AFTER publishing (publish-before-read-bit, like the simple ring's response side).
- **DUPLEX-SERVER:** the server, parked waiting for the C2S upcall reply, is woken when the guest publishes
  it — same publish-before-read-bit discipline on the upcall-reply ring's own waiter bit.
- **NO-LOST-UPCALL:** the guest's pump drains ALL pending S2C upcalls before parking (a `while`, not an
  `if`), so a burst can't leave one stranded while the guest sleeps for the final reply.

These are the predicates the gate enumerates. The RED arms break each recheck/ordering and the enumeration
must find lost > 0.

## 4. RED arms (deterministic, mirror dar-my8)
- `DUPLEX_NO_GUEST_PUMP`: guest parks for the final reply WITHOUT draining the upcall ring first → an
  outstanding S2C upcall is stranded (server blocked on its reply, guest asleep) → lost progress.
- `DUPLEX_WAITERBIT_AFTER_RECHECK`: guest sets its waiter bit AFTER rechecking the rings → server can read
  bit==0 between recheck and park and skip the wake → lost wake.
- `DUPLEX_WRONG_CORRELATION`: an upcall reply tagged with the wrong `upcall_id` (or a final reply with the
  wrong `parent_id`) is accepted → the server resumes on a reply that isn't its upcall's → protocol error.

## 5. Acceptance ladder (P8)
1. **THIS artifact — wake-model gate GREEN, RED arms fail.** (the hard gate; pure logic, hermetic)
2. Wire the duplex transport (Phase D): server `_s2cPerform` duplex variant for a ring-parked caller +
   guest pump in `gr_wait_reply`'s sibling. Migrate ONE op (deallocate of a mapped-region-backed port).
3. Side-effect A/B vs prod-OFF UDS INCLUDING a real mapped-region destroy under a launchd-like boot.
4. Simple-ring perf unchanged (mach_reply_port p50 ~sub-µs); fork-storm 3/3; death/teardown cases.

Steps 2–4 are the rest of Phase D/E; step 1 is the gate that proves the model is sound BEFORE any wiring.
