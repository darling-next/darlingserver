#include <darlingserver/duct-tape/thread-state.h>

/*
 * dar-dtape-explicit-context-6to3.3 -- the shared decision for the guest thread-state save/load.
 *
 * There is deliberately NO ambient parameter and NO ambient call in this file: the function can
 * only act on the `xnu_target` it is given. A host contract (tests/thread_state_test.cpp)
 * compiles this file and asserts that the target handed to `set_state`/`get_state` is exactly the
 * explicit one, and that a reverted implementation that reached for `current_thread()` cannot
 * build there.
 */

int dtape_thread_state_load(
	void* xnu_target,
	int architecture,
	uintptr_t thread_state_address,
	uintptr_t float_state_address,
	const dtape_thread_state_layout_desc_t* desc,
	void* thread_buffer,
	void* float_buffer,
	const dtape_thread_state_io_t* io)
{
	if (!dtape_thread_state_target_valid(xnu_target)) {
		return dtape_thread_state_efault;
	}
	if (dtape_thread_state_layout_for_architecture(architecture) == dtape_thread_state_layout_none) {
		return dtape_thread_state_enosys;
	}
	if (io->copyin(io->ctx, thread_state_address, thread_buffer, desc->thread_size)) {
		return dtape_thread_state_efault;
	}
	if (io->copyin(io->ctx, float_state_address, float_buffer, desc->float_size)) {
		return dtape_thread_state_efault;
	}

	/* The XNU traps key entirely off the explicit target; both kinds of state go to it. */
	io->set_state(io->ctx, xnu_target, desc->thread_flavor, thread_buffer, desc->thread_count);
	io->set_state(io->ctx, xnu_target, desc->float_flavor, float_buffer, desc->float_count);
	return dtape_thread_state_ok;
}

int dtape_thread_state_save(
	void* xnu_target,
	int architecture,
	uintptr_t thread_state_address,
	uintptr_t float_state_address,
	const dtape_thread_state_layout_desc_t* desc,
	void* thread_buffer,
	void* float_buffer,
	const dtape_thread_state_io_t* io)
{
	if (!dtape_thread_state_target_valid(xnu_target)) {
		return dtape_thread_state_efault;
	}
	if (dtape_thread_state_layout_for_architecture(architecture) == dtape_thread_state_layout_none) {
		return dtape_thread_state_enosys;
	}

	io->get_state(io->ctx, xnu_target, desc->thread_flavor, thread_buffer, desc->thread_count);
	if (io->copyout(io->ctx, thread_buffer, thread_state_address, desc->thread_size)) {
		return dtape_thread_state_efault;
	}
	io->get_state(io->ctx, xnu_target, desc->float_flavor, float_buffer, desc->float_count);
	if (io->copyout(io->ctx, float_buffer, float_state_address, desc->float_size)) {
		return dtape_thread_state_efault;
	}
	return dtape_thread_state_ok;
}
