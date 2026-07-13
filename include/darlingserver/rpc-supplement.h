#ifndef _DARLINGSERVER_RPC_SUPPLEMENT_H_
#define _DARLINGSERVER_RPC_SUPPLEMENT_H_

#include <stdint.h>
#include <stdbool.h>

#if __cplusplus
extern "C" {
#endif

//
// kqueue channels
//

/**
 * kqchan is short for "kqueue channel".
 *
 * kqueue channels are used to allow libkqueue to monitor events
 * that occur on the server side (in darlingserver). This is necessary
 * for filters like EVFILT_MACHPORT and EVFILT_PROC.
 *
 * The general process works like this:
 *   1. libkqueue is told to add a knote for one of the special filters that requires a kqchan.
 *   2. libkqueue hands that off to our filter handler code for that filter.
 *   3. our filter handler opens a kqchan using a darlingserver RPC call.
 *   4. darlingserver sets up the kqchan on the server side and sends back a socket
 *      that the client can monitor and send channel messages to.
 *   5. when the client needs to modify some of the server state for the channel,
 *      it sends a special message (specialized depending on the channel type).
 *   6. when the server receives an event of interest, it notifies the client
 *      by sending a generic notification message (to which the client should NOT reply).
 *   7. when the client receives a notification, libkqueue will attempt to read the message
 *      by calling the copyout method on the filter. for kqchan-based filters, this means
 *      sending a special message (specialized depending on the channel type) to which the
 *      server replies with the necessary event information.
 */

enum dserver_kqchan_msgnum {
	dserver_kqchan_msgnum_invalid = 0,

	/**
	 * Indicates that something has occurred on the server side that the client should know about.
	 *
	 * This is the only type of server-initiated message.
	 *
	 * This is common to all types of kqueue channels.
	 *
	 * This message does NOT require a reply.
	 */
	dserver_kqchan_msgnum_notification,

	/**
	 * A request to modify the server context for this mach port kqueue channel.
	 */
	dserver_kqchan_msgnum_mach_port_modify,

	/**
	 * A request to read the data for the most recent notification on this mach port kqueue channel.
	 */
	dserver_kqchan_msgnum_mach_port_read,

	/**
	 * A request to modify the server context for this proc kqueue channel.
	 */
	dserver_kqchan_msgnum_proc_modify,

	/**
	 * A request to read the data for the most recent notification on this proc kqueue channel.
	 */
	dserver_kqchan_msgnum_proc_read,
};

typedef enum dserver_kqchan_msgnum dserver_kqchan_msgnum_t;

typedef struct dserver_kqchan_callhdr {
	dserver_kqchan_msgnum_t number;
	int pid;
	int tid;
} dserver_kqchan_callhdr_t;

typedef struct dserver_kqchan_replyhdr {
	dserver_kqchan_msgnum_t number;
	int code;
} dserver_kqchan_replyhdr_t;

typedef struct dserver_kqchan_call_notification {
	dserver_kqchan_callhdr_t header;
} dserver_kqchan_call_notification_t;

typedef struct dserver_kqchan_call_mach_port_modify {
	dserver_kqchan_callhdr_t header;
	uint64_t receive_buffer;
	uint64_t receive_buffer_size;
	uint64_t saved_filter_flags;
} dserver_kqchan_call_mach_port_modify_t;

typedef struct dserver_kqchan_reply_mach_port_modify {
	dserver_kqchan_replyhdr_t header;
} dserver_kqchan_reply_mach_port_modify_t;

typedef struct dserver_kqchan_call_mach_port_read {
	dserver_kqchan_callhdr_t header;
	uint64_t default_buffer;
	uint64_t default_buffer_size;
} dserver_kqchan_call_mach_port_read_t;

typedef struct dserver_kqchan_reply_mach_port_read {
	dserver_kqchan_replyhdr_t header;
	struct {
		uint64_t ident;
		int16_t filter;
		uint16_t flags;
		int32_t qos;
		uint64_t udata;
		uint32_t fflags;
		uint32_t xflags;
		int64_t data;
		uint64_t ext[4];
	} kev;
} dserver_kqchan_reply_mach_port_read_t;

typedef struct dserver_kqchan_call_proc_modify {
	dserver_kqchan_callhdr_t header;
	uint32_t flags;
} dserver_kqchan_call_proc_modify_t;

typedef struct dserver_kqchan_reply_proc_modify {
	dserver_kqchan_replyhdr_t header;
} dserver_kqchan_reply_proc_modify_t;

typedef struct dserver_kqchan_call_proc_read {
	dserver_kqchan_callhdr_t header;
} dserver_kqchan_call_proc_read_t;

typedef struct dserver_kqchan_reply_proc_read {
	dserver_kqchan_replyhdr_t header;
	uint32_t fflags;
	int64_t data;
} dserver_kqchan_reply_proc_read_t;

//
// perf #18 (bead dar-dar6x4-perf-5dq.30): shared-memory SPSC ring transport ABI.
//
// The hot-path RPC round-trip (guest sendmsg -> server -> guest recvmsg) is ~90% empty
// cross-process wakeup latency, not server work (memory dar-perf-startup-rpc-90pct-empty-
// roundtrip). This is the wire format for moving small, reply-bearing, fd-free C2S calls
// onto a shared-memory ring with a futex wake, keeping UDS for connect/auth/fd-passing/
// fallback. See ~/work/dar-6x4-repro/PERF18-SHMEM-RING-ABI.md for the full design.
//
// THE GUEST IS UNTRUSTED. Every field below is written by the guest into shared memory and
// may change at any time; the server copies-in and validates before use, and NEVER
// dereferences a guest-supplied pointer (offsets only, bounds-checked). The pure validator
// dserver_ring_shm_validate() at the bottom is the trust-boundary gate; it is compiled
// identically into the server and into the adversarial unit test.
//
// This whole block is gated behind DSERVER_RING_TRANSPORT so a default build is
// byte-identical (the structs/inline fn are only referenced when the feature is enabled).
//

#ifdef DSERVER_RING_TRANSPORT

#define DSERVER_RING_MAGIC       0x44524e47u /* 'DRNG' */
// ABI v2 (perf #18 P4, dar-dar6x4-perf-5dq.33): added the wake-model state words
// (server_state + s2c_waiters) to the control block. Bumped so a v1 guest/server pair rejects
// cleanly at attach instead of mismatching the struct size.
// ABI v3 (perf #18 dar-1il.2 item 2): added c2s_opcode_hash to the control block (the guest's
// compiled C2S opcode-set hash, cross-checked at attach). The struct grew, so bump the version too:
// a v2 server reading a v3 guest's block (or vice versa) rejects on abi_version BEFORE it would ever
// misread the new field, and a same-version pair whose opcode SETS still differ rejects on the hash.
// ABI v4 (perf #18 P8 D1/D2, dar-1il.3.1): added the DUPLEX MAILBOX (duplex_* words below) + a
// capability flag (duplex_caps) to the control block for the minimal duplex lane (one synthetic S2C
// upcall + reply to a ring-parked caller). The struct grew again, so a v3<->v4 pair rejects on
// abi_version at attach (clean all-UDS fallback) -- a v3 server never misreads the duplex words, and
// the duplex datapath only activates when BOTH sides are v4 AND the caps bit is set. The Lane-1 c2s/
// s2c RINGS are byte-identical; the mailbox lives in the fixed control block on its own cache lines.
// ABI v5 (perf #18 P8 D4, dar-1il.3.2.1): the duplex mailbox grew a TYPED PAYLOAD so a REAL S2C upcall
// (the vm-munmap that mach_port_deallocate of a mapped-region-backed port drives) can ride the lane,
// not just the synthetic single-uint32 echo. The upcall slot gained an address+length u64 pair and the
// reply slot a second int (return_value + errno) -- enough to carry dserver_s2c_call_munmap_t /
// dserver_s2c_reply_munmap_t losslessly. v4<->v5 rejects on abi_version at attach (clean all-UDS). The
// echo selftest still works (it uses duplex_upcall_arg / duplex_reply_arg, unchanged). Lane-1 rings stay
// byte-identical; the new words live in the mailbox cache lines.
#define DSERVER_RING_ABI_VERSION 5u

// perf #18 P8 D1/D2: duplex capability bits, advertised by the guest in cb.duplex_caps and confirmed
// by the server. The minimal prototype advertises exactly DUPLEX_SELFTEST: it can pump ONE synthetic
// S2C upcall to a ring-parked caller and reply. NO real op rides the duplex lane yet (no deallocate,
// no mod_refs). A server that doesn't recognize a cap bit simply doesn't use that duplex path.
#define DSERVER_RING_DUPLEX_CAP_SELFTEST 0x1u
// perf #18 P8 D4 (dar-1il.3.2.1): the guest advertises this cap iff it can pump the vm-munmap S2C upcall
// shape AND its routing layer is allowed to send mach_port_deallocate over the duplex lane (gated behind
// a per-command hatch, default OFF -- see __dserver_ring_dealloc_via_duplex_enabled). The server's
// _s2cPerform duplex variant for the munmap upcall requires this bit; without it, a deallocate that
// drives a munmap S2C takes the UDS S2C path unchanged. Negotiated like SELFTEST: guest-written into
// duplex_caps at attach, the server reads it under the conjunction guard. (No silent drop: if the guest
// routed a deallocate onto the duplex lane it MUST have set this cap; the server services or the parent
// op fails closed + the guest UDS-falls-back the NEXT call at the routing layer.)
#define DSERVER_RING_DUPLEX_CAP_DEALLOCATE 0x2u
// perf #18 P8 D5 (dar-1il.3.2.2): the guest advertises this cap iff its routing layer is allowed to send
// mach_vm_deallocate over the duplex lane (gated behind a per-command hatch, default OFF -- see
// __dserver_ring_vm_dealloc_via_duplex_enabled). vm_deallocate is the op that ACTUALLY drives a real
// caller munmap S2C in Darling (mach_vm_deallocate -> vm_map_remove -> dtape_hook_task_free_pages ->
// _munmap -> _s2cPerform); mach_port_deallocate (D4) does NOT, because mach_make_memory_entry_64 is a
// stub. D5 routes vm_deallocate onto the SAME D4 duplex munmap transport (no new wire format): the munmap
// S2C uses DSERVER_RING_DUPLEX_UPCALL_MUNMAP + the v5 typed mailbox payload unchanged. The server's
// munmap publish guard (_s2cTryDuplexMunmapLocked) accepts EITHER the DEALLOCATE or the VM_DEALLOCATE cap
// (both mean "this caller can pump a munmap S2C"); the per-op ROUTING decline in ringServiceThread keys on
// THIS bit specifically so a vm_deallocate from a non-VM-capable guest declines pre-dispatch -> UDS. A new
// forward-compatible cap bit needs no ABI bump (the v5 mailbox wire layout is unchanged); an old server
// that doesn't know 0x4 simply never sets it, so the guest never routes vm_deallocate onto the lane.
#define DSERVER_RING_DUPLEX_CAP_VM_DEALLOCATE 0x4u
// Convenience: either cap means "this caller can pump a vm-munmap S2C upcall" (the shape both D4 and D5
// deliver). The munmap publish guard requires this; the per-op routing decline requires the specific bit.
#define DSERVER_RING_DUPLEX_CAP_MUNMAP_PUMP (DSERVER_RING_DUPLEX_CAP_DEALLOCATE | DSERVER_RING_DUPLEX_CAP_VM_DEALLOCATE)

// perf #18 P8 D1/D2: the synthetic duplex parent op. A guest selftest publishes a c2s slot with this
// reserved callnum sentinel (NOT a real dserver_callnum_*, deliberately out of the generated enum's
// range so it can never collide) to drive ONE duplex roundtrip. The server recognizes ONLY this
// sentinel as a duplex parent; every other callnum takes the existing Lane-1 path unchanged.
#define DSERVER_RING_DUPLEX_SELFTEST_CALLNUM 0xD0DE0001u

// perf #18 P8 D1/D2: the synthetic S2C upcall op carried in the mailbox. The minimal protocol defines
// exactly one supported upcall shape (a no-side-effect echo: guest returns the payload transformed),
// so the "upcall shape supported" precondition is a single equality check.
#define DSERVER_RING_DUPLEX_UPCALL_ECHO 0x1u
// perf #18 P8 D4 (dar-1il.3.2.1): the REAL vm-munmap S2C upcall shape. Carried in the typed mailbox
// payload (duplex_upcall_addr + duplex_upcall_len out; duplex_reply_status as the kernel return_value /
// duplex_reply_errno as the errno back). This is the S2C that mach_port_deallocate of a mapped-region-
// backed port drives; the guest pump runs the SAME munmap(2) the UDS recvmsg S2C path runs, so the side
// effect is byte-identical to UDS -- only the transport differs.
#define DSERVER_RING_DUPLEX_UPCALL_MUNMAP 0x2u

// perf #18 P8 D4 (dar-1il.3.2.1): a reserved reply-header `code` the server uses to tell the guest it
// DECLINED a duplex-routed op BEFORE dispatching it (pre-mutation: no v5 ring / no DEALLOCATE cap /
// mailbox busy / unsupported shape). The guest treats this as "transport miss -> UDS-fall-back", NOT as
// a kern_return_t. It is deliberately a large negative value that no Mach kern_return_t takes (those are
// small non-negative codes or err_mach_ipc-range values), so it can never be confused with a real
// result. The decline is ALWAYS pre-mutation, so the UDS fall-back re-runs the op exactly once.
#define DSERVER_RING_DUPLEX_DECLINE ((int32_t)0x7ADEC11E)

// perf #18 P4 wake model: the server publishes its current sleep state into server_state so a
// producing guest can SKIP the eventfd doorbell when the server is actively polling (it will
// see the request on its own). Values are a plain uint32 (portable wire format); transitions
// are single release-stores. Guests treat any unknown value conservatively as "not polling"
// (== send the doorbell), so an old/garbage value only costs a redundant wake, never a missed one.
#define DSERVER_RING_SRV_SLEEPING_EPOLL 0u // server is (or is about to be) blocked in epoll_wait -> MUST doorbell
#define DSERVER_RING_SRV_ACTIVE_POLLING 1u // server is draining rings in its spin phase -> doorbell NOT needed
#define DSERVER_RING_SRV_SLEEP_ARMED    2u // server is between "decided to sleep" and epoll_wait -> doorbell (race window)

// Bounds the server is willing to accept for a guest-proposed ring, so a malicious or buggy
// guest can't make the server map an absurd region or compute a huge index. Kept modest;
// raise deliberately if a real workload needs deeper rings.
#define DSERVER_RING_MIN_SLOT_SIZE   64u
#define DSERVER_RING_MAX_SLOT_SIZE   4096u
#define DSERVER_RING_MIN_SLOT_COUNT  2u
#define DSERVER_RING_MAX_SLOT_COUNT  1024u
#define DSERVER_RING_MAX_TOTAL_SIZE  (16u * 1024u * 1024u) /* 16 MiB cap per ring mapping */

// Per-direction ring header. head/tail live on separate cache lines (the producer owns tail,
// the consumer owns head) so the two sides never false-share. slot_size-byte slots follow.
// The header is included by both C (guest, C11 _Alignas) and C++ (server, alignas), so the
// alignment keyword is selected per language.
#if defined(__cplusplus)
	#define DSERVER_RING_ALIGN64 alignas(64)
#else
	#define DSERVER_RING_ALIGN64 _Alignas(64)
#endif
typedef struct dserver_ring {
	DSERVER_RING_ALIGN64 uint32_t head; // consumer-owned: next slot to READ
	char _pad_head[64 - sizeof(uint32_t)];
	DSERVER_RING_ALIGN64 uint32_t tail; // producer-owned: one past last PUBLISHED slot
	char _pad_tail[64 - sizeof(uint32_t)];
	// dserver_ring_slot slots[slot_count] follow here
} dserver_ring_t;

// Overlay on each slot. The inline payload carries the existing call/reply struct body;
// bodies too big for a slot use the arena (arena_len > 0).
typedef struct dserver_ring_slot {
	uint32_t length;     // inline payload bytes used (<= slot_size - sizeof(dserver_ring_slot))
	uint32_t arena_off;  // if arena_len>0: byte offset into the arena region
	uint32_t arena_len;  // if >0: payload is in the arena, this many bytes
	uint32_t seq;        // monotonic per ring; a reply echoes its request's seq
	uint32_t callnum;    // dserver_callnum_t; validated against the C2S call switch
	uint32_t flags;      // bit0 = reply-is-error; rest reserved (must be 0)
	// char payload[] follows
} dserver_ring_slot_t;

// --- SPSC datapath ---------------------------------------------------------------------
//
// Single-producer / single-consumer ring access. Both directions share these helpers: for
// the c2s ring the guest is the producer and the server is the consumer; for the s2c ring
// it is reversed. They operate on offsets validated by dserver_ring_shm_validate(), so the
// only attacker-controlled values at runtime are head/tail (each owned by ONE side). We
// re-clamp them on every access anyway -- a corrupt index from the untrusted peer must never
// be able to make us read/write outside the ring's own slot array.
//
// Discipline (torn-write-safe, no locks). head and tail are FREE-RUNNING uint32 counters
// (never pre-masked); the slot index is (counter & (slot_count-1)). slot_count is a validated
// power of two, so the counters wrapping at 2^32 is harmless (2^32 is a multiple of any
// power-of-two slot_count, so the masked index stays continuous across the wrap).
//   * tail = total slots ever PUBLISHED; head = total slots ever CONSUMED.
//   * empty when tail == head; full when (tail - head) == slot_count.
//   * The producer writes the whole slot body, THEN does ONE release-store incrementing tail.
//     A consumer that observes the new tail therefore observes a fully written slot.
//   * The consumer reads the slot, THEN does ONE release-store incrementing head. The producer
//     treats a slot as free once head has advanced past it.
// The (tail - head) distance is itself untrusted (the peer owns one end), so we treat any
// distance > slot_count as "corrupt -> empty/full" rather than trusting it to index.
//
// Header-only inline so the guest (libc-free C), the server (C++), and the host loopback test
// all compile the IDENTICAL datapath.

// Pointer to the slot for free-running counter value `counter` within a ring whose header is
// at `ring`. slot_size/slot_count come from the validated control block.
static inline dserver_ring_slot_t* dserver_ring_slot_at(dserver_ring_t* ring, uint32_t counter, uint32_t slot_size, uint32_t slot_count) {
	uint32_t idx = counter & (slot_count - 1u);
	char* slots = (char*)ring + sizeof(dserver_ring_t);
	return (dserver_ring_slot_t*)(slots + (uint64_t)idx * slot_size);
}

// Producer: claim the next free slot and return a pointer to it WITHOUT publishing, or NULL
// if the ring is full (or the consumer's head looks corrupt). The caller writes the slot
// fields + body, then calls dserver_ring_producer_publish().
static inline dserver_ring_slot_t* dserver_ring_producer_begin(dserver_ring_t* ring, uint32_t slot_size, uint32_t slot_count) {
	uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED); // producer owns tail
	uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE); // consumer owns head (untrusted)
	uint32_t used = tail - head; // wraps correctly in unsigned arithmetic
	if (used >= slot_count) {
		return (dserver_ring_slot_t*)0; // full, or a corrupt head -> refuse to produce
	}
	return dserver_ring_slot_at(ring, tail, slot_size, slot_count);
}

