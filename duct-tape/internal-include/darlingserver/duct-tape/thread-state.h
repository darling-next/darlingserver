#ifndef _DARLINGSERVER_DUCT_TAPE_THREAD_STATE_H_
#define _DARLINGSERVER_DUCT_TAPE_THREAD_STATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Explicit-context boundary for the guest thread-state save/load operations
 * (`dtape_thread_load_state_from_user` / `dtape_thread_save_state_to_user`).
 *
 * EPIC dar-dtape-explicit-context-6to3 -- central invariant: an operation that NAMES a target
 * thread must act on THAT thread, and must not read `current_thread()`/`current_task()` on the
 * way. A `_for(thread)`/`(thread, ...)` signature guarantees nothing while any transitive callee
 * still substitutes the ambient thread.
 *
 * The XNU thread-state traps (`thread_set_state` / `thread_get_state`) already key entirely off
 * their explicit `thread` argument. The duct-tape wrappers, however, passed `current_thread()`
 * instead of the named target, so a caller that resolved an explicit thread (the management
 * plane, an interrupt stacked on another thread) silently acted on the WRONG thread's state
 * whenever that thread was not the currently-executing one.
 *
 * This module carries the decision and the sequencing with NO XNU dependency, so a host contract
 * can drive it without a guest: the caller supplies the explicit XNU target, the layout
 * descriptor, scratch buffers and an I/O table. `dtape_thread_state_load`/`_save` have no ambient
 * parameter at all -- there is no way for them to select any thread other than the explicit one
 * -- and the host contract asserts the target it receives is the one it was given.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dtape_thread_state_layout {
	/* No thread-state layout is defined for this guest architecture; the operation must fail
	 * with ENOSYS rather than guess. */
	dtape_thread_state_layout_none = 0,
	dtape_thread_state_layout_x86_64,
	dtape_thread_state_layout_i386,
} dtape_thread_state_layout_t;

/* Mirrors the `dserver_rpc_architecture_*` values. Passed as a plain integer so this header
 * stays free of the generated RPC headers; `thread.c` statically asserts the correspondence. */
enum {
	dtape_thread_state_architecture_invalid = 0,
	dtape_thread_state_architecture_i386 = 1,
	dtape_thread_state_architecture_x86_64 = 2,
	dtape_thread_state_architecture_arm32 = 3,
	dtape_thread_state_architecture_arm64 = 4,
};

/* The XNU flavor/count and scratch-buffer size for one architecture's thread state. Supplied by
 * the product (which owns the XNU macros and `sizeof`s); the contract supplies its own values. */
typedef struct dtape_thread_state_layout_desc {
	int thread_flavor;
	int float_flavor;
	unsigned int thread_count;
	unsigned int float_count;
	unsigned int thread_size;
	unsigned int float_size;
} dtape_thread_state_layout_desc_t;

/* Result codes, mapped to the product's negative Linux errno at the boundary. */
enum {
	dtape_thread_state_ok = 0,
	dtape_thread_state_efault = 14, /* -LINUX_EFAULT */
	dtape_thread_state_enosys = 38, /* -LINUX_ENOSYS */
};

/* The only operations the module needs from its host. `xnu_thread` is the EXPLICIT target
 * forwarded verbatim; no callback may substitute an ambient thread. */
typedef struct dtape_thread_state_io {
	void* ctx;
	int (*copyin)(void* ctx, uintptr_t address, void* buffer, unsigned long length);
	int (*copyout)(void* ctx, const void* buffer, uintptr_t address, unsigned long length);
	void (*set_state)(void* ctx, void* xnu_thread, int flavor, const void* state, unsigned int count);
	void (*get_state)(void* ctx, void* xnu_thread, int flavor, void* state, unsigned int count);
} dtape_thread_state_io_t;

/*
 * Returns the layout for a guest architecture, or `none` for an architecture the duct-tape does
 * not implement (invalid, arm32, arm64), which the caller MUST turn into ENOSYS.
 */
static inline dtape_thread_state_layout_t
dtape_thread_state_layout_for_architecture(int architecture)
{
	switch (architecture) {
		case dtape_thread_state_architecture_x86_64:
			return dtape_thread_state_layout_x86_64;
		case dtape_thread_state_architecture_i386:
			return dtape_thread_state_layout_i386;
		default:
			return dtape_thread_state_layout_none;
	}
}

/*
 * The explicit-target validity rule. A caller that named a thread and got NULL must fail; it must
 * NEVER fall back to the ambient/current thread, because setting or reading the current thread's
 * state when a different thread was named corrupts both.
 */
static inline bool
dtape_thread_state_target_valid(const void* explicit_target)
{
	return explicit_target != NULL;
}

/* Load the guest's saved state into `xnu_target` (the named thread) and nothing else. */
int dtape_thread_state_load(
	void* xnu_target,
	int architecture,
	uintptr_t thread_state_address,
	uintptr_t float_state_address,
	const dtape_thread_state_layout_desc_t* desc,
	void* thread_buffer,
	void* float_buffer,
	const dtape_thread_state_io_t* io);

/* Read the named thread's state back out to the guest. */
int dtape_thread_state_save(
	void* xnu_target,
	int architecture,
	uintptr_t thread_state_address,
	uintptr_t float_state_address,
	const dtape_thread_state_layout_desc_t* desc,
	void* thread_buffer,
	void* float_buffer,
	const dtape_thread_state_io_t* io);

#ifdef __cplusplus
}
#endif

#endif /* _DARLINGSERVER_DUCT_TAPE_THREAD_STATE_H_ */
