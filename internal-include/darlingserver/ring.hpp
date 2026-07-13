#ifndef _DARLINGSERVER_RING_HPP_
#define _DARLINGSERVER_RING_HPP_

// perf #18 (bead dar-dar6x4-perf-5dq.30): server-side owner of an attached shared-memory ring.
//
// A RingBuffer owns one guest-shared mapping (the dserver_ring_shm control block + the two
// SPSC rings + arena) plus the eventfd the guest signals to wake the server. It is created
// only AFTER dserver_ring_attach_check() has validated the guest's control block, so by the
// time a RingBuffer exists the layout offsets are known-good and bounded. RAII tears the
// mapping + eventfd down on destruction; the owning Thread drops its shared_ptr on death.
//
// The whole type is gated behind DSERVER_RING_TRANSPORT so a default build never references
// it (byte-identical when off).

#ifdef DSERVER_RING_TRANSPORT

#include <cstdint>
#include <cstddef>
#include <memory>

#include <darlingserver/rpc-supplement.h>
#include <darlingserver/utility.hpp>

namespace DarlingServer {
	class Thread;

	// perf #18 P4: service every C2S request a thread's ring has published, dispatching each
	// through the normal Call path and redirecting the reply onto the s2c ring. Defined in
	// call.cpp (it reuses the Call machinery there); declared here so the server's main-loop
	// spin phase (server.cpp) can drain rings directly, not only the eventfd Monitor callback.
	// Returns the number of requests serviced.
	uint32_t ringServiceThread(const std::shared_ptr<Thread>& thread);

	class RingBuffer {
	private:
		void* _map = nullptr;     // the RW mapping of the guest memfd (size _size)
		size_t _size = 0;
		FD _eventfd;              // guest writes this to wake the server; the server reads to drain
		dserver_ring_shm_t _cb;   // a server-owned COPY of the validated control block (never re-read from the page for layout)

		RingBuffer(void* map, size_t size, FD&& eventfd, const dserver_ring_shm_t& cb);

	public:
		~RingBuffer();

		RingBuffer(const RingBuffer&) = delete;
		RingBuffer& operator=(const RingBuffer&) = delete;

		/**
		 * Validate + attach the guest-supplied ring memfd. Runs dserver_ring_attach_check()
		 * (fstat true size, map RO, copy + validate the control block), and on success maps
		 * the region RW and creates the wake eventfd. Returns nullptr on any rejection (the
		 * caller falls the thread back to UDS); *outReject carries the reason for logging.
		 *
		 * The fd is duped internally for the lasting mapping; the caller keeps ownership of
		 * its own ring_fd (the generated Call dtor closes it as usual).
		 */
		static std::shared_ptr<RingBuffer> attach(int ring_fd, uint64_t claimed_size, int32_t scm_nsid, dserver_ring_reject_t* outReject);

		/** The eventfd to register as a Readable Monitor; guest signals it to wake the server. */
		int eventfd() const;

		/** Accessors into the validated mapping (offsets came from the validated control block). */
		const dserver_ring_shm_t& controlBlock() const;
		dserver_ring_t* c2sRing() const;  // request ring (guest -> server)
		dserver_ring_t* s2cRing() const;  // reply ring (server -> guest)
		void* arena() const;              // nullptr if arena_size == 0
		uint32_t slotSize() const;
		uint32_t slotCount() const;

		/**
		 * Publish one reply onto the s2c ring: claim a slot, write the reply header (code) +
		 * body, echo the request's seq + callnum, single-release-store publish. Returns false
		 * if the s2c ring is full (caller leaves the guest to retry / UDS-fall-back) or the body
		 * exceeds the inline slot capacity. Does NOT wake the guest -- call wakeGuest() after a
		 * batch so one wake covers several replies.
		 */
		bool publishReply(uint32_t seq, uint32_t callnum, int32_t code, const void* body, uint32_t bodyLen);

		/**
		 * Wake a guest that is FUTEX_WAITing on the s2c futex word. perf #18 P4: this is now
		 * CONDITIONAL -- it always bumps the s2c_futex word (so a guest racing its pre-sleep
		 * recheck sees the change), but only issues the FUTEX_WAKE syscall when the guest has
		 * parked (s2c_waiters != 0, via dserver_ring_server_should_wake). A spinning guest costs
		 * zero syscalls. Returns true if a FUTEX_WAKE was actually issued (for metrics/tests).
		 */
		bool wakeGuest();

		/** Drain the wake eventfd (called from the Monitor callback so it stops re-firing). */
		void drainWake();

		/**
		 * perf #18 P4: publish the server's current sleep state (DSERVER_RING_SRV_*) into the
		 * shared control block so a producing guest can skip the doorbell while we poll. A single
		 * release-store; the guest reads it with acquire in dserver_ring_guest_should_doorbell().
		 */
		void setServerState(uint32_t state);

		/**
		 * perf #18 P4: true if the c2s (request) ring has at least one published, unconsumed slot.
		 * Used by the server's pre-epoll spin phase to decide whether there is work to drain
		 * without committing to service it. Bounds-safe (defends against a corrupt guest tail).
		 */
		bool hasPendingRequests() const;

		// --- perf #18 P8 D3 (dar-1il.3.1.1): duplex lane mailbox access -----------------------------
		// The duplex mailbox lives in the SHARED mapping (the live page), NOT in the server-owned _cb
		// COPY -- the copy is layout-only and never re-read. These reach the live page so the duplex
		// upcall/reply words the guest pump writes are actually observed. The duplex datapath uses ONLY
		// the pure helpers in rpc-supplement.h (dserver_ring_duplex_*) over this pointer, so the wake
		// model + correlation that ring_duplex_wake_gate_test / ring_duplex_roundtrip_test pin hold
		// byte-for-byte in the real transport. nullptr-safe to call before attach (returns nullptr).
		dserver_ring_shm_t* liveControlBlock() const; // the shared page (duplex words live here)

		// True iff the guest advertised the given duplex capability bit at attach (cb.duplex_caps).
		// A real op may take the duplex S2C path ONLY when this holds for its cap -- part of the
		// conjunction guard (no duplex to a guest that never agreed to pump).
		bool duplexCapable(uint32_t capBit) const;
	};
};

#endif // DSERVER_RING_TRANSPORT

#endif // _DARLINGSERVER_RING_HPP_