// Producer: publish the slot claimed by dserver_ring_producer_begin(). The single release
// store of tail is the linearization point; everything written to the slot happens-before it.
static inline void dserver_ring_producer_publish(dserver_ring_t* ring) {
	uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->tail, tail + 1u, __ATOMIC_RELEASE);
}

// Consumer: return a pointer to the next published slot WITHOUT consuming, or NULL if empty
// (or the producer's tail looks corrupt). The caller reads the body, then calls
// dserver_ring_consumer_advance().
static inline dserver_ring_slot_t* dserver_ring_consumer_begin(dserver_ring_t* ring, uint32_t slot_size, uint32_t slot_count) {
	uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED); // consumer owns head
	uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE); // producer owns tail (untrusted)
	uint32_t avail = tail - head; // wraps correctly in unsigned arithmetic
	if (avail == 0u || avail > slot_count) {
		return (dserver_ring_slot_t*)0; // empty, or a corrupt tail -> consume nothing
	}
	return dserver_ring_slot_at(ring, head, slot_size, slot_count);
}

// Consumer: free the slot returned by dserver_ring_consumer_begin().
static inline void dserver_ring_consumer_advance(dserver_ring_t* ring) {
	uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->head, head + 1u, __ATOMIC_RELEASE);
}

// --- Ring message convention (P3, first migrated op) ----------------------------------
//
// A request slot carries: callnum = dserver_callnum_<name>, seq = a guest-chosen request id,
// length = inline request-body bytes, payload = the dserver_call_<name>_t body (none for a
// no-arg call like task_self_trap). A reply slot echoes the request's callnum + seq, sets
// flags bit0 if the call failed, carries the result code in dserver_ring_reply_hdr_t at the
// start of the payload, followed by the dserver_reply_<name>_t body. Big bodies (arena_len>0)
// are P4; P3 only migrates small inline-only ops.
#define DSERVER_RING_FLAG_REPLY_ERROR 0x1u

