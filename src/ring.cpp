/**
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling developers
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Darling is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Darling.  If not, see <http://www.gnu.org/licenses/>.
 */

// perf #18 (bead dar-dar6x4-perf-5dq.30): RingBuffer -- server-side owner of an attached
// shared-memory ring. See ring.hpp + ~/work/dar-6x4-repro/PERF18-SHMEM-RING-ABI.md.

#ifdef DSERVER_RING_TRANSPORT

#define _GNU_SOURCE 1
#include <darlingserver/ring.hpp>
#include <darlingserver/metrics.hpp>

#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>

DarlingServer::RingBuffer::RingBuffer(void* map, size_t size, FD&& eventfd, const dserver_ring_shm_t& cb):
	_map(map),
	_size(size),
	_eventfd(std::move(eventfd))
{
	std::memcpy(&_cb, &cb, sizeof(_cb));
};

DarlingServer::RingBuffer::~RingBuffer() {
	if (_map && _map != MAP_FAILED) {
		munmap(_map, _size);
		_map = nullptr;
	}
	// _eventfd (FD) closes itself.
};

std::shared_ptr<DarlingServer::RingBuffer> DarlingServer::RingBuffer::attach(int ring_fd, uint64_t claimed_size, int32_t scm_nsid, dserver_ring_reject_t* outReject) {
	uint64_t realSize = 0;
	dserver_ring_shm_t cb;

	// Validate against real kernel state first: fstat true size, map RO, copy + validate the
	// control block. Nothing lasting is mapped if this rejects.
	dserver_ring_reject_t r = dserver_ring_attach_check(ring_fd, claimed_size, scm_nsid, &realSize, &cb);
	if (r != dserver_ring_ok) {
		if (outReject) *outReject = r;
		return nullptr;
	}

	// Accepted. Dup the fd so the mapping outlives the Call (which closes its own ring_fd in
	// its dtor), then map the whole region RW.
	int duped = ::dup(ring_fd);
	if (duped < 0) {
		if (outReject) *outReject = dserver_ring_reject_total_size;
		return nullptr;
	}
	FD dupedFD(duped);

	void* map = mmap(NULL, realSize, PROT_READ | PROT_WRITE, MAP_SHARED, dupedFD.fd(), 0);
	if (map == MAP_FAILED) {
		if (outReject) *outReject = dserver_ring_reject_total_size;
		return nullptr;
	}
	// the fd is no longer needed once mapped; the mapping holds its own reference.
	dupedFD = FD();

	// The wake eventfd the guest signals; nonblocking + cloexec like the server's own.
	int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (efd < 0) {
		munmap(map, realSize);
		if (outReject) *outReject = dserver_ring_reject_total_size;
		return nullptr;
	}

	if (outReject) *outReject = dserver_ring_ok;
	// private ctor -> can't use make_shared
	return std::shared_ptr<RingBuffer>(new RingBuffer(map, realSize, FD(efd), cb));
};

int DarlingServer::RingBuffer::eventfd() const {
	return _eventfd.fd();
};

const dserver_ring_shm_t& DarlingServer::RingBuffer::controlBlock() const {
	return _cb;
};

dserver_ring_t* DarlingServer::RingBuffer::c2sRing() const {
	return reinterpret_cast<dserver_ring_t*>(static_cast<char*>(_map) + _cb.c2s_ring_off);
};

dserver_ring_t* DarlingServer::RingBuffer::s2cRing() const {
	return reinterpret_cast<dserver_ring_t*>(static_cast<char*>(_map) + _cb.s2c_ring_off);
};

void* DarlingServer::RingBuffer::arena() const {
	if (_cb.arena_size == 0) {
		return nullptr;
	}
	return static_cast<char*>(_map) + _cb.arena_off;
};

uint32_t DarlingServer::RingBuffer::slotSize() const {
	return _cb.slot_size;
};

uint32_t DarlingServer::RingBuffer::slotCount() const {
	return _cb.slot_count;
};

