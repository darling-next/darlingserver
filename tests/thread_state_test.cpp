// Host contract for the explicit-context thread-state boundary (dar-dtape-explicit-context-6to3.3).
//
// The product operations dtape_thread_load_state_from_user / _save_state_to_user NAME a target
// thread. Their only ambient-context dependency used to be that they substituted current_thread()
// for that named target; the fix routes the decision through the shared, ambient-free module
// duct-tape/src/thread-state.c (compiled here) and forwards the explicit target.
//
// This contract drives that decision without a guest and without XNU:
//   * it pins the supported-architecture mapping (an unimplemented guest architecture fails ENOSYS
//     instead of guessing);
//   * it pins the explicit-target validity rule (a NULL named target is an error, never a silent
//     fallback to the ambient thread);
//   * and it proves the important invariant directly: the XNU target handed to
//     thread_set_state/thread_get_state is EXACTLY the explicit target, never an ambient one.
//
// The shared module has no ambient parameter and no ambient call; a reverted implementation that
// reached for current_thread() could not build against this file (no XNU headers, and the
// current_thread() stub below is counted, so any use shows up as a non-zero ambientCalls).

#include <darlingserver/duct-tape/thread-state.h>

#include <cassert>
#include <cstdint>

namespace {

constexpr std::uintptr_t kExplicit = 0xABCD0000u;
constexpr std::uintptr_t kAmbient = 0x11110000u;

int g_ambientCalls = 0;

struct Io {
	void* lastSetTarget = nullptr;
	void* lastGetTarget = nullptr;
	int setCalls = 0;
	int getCalls = 0;
	int copyinCalls = 0;
	int copyoutCalls = 0;
	bool failCopyin = false;
};

int ioCopyin(void* ctx, uintptr_t, void*, unsigned long) {
	auto* io = static_cast<Io*>(ctx);
	io->copyinCalls++;
	return io->failCopyin ? 1 : 0;
}
int ioCopyout(void* ctx, const void*, uintptr_t, unsigned long) {
	static_cast<Io*>(ctx)->copyoutCalls++;
	return 0;
}
void ioSetState(void* ctx, void* xnuThread, int, const void*, unsigned int) {
	auto* io = static_cast<Io*>(ctx);
	io->setCalls++;
	io->lastSetTarget = xnuThread;
}
void ioGetState(void* ctx, void* xnuThread, int, void*, unsigned int) {
	auto* io = static_cast<Io*>(ctx);
	io->getCalls++;
	io->lastGetTarget = xnuThread;
}

dtape_thread_state_layout_desc_t fakeDesc() {
	dtape_thread_state_layout_desc_t d{};
	d.thread_flavor = 4;
	d.float_flavor = 5;
	d.thread_count = 1;
	d.float_count = 1;
	d.thread_size = sizeof(std::uint64_t);
	d.float_size = sizeof(std::uint64_t);
	return d;
}

dtape_thread_state_io_t fakeIo(Io* io) {
	return dtape_thread_state_io_t{ io, ioCopyin, ioCopyout, ioSetState, ioGetState };
}

} // namespace

// A reverted implementation would call this; the contract counts any use. Provided with C linkage
// so an accidental reintroduction links here during the host build instead of silently passing.
extern "C" void* current_thread(void) {
	g_ambientCalls++;
	return reinterpret_cast<void*>(kAmbient);
}

int main() {
	// --- the pure decisions (architecture mapping + explicit-target validity) ---
	assert(dtape_thread_state_layout_for_architecture(dtape_thread_state_architecture_x86_64)
		== dtape_thread_state_layout_x86_64);
	assert(dtape_thread_state_layout_for_architecture(dtape_thread_state_architecture_i386)
		== dtape_thread_state_layout_i386);
	assert(dtape_thread_state_layout_for_architecture(dtape_thread_state_architecture_invalid)
		== dtape_thread_state_layout_none);
	assert(dtape_thread_state_layout_for_architecture(dtape_thread_state_architecture_arm32)
		== dtape_thread_state_layout_none);
	assert(dtape_thread_state_layout_for_architecture(dtape_thread_state_architecture_arm64)
		== dtape_thread_state_layout_none);
	assert(dtape_thread_state_layout_for_architecture(99) == dtape_thread_state_layout_none);

	assert(!dtape_thread_state_target_valid(nullptr));
	assert(dtape_thread_state_target_valid(reinterpret_cast<const void*>(kExplicit)));

	// --- the invariant: the explicit target reaches thread_set_state/get_state verbatim ---
	{
		Io io;
		auto table = fakeIo(&io);
		auto desc = fakeDesc();
		std::uint64_t threadBuffer = 0;
		std::uint64_t floatBuffer = 0;

		int rc = dtape_thread_state_load(reinterpret_cast<void*>(kExplicit),
			dtape_thread_state_architecture_x86_64, 0x1000, 0x2000, &desc,
			&threadBuffer, &floatBuffer, &table);
		assert(rc == dtape_thread_state_ok);
		assert(io.copyinCalls == 2);
		assert(io.setCalls == 2);
		assert(io.lastSetTarget == reinterpret_cast<void*>(kExplicit));
		assert(io.lastSetTarget != reinterpret_cast<void*>(kAmbient));

		rc = dtape_thread_state_save(reinterpret_cast<void*>(kExplicit),
			dtape_thread_state_architecture_x86_64, 0x1000, 0x2000, &desc,
			&threadBuffer, &floatBuffer, &table);
		assert(rc == dtape_thread_state_ok);
		assert(io.getCalls == 2);
		assert(io.lastGetTarget == reinterpret_cast<void*>(kExplicit));
		assert(io.lastGetTarget != reinterpret_cast<void*>(kAmbient));
		assert(io.copyoutCalls == 2);
	}

	// --- a NULL named target is refused, never substituted ---
	{
		Io io;
		auto table = fakeIo(&io);
		auto desc = fakeDesc();
		std::uint64_t threadBuffer = 0;
		std::uint64_t floatBuffer = 0;

		int rc = dtape_thread_state_load(nullptr, dtape_thread_state_architecture_x86_64,
			0x1000, 0x2000, &desc, &threadBuffer, &floatBuffer, &table);
		assert(rc == dtape_thread_state_efault);
		assert(io.copyinCalls == 0);
		assert(io.setCalls == 0);
	}

	// --- an unimplemented architecture fails before touching the target ---
	{
		Io io;
		auto table = fakeIo(&io);
		auto desc = fakeDesc();
		std::uint64_t threadBuffer = 0;
		std::uint64_t floatBuffer = 0;

		int rc = dtape_thread_state_load(reinterpret_cast<void*>(kExplicit),
			dtape_thread_state_architecture_arm64, 0x1000, 0x2000, &desc,
			&threadBuffer, &floatBuffer, &table);
		assert(rc == dtape_thread_state_enosys);
		assert(io.copyinCalls == 0);
		assert(io.setCalls == 0);
	}

	// --- a failing copyin aborts before any state is applied to the target ---
	{
		Io io;
		io.failCopyin = true;
		auto table = fakeIo(&io);
		auto desc = fakeDesc();
		std::uint64_t threadBuffer = 0;
		std::uint64_t floatBuffer = 0;

		int rc = dtape_thread_state_load(reinterpret_cast<void*>(kExplicit),
			dtape_thread_state_architecture_x86_64, 0x1000, 0x2000, &desc,
			&threadBuffer, &floatBuffer, &table);
		assert(rc == dtape_thread_state_efault);
		assert(io.setCalls == 0);
	}

	// The decision must never consult the ambient thread.
	assert(g_ambientCalls == 0);

	return 0;
}