// --- C2S ring opcode allowlist: the SINGLE source of truth (perf #18 P5-bulk, dar-1il.1) ------
//
// The set of dserver_callnum_* values a guest may publish onto the c2s ring (and that the server
// will therefore service over the ring) MUST be defined in exactly ONE place. Both sides consume
// THIS X-macro:
//   * the guest (dserver-ring.c) -- only these ops have a ring helper / route over the ring;
//   * the server (call.cpp ringServiceThread) -- the `eligible` check is generated from this list.
//
// THE PROTOCOL INVARIANT (no silent drop): if the guest can publish an opcode onto the ring, the
// server MUST service it over the ring (or reply an explicit error). The fatal failure mode this
// list exists to PREVENT is drift: an opcode the guest routes over the ring but the server does
// NOT allowlist -> the server consumes the request slot and produces no reply -> the guest is
// stranded on its bounded reply-wait (it published, so it will NOT UDS-fall-back for that call) ->
// effective per-call wedge. This was the exact bug class P5 hit under an early per-op server hatch
// (see dar-1il P5 comment). Keying both sides off one macro makes that drift a compile-time
// impossibility, and ring_drift_gate_test.c asserts the two derived sets are identical.
//
// Each entry carries the BARE op name; a consumer forms the enum value as dserver_callnum_##name
// (so this header stays independent of the GENERATED rpc.h -- the consumer includes that itself).
//
// MEMBERSHIP RULE: an op belongs here iff it is C2S, reply-bearing, fd-free, has an empty body +
// small inline reply (the port traps) or a small inline body + header-only reply (the port/right
// bookkeeping ops -- result is the kern_return_t, any out-value travels by the existing guest-memory
// write the generic Call path already performs), AND -- CRITICALLY -- it NEVER triggers a
// server-to-client (S2C) upcall to the calling thread. This last rule is load-bearing: a thread
// parked on a ring reply is NOT sitting in a UDS recvmsg, so it CANNOT service an S2C call. An op
// that needs an S2C upcall to the caller (e.g. a port destruction that drives a vm munmap upcall)
// will deadlock: the server microthread blocks on the S2C reply semaphore while the guest blocks in
// the ring futex-wait, neither able to make progress. This is exactly why mach_port_deallocate is
// NOT here (see dar-1il.1): under launchd's real workload it destroys ports backing mapped regions
// -> munmap S2C -> wedge. It stays on UDS, where the caller's recvmsg can service the S2C. The
// safe ops below only CREATE/REF (allocate, insert_right) -- a pure right-table write with no
// caller upcall.
//
// mach_port_mod_refs is ALSO deliberately NOT here (dar-1il.2): it is destroy-CAPABLE. A
// mod_refs(right, delta<0) that drops the last user-ref of a receive right destroys the port, and
// destroying a port that backs a mapped region drives the SAME vm munmap S2C upcall to the caller
// that sank deallocate -- but the failure is a SIDE EFFECT of certain args, not the return code, so
// the byte-identical kern_return_t A/B in dar-1il.1 (which destroyed only bare ports) did NOT prove
// safety. The safety of a given mod_refs call cannot be decided until the ipc_space lock is held and
// the right's current refcount is known, i.e. AFTER mutation begins, which violates the "decide
// safety BEFORE mutating" rule -- you cannot start, discover "oops, need S2C", and roll back. So
// mod_refs stays UDS until a proven-safe subset (positive-delta / provably-not-last-ref / no-destroy
// with a pre-mutation server guard, dar-1il.2 item 1 option B) is built and gated. See mach_traps.c
// _kernelrpc_mach_port_mod_refs_trap_impl.
//
// === PERMANENT MEMBERSHIP CANON (dar-1il.2) -- ALL must hold to add an op to THIS macro =========
// An op may ride the simple C2S ring ONLY if every one of these is true. This is the authoritative
// checklist; the prose above is the rationale. Adding an op that fails any of these has caused a
// real launchd wedge (deallocate, mod_refs).
//   1. NO caller S2C upcall. The op must never drive a server-to-client upcall (mmap/munmap/
//      mprotect/msync) to the CALLING thread -- a thread parked on a ring reply is not in recvmsg
//      and cannot service it -> deadlock. (This is SEPARATE from "non-blocking"; the fiber does not
//      save you.)
//   2. NO destroy-capable side effect. The op must not, for ANY argument combination, destroy a
//      port/right (last-ref drop, explicit destroy) -- destroying a mapped-region-backed object
//      triggers rule-1's munmap S2C. "Bookkeeping-only / create-or-ref" ops qualify; anything that
//      can tear down does not.
//   3. Safety decidable BEFORE mutation. The decision "is this call ring-safe?" must be answerable
//      from the request args alone, BEFORE taking ipc_space/any lock and BEFORE mutating state. If
//      safety only becomes knowable mid-mutation (e.g. after the refcount is read under the lock),
//      there is no valid preflight and the op is INELIGIBLE -- you cannot start, discover "oops,
//      need S2C", and roll back.
//   4. Opcode set in the ABI negotiation. The op participates in DSERVER_RING_C2S_OPCODES (which the
//      attach handshake hashes into c2s_opcode_hash); a build/version skew rejects the ring -> all
//      UDS, never a silent per-op drop.
//   5. SIDE-EFFECT A/B, not just return-code A/B. Correctness vs UDS must assert post-op namespace
//      STATE (right present/type/refcount, name reclamation, no garbage on error edges), not only a
//      byte-identical kern_return_t -- a transport bug can return the right code with the wrong
//      state.
//
// "WRONG LANE, NOT BAD OP" (the frame for ops that fail the canon): an op rejected here is not
// permanently UDS-doomed -- it is in the WRONG LANE. deallocate / mod_refs fail rules 1-3 because the
// SIMPLE fast ring has no way to deliver a caller-side S2C while the caller is parked. A future
// DUPLEX-ring lane that can accept caller-side S2C mid-call (the caller services upcalls while
// waiting for its reply) is the right home for destroy-capable ops. Until that lane exists they stay
// UDS; do NOT smuggle them back onto the simple ring via a "clever subset" that still risks an S2C.
//
// NOTE this is the TRANSPORT allowlist (Tier 1 = ride the ring via the generic fiber doWork); it is
// DISTINCT from the no-fiber inline fast path (ringFastPathEligible / Tier 2), a strictly smaller,
// separately-proven set. Never conflate the two.
#define DSERVER_RING_C2S_OPCODES(X) \
	X(task_self_trap) \
	X(thread_self_trap) \
	X(host_self_trap) \
	X(mach_reply_port) \
	X(mach_port_allocate) \
	X(mach_port_insert_right) \
	/* perf #18 D11 (dar-1il.6): bulk closed-fast Lane-1 batch, generic-fiber Tier-1 (NOT no-fiber). */ \
	/* Each is a CLOSED request->single-reply transaction with a FIXED inline request body and a FIXED */ \
	/* inline reply body (or header-only), no fd-passing, no caller-S2C, no destroy -- it passes all 5 */ \
	/* canon rules. The reply body (uidgid's old_uid/old_gid; started_suspended's bool; etc.) travels */ \
	/* over the ring verbatim via _publishReplyToRingLocked (everything after the reply hdr), so the */ \
	/* server needs NO per-op code: the generic eligible-op path rebuilds {callhdr,body}, dispatches via */ \
	/* callFromMessage -> doWork (the same dtape primitive as UDS), and the reply rides the ring. */ \
	X(uidgid) \
	X(set_thread_handles) \
	X(started_suspended) \
	X(get_tracer) \
	X(task_is_64_bit) \
	/* perf #18 D13 (dar-1il.8): Lane-1.5 "string/path" ops -- but NOT variable-payload. The path */ \
	/* bytes travel out-of-band via the server's process_vm_readv/writev on the guest's /proc/mem */ \
	/* (writeMemory/readMemory), so the RPC body is FIXED+tiny ({u64 buffer, u64 buffer_size} = 16B */ \
	/* request; {u64 length} or header-only reply) -- byte-identical shape class to the D11 batch, */ \
	/* NO arena needed. /proc/mem access is NOT a caller-S2C (the parked guest doesn't participate), */ \
	/* the ops are pure reads of server config written into the guest (no destroy) -> all 5 canon */ \
	/* rules hold. set_executable_path is deliberately NOT here (pre-attach mldr-only + NO_REPLY). */ \
	X(mldr_path) \
	X(vchroot_path)