bool DarlingServer::RingBuffer::publishReply(uint32_t seq, uint32_t callnum, int32_t code, const void* body, uint32_t bodyLen) {
	dserver_ring_t* s2c = s2cRing();
	uint32_t slotSize = _cb.slot_size;
	uint32_t slotCount = _cb.slot_count;
	uint32_t inlineCap = slotSize - (uint32_t)sizeof(dserver_ring_slot_t);

	// header (code) + body must fit one inline slot. (Arena replies are P4.)
	if ((uint64_t)sizeof(dserver_ring_reply_hdr_t) + bodyLen > inlineCap) {
		return false;
	}

	dserver_ring_slot_t* rep = dserver_ring_producer_begin(s2c, slotSize, slotCount);
	if (!rep) {
		return false; // s2c full -> guest retries / UDS-falls-back
	}

	char* payload = (char*)rep + sizeof(dserver_ring_slot_t);
	dserver_ring_reply_hdr_t* rhdr = (dserver_ring_reply_hdr_t*)payload;
	rhdr->code = code;
	if (bodyLen > 0 && body) {
		std::memcpy(payload + sizeof(dserver_ring_reply_hdr_t), body, bodyLen);
	}
	rep->callnum = callnum;
	rep->seq = seq;
	rep->length = (uint32_t)sizeof(dserver_ring_reply_hdr_t) + bodyLen;
	rep->arena_off = 0;
	rep->arena_len = 0;
	rep->flags = (code != 0) ? DSERVER_RING_FLAG_REPLY_ERROR : 0u;

	dserver_ring_producer_publish(s2c);
	return true;
};

bool DarlingServer::RingBuffer::wakeGuest() {
	// The s2c futex word lives in the control block at the head of the mapping. Always bump it
	// (so a guest that races its pre-sleep recheck sees a change and re-checks the ring), but
	// only pay for the FUTEX_WAKE syscall when a guest is actually parked. The guest sets
	// s2c_waiters BEFORE its final recheck and we publish the reply BEFORE reading the bit, so a
	// 0 here means the guest is still spinning (it'll see the new tail) -- never a lost wakeup.
	dserver_ring_shm_t* cb = (dserver_ring_shm_t*)_map;
	uint32_t* word = &cb->s2c_futex;
	__atomic_fetch_add(word, 1u, __ATOMIC_RELEASE);
	if (dserver_ring_server_should_wake(cb)) {
		syscall(SYS_futex, word, FUTEX_WAKE, 1, nullptr, nullptr, 0);
		Metrics::shared().ringWakesIssued.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	Metrics::shared().ringWakesSkipped.fetch_add(1, std::memory_order_relaxed);
	return false;
};

void DarlingServer::RingBuffer::drainWake() {
	eventfd_t value;
	eventfd_read(_eventfd.fd(), &value);
};

void DarlingServer::RingBuffer::setServerState(uint32_t state) {
	dserver_ring_shm_t* cb = (dserver_ring_shm_t*)_map;
	__atomic_store_n(&cb->server_state, state, __ATOMIC_RELEASE);
};

bool DarlingServer::RingBuffer::hasPendingRequests() const {
	// consumer_begin is bounds-safe against a corrupt guest tail (returns NULL on empty OR on a
	// distance > slot_count); a non-NULL result means there is a real published slot to service.
	dserver_ring_t* c2s = c2sRing();
	return dserver_ring_consumer_begin(c2s, _cb.slot_size, _cb.slot_count) != nullptr;
};

dserver_ring_shm_t* DarlingServer::RingBuffer::liveControlBlock() const {
	// The duplex mailbox words are written by BOTH sides on the shared page; reach the live mapping
	// (not the layout-only _cb copy). _map is the validated RW mapping; null before attach.
	return reinterpret_cast<dserver_ring_shm_t*>(_map);
};

bool DarlingServer::RingBuffer::duplexCapable(uint32_t capBit) const {
	dserver_ring_shm_t* cb = liveControlBlock();
	if (!cb) {
		return false;
	}
	// ABI v4+ guarantees the duplex words exist; the guest publishes its supported caps at attach.
	// _cb (the validated copy) carries the negotiated abi_version, so we gate on it without re-reading
	// the live page's version (which the guest could in principle mutate post-attach).
	if (_cb.abi_version < 4u) {
		return false;
	}
	return (__atomic_load_n(&cb->duplex_caps, __ATOMIC_ACQUIRE) & capBit) != 0u;
};

#endif // DSERVER_RING_TRANSPORT