// === perf #18 Phase A (dar-dar6x4-perf-5dq.30.1): THREE-LANE hybrid IPC taxonomy + guardrail ======
//
// plan.md ("Hybrid Ring Architecture Brief") froze the target as a MULTI-LANE IPC, not "a faster UDS
// for every RPC". Each op rides the CHEAPEST lane that preserves its semantics:
//   Lane 0  UDS            -- control/fallback: bootstrap, creds, fd-passing, unknown/complex ops,
//                             and destroy-capable / caller-S2C ops UNTIL a duplex lane proves them.
//   Lane 1  simple ring    -- CLOSED request->single-reply transactions, NO caller S2C while parked.
//                             Two sub-tiers (see below): generic-fiber (Tier 1) and no-fiber (Tier 2).
//   Lane 2  duplex ring    -- FUTURE reentrant lane for caller-S2C-capable ops (dar-1il.3). Not built.
//
// The lane an op MAY use is a set of orthogonal, independently-proven properties. Encode them as
// explicit bits so the code stops overloading "fast op" / "allowlist" / "ring op" (plan.md §6):
//
//   SimpleRingC2SEligible  -- may ride the Lane-1 simple ring (== membership in DSERVER_RING_C2S_OPCODES)
//   NoFiberFastEligible    -- may ALSO skip the generic fiber/Call machinery (Tier 2; ringFastPathEligible)
//   DuplexRingEligible     -- may ride the FUTURE Lane-2 duplex ring (no op qualifies yet)
//   UdsOnly                -- must stay on UDS for now (no ring lane is safe for it yet)
//   DestroyCapable         -- may, for SOME args, drop a last ref / tear down / cause a caller S2C
//   CallerS2CCapable       -- may synchronously upcall the CALLER (mmap/munmap/...) before its reply
//
// POLICY (these are the load-bearing relationships; the static guardrail below enforces #2/#3):
//   1. SimpleRingC2SEligible, NoFiberFastEligible, and DuplexRingEligible are DISTINCT concepts.
//      SimpleRingC2SEligible does NOT imply NoFiberFastEligible (Tier 2 is a strictly smaller proven
//      set). A future DuplexRingEligible op is NOT automatically SimpleRingC2SEligible.
//   2. DestroyCapable  => NOT SimpleRingC2SEligible. (canon rule 2: a teardown can drive a caller S2C.)
//   3. CallerS2CCapable => NOT SimpleRingC2SEligible. (canon rule 1: a parked caller can't service it.)
//   4. NoFiberFastEligible => SimpleRingC2SEligible. (Tier 2 is a sub-lane of Tier 1.)
//
// CLASSIFICATION TABLE: every Mach op that is RING-RELEVANT (already on a ring lane, or a known
// future/duplex candidate the canon has ruled on) is listed here with its lane-class bits, so the
// guardrail can mechanically cross-check the simple-ring set against the destroy/S2C prohibition.
// An op that never comes near a ring lane (the vast majority) need NOT be listed -- absence just
// means "unclassified == treated as UdsOnly by default". X is invoked as X(op, classbits).
#define DSERVER_RING_CLASS_SIMPLE_C2S   0x01u // SimpleRingC2SEligible: in DSERVER_RING_C2S_OPCODES
#define DSERVER_RING_CLASS_NOFIBER_FAST 0x02u // NoFiberFastEligible: Tier-2 no-fiber direct dispatch
#define DSERVER_RING_CLASS_DUPLEX       0x04u // DuplexRingEligible: future Lane-2 (none yet)
#define DSERVER_RING_CLASS_UDS_ONLY     0x08u // UdsOnly: no ring lane safe yet
#define DSERVER_RING_CLASS_DESTROY      0x10u // DestroyCapable: can tear down / drop last ref
#define DSERVER_RING_CLASS_CALLER_S2C   0x20u // CallerS2CCapable: can upcall the caller pre-reply

#define DSERVER_RING_OP_CLASS(X) \
	/* Lane 1 + Tier 2 (no-fiber): pure mint, never blocks, never an S2C. */ \
	X(task_self_trap,         DSERVER_RING_CLASS_SIMPLE_C2S | DSERVER_RING_CLASS_NOFIBER_FAST) \
	/* perf #18 D10 (dar-1il.5): thread_self_trap + host_self_trap join the pure-mint family. They */ \
	/* have a BYTE-IDENTICAL shape to task_self_trap (empty request, single uint32 port_name reply; */ \
	/* generate-rpc-wrappers.py:305-315) and the same processCall structure (dtape_*_self_trap mint + */ \
	/* _sendReply, call.cpp:617-625) -- pure mint via current_task()/current's space, never blocks, */ \
	/* never an S2C, never destroys. Chosen by the D9 heatmap (dar-1il.4) as the next migration: 100% */ \
	/* UDS today, caller_s2c=0, ~12% of workload RPC. Tier-2 no-fiber-safe exactly like task_self_trap. */ \
	X(thread_self_trap,       DSERVER_RING_CLASS_SIMPLE_C2S | DSERVER_RING_CLASS_NOFIBER_FAST) \
	X(host_self_trap,         DSERVER_RING_CLASS_SIMPLE_C2S | DSERVER_RING_CLASS_NOFIBER_FAST) \
	X(mach_reply_port,        DSERVER_RING_CLASS_SIMPLE_C2S | DSERVER_RING_CLASS_NOFIBER_FAST) \
	/* Lane 1 Tier 1 (generic fiber): create/ref bookkeeping, no teardown, no caller S2C. */ \
	X(mach_port_allocate,     DSERVER_RING_CLASS_SIMPLE_C2S) \
	X(mach_port_insert_right, DSERVER_RING_CLASS_SIMPLE_C2S) \
	/* perf #18 D11 (dar-1il.6): bulk closed-fast Lane-1 batch -- SIMPLE_C2S ONLY (Tier-1 generic fiber, */ \
	/* NOT NoFiberFast). These read/mutate process or thread state (creds, pthread handles, suspend/tracer */ \
	/* flags, 64-bitness) via the generic Call path, NOT pure mint, so they are NOT no-fiber-safe like the */ \
	/* self-trap family -- they stay Tier 1. None is destroy-capable or caller-S2C, so the canon holds. */ \
	X(uidgid,                 DSERVER_RING_CLASS_SIMPLE_C2S) \
	X(set_thread_handles,     DSERVER_RING_CLASS_SIMPLE_C2S) \
	X(started_suspended,      DSERVER_RING_CLASS_SIMPLE_C2S) \
	X(get_tracer,             DSERVER_RING_CLASS_SIMPLE_C2S) \
	X(task_is_64_bit,         DSERVER_RING_CLASS_SIMPLE_C2S) \
	/* perf #18 D13 (dar-1il.8): path ops -- SIMPLE_C2S Tier-1 (generic fiber). Fixed 16B request / */ \
	/* 8B-or-header reply; the path itself rides /proc/mem (process_vm_readv/writev), not the payload, */ \
	/* so no arena and no caller-S2C. Pure read of server config -> not destroy-capable. */ \
	X(mldr_path,              DSERVER_RING_CLASS_SIMPLE_C2S) \
	X(vchroot_path,           DSERVER_RING_CLASS_SIMPLE_C2S) \
	/* UDS-only today: destroy-capable -> caller-S2C deadlock on the simple ring. The right home is */ \
	/* the future duplex lane (dar-1il.3) -- "wrong lane, not bad op". They are DuplexRingEligible */ \
	/* CANDIDATES but NOT yet proven, so they carry UDS_ONLY until Phase D/E lands. */ \
	X(mach_port_deallocate,   DSERVER_RING_CLASS_UDS_ONLY | DSERVER_RING_CLASS_DESTROY | DSERVER_RING_CLASS_CALLER_S2C) \
	X(mach_port_mod_refs,     DSERVER_RING_CLASS_UDS_ONLY | DSERVER_RING_CLASS_DESTROY | DSERVER_RING_CLASS_CALLER_S2C)

// dserver_ring_op_class(): the lane-class bits for a callnum, or 0 (== unclassified, treat as UdsOnly)
// if the op is not in the table. Only defined where rpc.h (the callnum source) is in scope, like the
// opcode hash; the server build always has it. Header-only; pure chain of integer comparisons, no
// allocation. constexpr under C++ (so it folds inside the static_assert below); plain inline in C.
#ifdef _DARLINGSERVER_API_H_
#if defined(__cplusplus)
#define DSERVER_RING_OPCLASS_LINKAGE constexpr
#else
#define DSERVER_RING_OPCLASS_LINKAGE static inline
#endif
DSERVER_RING_OPCLASS_LINKAGE uint32_t dserver_ring_op_class(uint32_t callnum) {
	// RED-ARM HOOK (gate-only, never defined in a real build): when the lane-class gate compiles this
	// header with -DDSERVER_RING_LANECLASS_RED_DESTROY_IN_RING, OR the DestroyCapable bit onto a
	// genuine simple-ring member (mach_port_allocate). That makes dserver_ring_c2s_set_is_canon_safe()
	// false and the static_assert below FAIL TO COMPILE -- proving the compile-time guardrail is live,
	// not vacuous. run-ring-shm-validate.sh asserts this arm fails to build.
#ifdef DSERVER_RING_LANECLASS_RED_DESTROY_IN_RING
	if (callnum == (uint32_t)dserver_callnum_mach_port_allocate)
		return DSERVER_RING_CLASS_SIMPLE_C2S | DSERVER_RING_CLASS_DESTROY;
#endif
#define DSERVER_RING_OP_CLASS_CASE(op, bits) if (callnum == (uint32_t)dserver_callnum_##op) return (bits);
	DSERVER_RING_OP_CLASS(DSERVER_RING_OP_CLASS_CASE)
#undef DSERVER_RING_OP_CLASS_CASE
	return 0u;
}

// === STATIC GUARDRAIL (plan.md §17.3): a destroy-capable / caller-S2C op CANNOT enter the simple =====
// ring set. Cross-check, at COMPILE TIME on the server (and at runtime in the C guest mirror gate),
// that every op the table marks SimpleRingC2SEligible is (a) actually in DSERVER_RING_C2S_OPCODES and
// (b) NOT marked DestroyCapable or CallerS2CCapable, and conversely that every op in
// DSERVER_RING_C2S_OPCODES is classified SimpleRingC2SEligible. This makes canon rules 1-2 a build
// error, not a review checklist: re-adding deallocate/mod_refs to the macro (or mis-tagging a real
// simple-ring op as destroy-capable) fails to compile. The bead's RED arms exercise exactly that.
//
// Property folded to a single bool over the WHOLE simple-ring set: each member must be classified
// SIMPLE_C2S and must carry NEITHER destroy NOR caller-S2C bit. (We can only check the C2S set here,
// not the reverse "every SIMPLE_C2S-tagged op is in the macro" -- the macro is the set of record; a
// tag claiming SIMPLE_C2S on an op the macro omits is a documentation slip, caught by the gate test.)
#define DSERVER_RING_C2S_MEMBER_IS_SAFE(op) \
	&& ((dserver_ring_op_class((uint32_t)dserver_callnum_##op) & DSERVER_RING_CLASS_SIMPLE_C2S) != 0u) \
	&& ((dserver_ring_op_class((uint32_t)dserver_callnum_##op) & (DSERVER_RING_CLASS_DESTROY | DSERVER_RING_CLASS_CALLER_S2C)) == 0u)
// Constant-foldable predicate (true iff the whole simple-ring set obeys the canon). Used by the
// static_assert (C++) and the guest mirror gate (C). RED arms flip a member's class to break it.
// constexpr under C++ so it is usable in static_assert; plain inline in C.
DSERVER_RING_OPCLASS_LINKAGE int dserver_ring_c2s_set_is_canon_safe(void) {
	return (1 DSERVER_RING_C2S_OPCODES(DSERVER_RING_C2S_MEMBER_IS_SAFE)) ? 1 : 0;
}
#if defined(__cplusplus) && !defined(DSERVER_RING_NO_LANECLASS_ASSERT)
// Compile-time enforcement in the server build. dserver_ring_op_class is a pure constexpr-foldable
// chain of integer comparisons over compile-time-constant callnums, so the whole fold is a constant
// expression; a violating classification makes this assertion fail to compile.
static_assert(dserver_ring_c2s_set_is_canon_safe(),
	"perf#18 canon: a DSERVER_RING_C2S_OPCODES member is classified DestroyCapable/CallerS2CCapable "
	"(or not SimpleRingC2SEligible) in DSERVER_RING_OP_CLASS -- a destroy-capable/caller-S2C op must "
	"NOT ride the simple ring (it deadlocks a parked caller on an S2C upcall). See the membership canon "
	"above; such ops belong on the future duplex lane (dar-1il.3), not Lane 1.");
#endif
#endif // _DARLINGSERVER_API_H_

// --- C2S opcode-set ABI tie (perf #18 dar-1il.2 item 2) ---------------------------------------
//
// The shared X-macro above makes the guest and server allowlists drift-proof ONLY when both are
// compiled from THIS header. An OLD server + NEW guest (or vice versa), built from DIFFERENT
// rpc-supplement.h revisions, can still disagree on the set -- and that disagreement is the exact
// silent-drop wedge the macro exists to prevent (guest publishes an op the server's older allowlist
// drops -> guest stranded on its bounded reply-wait). Source-level sharing cannot catch a version
// skew across two separately-built binaries; the ATTACH HANDSHAKE can.
//
// So fold the C2S opcode set into a 64-bit hash that BOTH sides compute from their OWN compiled
// DSERVER_RING_C2S_OPCODES, carry it in the control block (c2s_opcode_hash), and have the server
// REJECT the ring at attach (-> dserver_ring_reject_opcode_set -> the thread falls back to UDS for
// EVERYTHING, never a silent per-op drop) if the guest's hash != the server's. A skewed pair thus
// degrades cleanly to all-UDS instead of wedging on the first divergent op. This generalizes the
// no-silent-drop invariant across binary/version skew, not just same-build source equality.
//
// FNV-1a over the little-endian callnum bytes, in macro-expansion order. Plain compile-time-foldable
// arithmetic; identical on guest (libc-free C) and server (C++). Order-sensitive, which is fine: the
// macro defines one canonical order and both sides expand the SAME macro. (A reorder of the macro is
// itself a set change we want to surface.) Folded away with the rest of the ring behind the guard.
//
// This fold references dserver_callnum_* values, which live in the GENERATED rpc.h -- the one header
// this supplement is otherwise independent of. So it is only defined when the consumer has ALREADY
// included rpc.h (detected via its guard _DARLINGSERVER_API_H_). Every real consumer that needs the
// hash (the guest ring attach, the server validator, the gates) includes rpc.h; a consumer that only
// wants the wake predicates / service loop (e.g. ring_wake_predicates_test) includes neither the
// hash nor anything that calls it, and still compiles. The server build ALWAYS has rpc.h in scope,
// so the validator's opcode-set check below is always active in production.
#define DSERVER_RING_OPCODE_HASH_FNV_OFFSET 1469598103934665603ull
#define DSERVER_RING_OPCODE_HASH_FNV_PRIME  1099511628211ull
#ifdef _DARLINGSERVER_API_H_
static inline uint64_t dserver_ring_c2s_opcode_hash(void) {
	uint64_t h = DSERVER_RING_OPCODE_HASH_FNV_OFFSET;
#define DSERVER_RING_OPCODE_HASH_FOLD(op) \
	do { \
		uint32_t _cn = (uint32_t)dserver_callnum_##op; \
		for (int _b = 0; _b < 4; ++_b) { \
			h ^= (uint64_t)((_cn >> (8 * _b)) & 0xffu); \
			h *= DSERVER_RING_OPCODE_HASH_FNV_PRIME; \
		} \
	} while (0);
	DSERVER_RING_C2S_OPCODES(DSERVER_RING_OPCODE_HASH_FOLD)
#undef DSERVER_RING_OPCODE_HASH_FOLD
	return h;
}
#endif // _DARLINGSERVER_API_H_

// Prefix of every REPLY payload: the RPC result code (what the UDS reply header.code carries),
// followed by the call's reply body. Kept separate from dserver_ring_slot so the slot stays a
// pure transport header and the payload stays a faithful copy of the UDS reply body.
typedef struct dserver_ring_reply_hdr {
	int32_t code; // RPC result code (0 == success); mirrors dserver_rpc_replyhdr_t.code
} dserver_ring_reply_hdr_t;

// The control block at the head of the shared mapping. The guest fills this in once before
// handing the memfd to the server; the server treats every field as adversarial input.
typedef struct dserver_ring_shm {
	uint32_t magic;        // must == DSERVER_RING_MAGIC
	uint16_t abi_version;  // must == DSERVER_RING_ABI_VERSION
	uint16_t slot_size;    // [MIN,MAX]_SLOT_SIZE; >= sizeof(dserver_ring_slot)
	uint32_t slot_count;   // power of two in [MIN,MAX]_SLOT_COUNT
	uint32_t arena_off;    // byte offset of the arena within the mapping (0 if none)
	uint32_t arena_size;   // arena bytes (0 if none)
	uint32_t c2s_ring_off; // offset of the request ring (guest->server)
	uint32_t s2c_ring_off; // offset of the reply ring (server->guest)
	uint32_t total_size;   // full mapping size; server cross-checks vs the fd's real size
	int32_t  guest_tid;    // nsid the guest claims; server cross-checks vs SCM credentials
	// perf #18 dar-1il.2 item 2: the guest's compiled C2S opcode-set hash
	// (dserver_ring_c2s_opcode_hash()). The server rejects the ring at attach if it != the server's
	// own hash, so a guest/server pair built from a different DSERVER_RING_C2S_OPCODES degrades to
	// all-UDS instead of wedging on the first op the older side doesn't allowlist. 8-byte aligned (it
	// follows guest_tid at offset 36; the struct's natural alignment pads to 40 before this 8-byte
	// field -> offset 40). Placed before the cache-line-aligned wake words so it doesn't perturb them.
	uint64_t c2s_opcode_hash;
	// Futex wake words. Plain uint32_t storage so the struct is a portable C/C++ wire format;
	// atomicity is at the ACCESS site (the producer/consumer use atomic load/store + FUTEX_*),
	// not in the storage type. Each on its own cache line so the two directions don't
	// false-share. (head/tail in dserver_ring carry the actual ring state; these only gate
	// sleep/wake.)
	DSERVER_RING_ALIGN64 uint32_t c2s_futex; // guest bumps to wake the server (via a registered eventfd)
	DSERVER_RING_ALIGN64 uint32_t s2c_futex; // server bumps to wake the guest
	// perf #18 P4 wake-model words. server_state is written ONLY by the server and read by the
	// guest (conditional doorbell). s2c_waiters is written ONLY by the guest (1 just before it
	// FUTEX_WAITs on s2c_futex, 0 after it wakes) and read by the server (conditional FUTEX_WAKE):
	// the server skips the wake syscall entirely while no guest is parked. Each is the producer's
	// to write and the peer's to read -- never a shared RMW -- so plain release/acquire suffices;
	// a stale read only ever causes a redundant wake or doorbell, never a lost one. Own cache
	// lines so the two directions don't false-share with each other or with the futex words.
	DSERVER_RING_ALIGN64 uint32_t server_state; // server-written: DSERVER_RING_SRV_* (guest reads for conditional doorbell)
	DSERVER_RING_ALIGN64 uint32_t s2c_waiters;  // guest-written: nonzero == a guest is parked in FUTEX_WAIT on s2c_futex
	// --- perf #18 P8 D1/D2 (dar-1il.3.1): DUPLEX MAILBOX (ABI v4) -------------------------------
	// The minimal duplex lane: ONE outstanding S2C upcall to a ring-parked caller + its reply, carried
	// in two fixed mailbox slots in the control block (NOT new rings -- the Lane-1 rings stay
	// byte-identical). Each slot has a `ready` flag the producer release-stores LAST (after the body),
	// so a peer that observes ready==1 sees a fully written slot (torn-write-safe, same discipline as
	// the ring tail publish). The guest reuses s2c_futex/s2c_waiters to park (it watches BOTH the s2c
	// reply ring AND this mailbox while parked -- the duplex wake model, proven in ring_duplex_wake_
	// gate_test.c). Correlation: an upcall reply is accepted ONLY if BOTH parent_id and upcall_id match
	// the in-flight upcall. Own cache lines so the duplex traffic never false-shares with Lane-1.
	DSERVER_RING_ALIGN64 uint32_t duplex_caps;          // guest-written at attach: DSERVER_RING_DUPLEX_CAP_* the guest supports
	// S2C upcall mailbox (server -> guest). Server writes body then release-stores upcall_ready=1.
	DSERVER_RING_ALIGN64 uint32_t duplex_upcall_ready;  // 1 == an upcall is published and unhandled (server->guest)
	uint32_t duplex_upcall_op;                          // DSERVER_RING_DUPLEX_UPCALL_* (which S2C shape)
	uint32_t duplex_upcall_parent;                      // parent_id this upcall belongs to (correlation)
	uint32_t duplex_upcall_id;                          // unique upcall id (correlation)
	uint32_t duplex_upcall_arg;                         // single inline arg for the minimal echo shape
	// perf #18 P8 D4: typed payload for a REAL S2C upcall (the munmap shape). For ECHO these are unused.
	// For MUNMAP: duplex_upcall_addr = address, duplex_upcall_len = length (the munmap(2) args).
	uint64_t duplex_upcall_addr;                        // munmap address (DSERVER_RING_DUPLEX_UPCALL_MUNMAP)
	uint64_t duplex_upcall_len;                         // munmap length  (DSERVER_RING_DUPLEX_UPCALL_MUNMAP)
	// C2S upcall reply mailbox (guest -> server). Guest writes body then release-stores reply_ready=1.
	DSERVER_RING_ALIGN64 uint32_t duplex_reply_ready;   // 1 == an upcall reply is published (guest->server)
	uint32_t duplex_reply_parent;                       // echoes the upcall's parent_id (server checks)
	uint32_t duplex_reply_id;                           // echoes the upcall's upcall_id (server checks)
	int32_t  duplex_reply_status;                       // guest's upcall result (0 == ok). For MUNMAP this is
	                                                    //   the munmap return_value (0 ok / -1 error).
	uint32_t duplex_reply_arg;                          // the echo result the guest computed (ECHO shape)
	// perf #18 P8 D4: the errno from a real S2C upcall (MUNMAP). For ECHO this is unused (0).
	int32_t  duplex_reply_errno;                        // munmap errno_result (0 on success)
} dserver_ring_shm_t;

// --- P4 wake-model decision predicates (shared by guest, server, and the host gate) ----------
//
// These are the entire wake model expressed as two pure functions, so the guest (libc-free C),
// the server (C++), and the host A/B gate all make the IDENTICAL decision from the same control
// block. The whole P4 win is: on the hot path BOTH of these return 0, so neither side enters the
// kernel. The cold path (server parked in epoll / guest parked in futex) makes them return 1 and
// the doorbell / FUTEX_WAKE happen exactly as in v0. Defined after dserver_ring_shm_t so they can
// dereference it.
//
// dserver_ring_guest_should_doorbell(): the guest just published a request. It must poke the
// server's wake eventfd ONLY if the server is not currently draining rings. While ACTIVE_POLLING
// the server will observe the new c2s tail on its own, so the doorbell is pure overhead and is
// skipped. SLEEP_ARMED and SLEEPING_EPOLL (and any unknown value) -> doorbell, because the server
// either is asleep or is in the arm/sleep race window and may miss the publish.
static inline int dserver_ring_guest_should_doorbell(const dserver_ring_shm_t* cb) {
	uint32_t st = __atomic_load_n(&cb->server_state, __ATOMIC_ACQUIRE);
	return st != DSERVER_RING_SRV_ACTIVE_POLLING;
}

// dserver_ring_server_should_wake(): the server just published a reply. It must FUTEX_WAKE the
// guest ONLY if a guest is actually parked (s2c_waiters != 0). A guest that is still spinning on
// the reply has s2c_waiters == 0 and will see the new s2c tail without a syscall, so the wake is
// skipped. The guest sets the bit BEFORE its final pre-sleep recheck and the server publishes the
// reply BEFORE reading the bit, so the bit can only be falsely-0 if the guest hasn't slept yet
// (in which case it's spinning and needs no wake) -- never a lost wakeup.
static inline int dserver_ring_server_should_wake(const dserver_ring_shm_t* cb) {
	uint32_t w = __atomic_load_n(&cb->s2c_waiters, __ATOMIC_ACQUIRE);
	return w != 0u;
}

// --- perf #18 P8 D1/D2: minimal duplex mailbox helpers (shared by guest, server, host test) -------
//
// These are the entire minimal duplex protocol expressed as pure functions over the mailbox words, so
// the guest (libc-free C), the server (C++), and the live host roundtrip test all use the IDENTICAL
// publish/observe/correlate logic. ONE upcall outstanding at a time; the `ready` flag is the
// release-store linearization point (written LAST, after the body) so an observer of ready==1 sees a
// fully written slot.

// Server: publish ONE S2C upcall into the mailbox. Body first, then release-store ready=1.
static inline void dserver_ring_duplex_publish_upcall(
	dserver_ring_shm_t* cb, uint32_t op, uint32_t parent_id, uint32_t upcall_id, uint32_t arg) {
	cb->duplex_upcall_op     = op;
	cb->duplex_upcall_parent = parent_id;
	cb->duplex_upcall_id     = upcall_id;
	cb->duplex_upcall_arg    = arg;
	__atomic_store_n(&cb->duplex_upcall_ready, 1u, __ATOMIC_RELEASE); // publish (linearization point)
}

// perf #18 P8 D4: server publishes a REAL munmap S2C upcall. Same linearization discipline as the echo
// publish (body first, ready release-stored LAST), but carries the typed addr/len payload. The op field
// tells the guest pump to run munmap(addr,len) instead of the echo transform.
static inline void dserver_ring_duplex_publish_munmap_upcall(
	dserver_ring_shm_t* cb, uint32_t parent_id, uint32_t upcall_id, uint64_t address, uint64_t length) {
	cb->duplex_upcall_op     = DSERVER_RING_DUPLEX_UPCALL_MUNMAP;
	cb->duplex_upcall_parent = parent_id;
	cb->duplex_upcall_id     = upcall_id;
	cb->duplex_upcall_arg    = 0;
	cb->duplex_upcall_addr   = address;
	cb->duplex_upcall_len    = length;
	__atomic_store_n(&cb->duplex_upcall_ready, 1u, __ATOMIC_RELEASE); // publish (linearization point)
}

// Guest pump: is an S2C upcall available? (acquire so the body is visible once ready is seen)
static inline int dserver_ring_duplex_upcall_available(const dserver_ring_shm_t* cb) {
	return __atomic_load_n(&cb->duplex_upcall_ready, __ATOMIC_ACQUIRE) != 0u;
}

// Guest pump: publish the reply to the current upcall, then clear the upcall-ready flag (consume).
// Echoes parent_id+upcall_id for the server's correlation check. Body first, ready last.
static inline void dserver_ring_duplex_publish_reply(
	dserver_ring_shm_t* cb, uint32_t parent_id, uint32_t upcall_id, int32_t status, uint32_t result_arg) {
	cb->duplex_reply_parent = parent_id;
	cb->duplex_reply_id     = upcall_id;
	cb->duplex_reply_status = status;
	cb->duplex_reply_arg    = result_arg;
	// consume the upcall slot (we've handled it) BEFORE advertising the reply, so the server never
	// sees reply_ready while upcall_ready is still set.
	__atomic_store_n(&cb->duplex_upcall_ready, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->duplex_reply_ready, 1u, __ATOMIC_RELEASE); // publish reply (linearization point)
}

// perf #18 P8 D4: guest publishes a REAL munmap upcall reply (carries the kernel return_value as status
// and the errno). Same ordering as the echo reply: body first, consume the upcall, then release-store
// reply_ready LAST. return_value 0 == success; on error return_value == -1 and errno_result == -errno.
static inline void dserver_ring_duplex_publish_munmap_reply(
	dserver_ring_shm_t* cb, uint32_t parent_id, uint32_t upcall_id, int32_t return_value, int32_t errno_result) {
	cb->duplex_reply_parent = parent_id;
	cb->duplex_reply_id     = upcall_id;
	cb->duplex_reply_status = return_value;
	cb->duplex_reply_arg    = 0;
	cb->duplex_reply_errno  = errno_result;
	__atomic_store_n(&cb->duplex_upcall_ready, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&cb->duplex_reply_ready, 1u, __ATOMIC_RELEASE); // publish reply (linearization point)
}

// Server: is the upcall reply available AND correlated to the upcall we sent? Accept ONLY if BOTH
// parent_id and upcall_id match -- a mismatch is a protocol error (the server must NOT resume on it).
// Returns 1 (accept) only when ready && correlated; 0 otherwise. `*out_mismatch` is set if a reply is
// ready but mis-correlated (so the caller can flag the protocol error rather than silently spin).
static inline int dserver_ring_duplex_reply_ready(
	const dserver_ring_shm_t* cb, uint32_t parent_id, uint32_t upcall_id, int* out_mismatch) {
	if (out_mismatch) *out_mismatch = 0;
	if (__atomic_load_n(&cb->duplex_reply_ready, __ATOMIC_ACQUIRE) == 0u) return 0;
	if (cb->duplex_reply_parent == parent_id && cb->duplex_reply_id == upcall_id) return 1;
	if (out_mismatch) *out_mismatch = 1; // ready but wrong correlation -> protocol error
	return 0;
}

// Server: consume the reply slot after accepting it.
static inline void dserver_ring_duplex_consume_reply(dserver_ring_shm_t* cb) {
	__atomic_store_n(&cb->duplex_reply_ready, 0u, __ATOMIC_RELEASE);
}

// The minimal supported upcall shape: ECHO. The guest computes result = arg ^ 0x5A5A5A5A on the
// CALLER thread (a cheap, deterministic, side-effect-free transform that proves the upcall ran in the
// caller's context -- the guest can also assert its own tid here). Shared so server + test agree.
static inline uint32_t dserver_ring_duplex_echo_transform(uint32_t arg) {
	return arg ^ 0x5A5A5A5Au;
}

// Why a ring control block was rejected. Returned by dserver_ring_shm_validate(); the server
// logs it once and falls the thread back to UDS. (0 == accepted.)
typedef enum dserver_ring_reject {
	dserver_ring_ok = 0,
	dserver_ring_reject_magic,
	dserver_ring_reject_abi,
	dserver_ring_reject_slot_size,
	dserver_ring_reject_slot_count,      // not in range, or not a power of two
	dserver_ring_reject_total_size,      // out of [hdr, MAX], or != the fd's real size
	dserver_ring_reject_ring_bounds,     // a ring's [off, off + header + slots) escapes the mapping
	dserver_ring_reject_ring_overlap,    // c2s and s2c rings (or the header) overlap
	dserver_ring_reject_arena_bounds,    // arena [off, off+size) escapes the mapping
	dserver_ring_reject_arena_overlap,   // arena overlaps a ring or the header
	dserver_ring_reject_tid,             // guest_tid != the SCM-credentialed nsid
	dserver_ring_reject_opcode_set,      // c2s_opcode_hash != the server's (guest/server C2S allowlists differ across build/version skew -> would silent-drop; reject -> all-UDS)
} dserver_ring_reject_t;

// --- Server-side C2S service loop (pure, bounds-safe) ---------------------------------
//
// Drain published request slots from the c2s ring, hand each to a servicer callback, and
// publish the reply onto the s2c ring. ALL of this is attacker-exposed: the guest owns the
// c2s tail and the slot bodies. We re-validate every slot before trusting it -- callnum is
// left to the servicer (it reuses call.cpp's switch), but length/seq/arena are clamped here.
//
// The servicer computes a reply: it receives the request body (already bounds-checked to lie
// within one slot) and writes a reply body + an RPC code. It returns 0 on "handled" or
// non-zero on "refuse this callnum on the ring" (the loop then drops the request and the
// guest will time out + fall back to UDS for that op). The servicer NEVER sees a pointer into
// guest memory beyond the single validated slot payload.
//
// Returns the number of requests serviced (0 if the ring was empty or every slot was corrupt).
// `*out_woke` is set nonzero if at least one reply was published (caller should wake the guest).
typedef int (*dserver_ring_servicer_t)(
	void* ctx,
	uint32_t callnum,
	const void* req_body, uint32_t req_len,
	void* reply_body, uint32_t reply_cap, uint32_t* out_reply_len,
	int32_t* out_code
);

static inline uint32_t dserver_ring_service_c2s(
	dserver_ring_t* c2s, dserver_ring_t* s2c,
	uint32_t slot_size, uint32_t slot_count,
	dserver_ring_servicer_t servicer, void* ctx,
	int* out_woke
) {
	uint32_t serviced = 0;
	uint32_t inline_cap = slot_size - (uint32_t)sizeof(dserver_ring_slot_t);
	if (out_woke) *out_woke = 0;

	for (;;) {
		dserver_ring_slot_t* req = dserver_ring_consumer_begin(c2s, slot_size, slot_count);
		if (!req) {
			break; // empty or corrupt tail -> stop (corrupt is handled defensively by begin())
		}

		// Copy the slot's transport header out before trusting it -- the guest can mutate the
		// page concurrently. Then bounds-check the inline request body strictly.
		uint32_t callnum = req->callnum;
		uint32_t reqlen  = req->length;
		uint32_t seq     = req->seq;
		const void* req_body = (const char*)req + sizeof(dserver_ring_slot_t);
		if (reqlen > inline_cap) {
			// malformed: claimed body exceeds the slot. Drop it (consume so we don't spin) and
			// keep going; the guest op will UDS-fall-back on timeout.
			dserver_ring_consumer_advance(c2s);
			continue;
		}

		// Produce the reply into an s2c slot. If the reply ring is full we stop (leave the
		// request unconsumed so we retry it on the next wake -- backpressure, not loss).
		dserver_ring_slot_t* rep = dserver_ring_producer_begin(s2c, slot_size, slot_count);
		if (!rep) {
			break;
		}
		char* rep_payload = (char*)rep + sizeof(dserver_ring_slot_t);
		uint32_t rep_cap = inline_cap;
		if (rep_cap < (uint32_t)sizeof(dserver_ring_reply_hdr_t)) {
			// slot too small to even carry a reply header -- impossible after validate(), but
			// be defensive: drop the request.
			dserver_ring_consumer_advance(c2s);
			continue;
		}
		dserver_ring_reply_hdr_t* rhdr = (dserver_ring_reply_hdr_t*)rep_payload;
		void* rep_body = rep_payload + sizeof(dserver_ring_reply_hdr_t);
		uint32_t rep_body_cap = rep_cap - (uint32_t)sizeof(dserver_ring_reply_hdr_t);
		uint32_t rep_body_len = 0;
		int32_t code = 0;

		int handled = servicer(ctx, callnum, req_body, reqlen, rep_body, rep_body_cap, &rep_body_len, &code);

		// We are done reading the request slot now; free it.
		dserver_ring_consumer_advance(c2s);

		if (handled != 0) {
			// servicer refused this callnum on the ring; don't publish a reply (guest UDS-falls
			// back). Roll back the reply slot we hadn't published yet -- producer_begin didn't
			// move tail, so simply not publishing is the rollback.
			continue;
		}

		if (rep_body_len > rep_body_cap) {
			rep_body_len = rep_body_cap; // servicer bug guard; never overruns the slot
		}
		rhdr->code = code;
		rep->callnum = callnum;
		rep->seq = seq;
		rep->length = (uint32_t)sizeof(dserver_ring_reply_hdr_t) + rep_body_len;
		rep->arena_off = 0;
		rep->arena_len = 0;
		rep->flags = (code != 0) ? DSERVER_RING_FLAG_REPLY_ERROR : 0u;
		dserver_ring_producer_publish(s2c);
		if (out_woke) *out_woke = 1;
		++serviced;
	}

	return serviced;
}

// Pure trust-boundary validator. Takes a COPY of the guest's control block (copied in by the
// caller -- never validate in place, the guest can mutate it concurrently) plus the mapping's
// real size (from fstat of the memfd) and the SCM-credentialed nsid. Returns dserver_ring_ok
// or the first reason it failed. Dereferences NOTHING -- arithmetic only -- so it is safe to
// run on fully attacker-controlled input. Uint64 math throughout so 32-bit fields can't wrap.
static inline dserver_ring_reject_t dserver_ring_shm_validate(
	const dserver_ring_shm_t* cb, // a server-owned COPY, not the shared page
	uint64_t real_mapping_size,   // fstat size of the memfd the guest passed
	int32_t  scm_nsid             // the thread nsid from the kernel, not from the guest
) {
	if (cb->magic != DSERVER_RING_MAGIC) return dserver_ring_reject_magic;
	if (cb->abi_version != DSERVER_RING_ABI_VERSION) return dserver_ring_reject_abi;

	uint64_t slot_size = cb->slot_size;
	if (slot_size < DSERVER_RING_MIN_SLOT_SIZE || slot_size > DSERVER_RING_MAX_SLOT_SIZE) return dserver_ring_reject_slot_size;
	if (slot_size < sizeof(dserver_ring_slot_t)) return dserver_ring_reject_slot_size;

	uint64_t slot_count = cb->slot_count;
	if (slot_count < DSERVER_RING_MIN_SLOT_COUNT || slot_count > DSERVER_RING_MAX_SLOT_COUNT) return dserver_ring_reject_slot_count;
	if ((slot_count & (slot_count - 1)) != 0) return dserver_ring_reject_slot_count; // power of two

	uint64_t total = cb->total_size;
	uint64_t hdr = sizeof(dserver_ring_shm_t);
	if (total < hdr || total > DSERVER_RING_MAX_TOTAL_SIZE) return dserver_ring_reject_total_size;
	if (total != real_mapping_size) return dserver_ring_reject_total_size; // the fd must be exactly this big

	// each ring spans [off, off + sizeof(header) + slot_count*slot_size)
	uint64_t ring_span = sizeof(dserver_ring_t) + slot_count * slot_size;
	uint64_t c2s_lo = cb->c2s_ring_off, c2s_hi = c2s_lo + ring_span;
	uint64_t s2c_lo = cb->s2c_ring_off, s2c_hi = s2c_lo + ring_span;
	if (c2s_lo < hdr || c2s_hi > total) return dserver_ring_reject_ring_bounds;
	if (s2c_lo < hdr || s2c_hi > total) return dserver_ring_reject_ring_bounds;
	// rings must not overlap each other
	if (c2s_lo < s2c_hi && s2c_lo < c2s_hi) return dserver_ring_reject_ring_overlap;

	if (cb->arena_size != 0) {
		uint64_t a_lo = cb->arena_off, a_hi = a_lo + cb->arena_size;
		if (a_lo < hdr || a_hi > total) return dserver_ring_reject_arena_bounds;
		// arena must not overlap either ring
		if (a_lo < c2s_hi && c2s_lo < a_hi) return dserver_ring_reject_arena_overlap;
		if (a_lo < s2c_hi && s2c_lo < a_hi) return dserver_ring_reject_arena_overlap;
	}

	if (cb->guest_tid != scm_nsid) return dserver_ring_reject_tid;

	// perf #18 dar-1il.2 item 2: the guest's C2S opcode-set hash MUST match the server's. A mismatch
	// means the two were built from different DSERVER_RING_C2S_OPCODES (binary/version skew the shared
	// macro can't catch), so some op one side routes the other would silently drop -> reject the whole
	// ring here -> the thread uses UDS for everything (no silent per-op drop, no wedge). Checked last
	// because it's the most "semantic" of the trust-boundary checks; arithmetic-only, dereferences
	// nothing beyond the server-owned copy. Active whenever rpc.h (the callnum source) is in scope --
	// which it always is in the server build; see dserver_ring_c2s_opcode_hash above.
#ifdef _DARLINGSERVER_API_H_
	if (cb->c2s_opcode_hash != dserver_ring_c2s_opcode_hash()) return dserver_ring_reject_opcode_set;
#endif

	return dserver_ring_ok;
}

// Server-side ring_attach decision: given the memfd the guest passed, its claimed size, and
// the SCM-credentialed nsid, decide whether to accept the ring. This is the bridge between
// the pure validator above and real kernel state -- it fstats the fd for its TRUE size
// (never trusting the guest's mapping_size), maps it read-only just to read the control
// block, copies the control block OUT (so validation runs on a stable server-owned copy, not
// the page the guest can mutate), validates, and unmaps. On success it hands back the real
// size and the validated control-block copy via out-params so the caller can keep the
// mapping; on any failure it returns the reject reason and maps nothing lasting.
//
// Header-only + dependency-free (only <sys/mman.h>, <sys/stat.h>, <unistd.h>, <string.h>) so
// the adversarial test drives it with a real memfd, exactly as the server will. Returns
// dserver_ring_ok or a reject reason; *out_real_size / *out_cb are written only on ok.
#if defined(__linux__) && !defined(DSERVER_RING_NO_ATTACH_CHECK)

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static inline dserver_ring_reject_t dserver_ring_attach_check(
	int ring_fd,             // the memfd the guest sent via @fd
	uint64_t claimed_size,   // mapping_size the guest claimed in the RPC (cross-checked, not trusted)
	int32_t scm_nsid,        // the thread nsid from the kernel
	uint64_t* out_real_size, // [out] the fstat size, written on ok
	dserver_ring_shm_t* out_cb // [out] validated copy of the control block, written on ok
) {
	struct stat st;
	if (fstat(ring_fd, &st) != 0) return dserver_ring_reject_total_size;
	uint64_t real_size = (uint64_t)st.st_size;

	// the guest's claimed size must match the real fd size (a lie is a protocol violation),
	// and must be at least the control block + within the hard cap before we even map it.
	if (claimed_size != real_size) return dserver_ring_reject_total_size;
	if (real_size < sizeof(dserver_ring_shm_t) || real_size > DSERVER_RING_MAX_TOTAL_SIZE) return dserver_ring_reject_total_size;

	// map read-only just to read the header; the real attach (caller) will map RW after ok.
	void* map = mmap(NULL, sizeof(dserver_ring_shm_t), PROT_READ, MAP_SHARED, ring_fd, 0);
	if (map == MAP_FAILED) return dserver_ring_reject_total_size;

	// copy the control block out of the shared page IMMEDIATELY -- never validate in place,
	// the guest can race a write between any two reads.
	dserver_ring_shm_t cb;
	memcpy(&cb, map, sizeof(cb));
	munmap(map, sizeof(dserver_ring_shm_t));

	dserver_ring_reject_t r = dserver_ring_shm_validate(&cb, real_size, scm_nsid);
	if (r != dserver_ring_ok) return r;

	if (out_real_size) *out_real_size = real_size;
	if (out_cb) *out_cb = cb;
	return dserver_ring_ok;
}

#endif // __linux__ && !DSERVER_RING_NO_ATTACH_CHECK

#endif // DSERVER_RING_TRANSPORT

//
// S2C
//
// server-to-client RPC calls
//

enum dserver_s2c_msgnum {
	dserver_s2c_msgnum_invalid = 0,
	dserver_s2c_msgnum_mmap,
	dserver_s2c_msgnum_munmap,
	dserver_s2c_msgnum_mprotect,
	dserver_s2c_msgnum_msync,
};

typedef enum dserver_s2c_msgnum dserver_s2c_msgnum_t;

typedef struct dserver_s2c_callhdr {
	int call_number;
	dserver_s2c_msgnum_t s2c_number;
} dserver_s2c_callhdr_t;

typedef struct dserver_s2c_replyhdr {
	int call_number;
	int pid;
	int tid;
	int architecture;
	dserver_s2c_msgnum_t s2c_number;
} dserver_s2c_replyhdr_t;

typedef struct dserver_s2c_call_mmap {
	dserver_s2c_callhdr_t header;
	uint64_t address;
	uint64_t length;
	int32_t protection;
	int32_t flags;
	int32_t fd;
	int64_t offset;
} dserver_s2c_call_mmap_t;

typedef struct dserver_s2c_reply_mmap {
	dserver_s2c_replyhdr_t header;
	uint64_t address;
	int errno_result;
} dserver_s2c_reply_mmap_t;

typedef struct dserver_s2c_call_munmap {
	dserver_s2c_callhdr_t header;
	uint64_t address;
	uint64_t length;
} dserver_s2c_call_munmap_t;

typedef struct dserver_s2c_reply_munmap {
	dserver_s2c_replyhdr_t header;
	int return_value;
	int errno_result;
} dserver_s2c_reply_munmap_t;

typedef struct dserver_s2c_call_mprotect {
	dserver_s2c_callhdr_t header;
	uint64_t address;
	uint64_t length;
	int protection;
} dserver_s2c_call_mprotect_t;

typedef struct dserver_s2c_reply_mprotect {
	dserver_s2c_replyhdr_t header;
	int return_value;
	int errno_result;
} dserver_s2c_reply_mprotect_t;

typedef struct dserver_s2c_call_msync {
	dserver_s2c_callhdr_t header;
	uint64_t address;
	uint64_t size;
	int sync_flags;
} dserver_s2c_call_msync_t;

typedef struct dserver_s2c_reply_msync {
	dserver_s2c_replyhdr_t header;
	int return_value;
	int errno_result;
} dserver_s2c_reply_msync_t;

typedef union dserver_s2c_call {
	dserver_s2c_call_mmap_t mmap;
	dserver_s2c_call_munmap_t munmap;
	dserver_s2c_call_mprotect_t mprotect;
        dserver_s2c_call_msync_t msync;
} dserver_s2c_call_t;

#if __cplusplus
};
#endif

#endif // _DARLINGSERVER_RPC_SUPPLEMENT_H_
