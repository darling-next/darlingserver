project(darlingserver)

cmake_minimum_required(VERSION 3.13)

option(DSERVER_TOOLS "Build darlingserver tools (for debugging)" OFF)
option(DSERVER_ASAN "Build darlingserver with ASAN" OFF)
option(DSERVER_UBSAN "Build darlingserver with UBSAN" OFF)
option(DARLING_EUNION
	"Enable E-UNION union-in-vchroot prefix assembly (experimental)"
	OFF)

if (DSERVER_TOOLS)
	add_subdirectory(tools)
endif()

if (DSERVER_ASAN)
	add_compile_definitions(
		DSERVER_ASAN=1
	)
else()
	add_compile_definitions(
		DSERVER_ASAN=0
	)
endif()

if (DSERVER_UBSAN)
	add_compile_definitions(
		DSERVER_UBSAN=1
	)
else()
	add_compile_definitions(
		DSERVER_UBSAN=0
	)
endif()

set(dserver_sanitizers "")
if (DSERVER_ASAN)
	if (NOT (dserver_sanitizers STREQUAL ""))
		set(dserver_sanitizers "${dserver_sanitizers},")
	endif()
	set(dserver_sanitizers "${dserver_sanitizers}address")
endif()
if (DSERVER_UBSAN)
	if (NOT (dserver_sanitizers STREQUAL ""))
		set(dserver_sanitizers "${dserver_sanitizers},")
	endif()
	set(dserver_sanitizers "${dserver_sanitizers}undefined")
endif()

option(DSERVER_EXTENDED_DEBUG "Build darlingserver with extended debugging support" OFF)

if (DSERVER_EXTENDED_DEBUG)
	add_compile_definitions(
		DSERVER_EXTENDED_DEBUG=1
	)
else()
	add_compile_definitions(
		DSERVER_EXTENDED_DEBUG=0
	)
endif()

option(DSERVER_SINGLE_THREADED "Only use a single thread per workqueue in darlingserver" ON)

# perf #18 (dar-dar6x4-perf-5dq.30): shared-memory ring transport for the hot-path RPC.
# perf#18 Phase B Stage 1 (dar-dar6x4-perf-5dq.30, 2026-06-29): default flipped OFF->ON for
# dev/nightly. The simple fast ring carries the FROZEN op set only {task_self_trap, mach_reply_port,
# mach_port_allocate, mach_port_insert_right}; destroy-capable ops stay UDS (Phase A compile-time
# guardrail). Rollback: this default back to OFF + rebuild (knob #3), or runtime DARLING_SERVER_FAST_OPS=0
# (knob #2, ring still used via generic fiber). RELEASE/PROD packaging policy is separate and unchanged.
# When OFF the ring ABI/handler code is compiled out (the ring_attach RPC is still defined, but its
# handler replies "unsupported"). See tests/PERF18-RING-DEFAULT-ON-DECISION.md.
option(DSERVER_RING_TRANSPORT "Build darlingserver with shared-memory ring transport support (perf#18)" ON)

if (DSERVER_SINGLE_THREADED)
	add_compile_definitions(
		DSERVER_SINGLE_THREADED=1
	)
else()
	add_compile_definitions(
		DSERVER_SINGLE_THREADED=0
	)
endif()

add_subdirectory(duct-tape)

set(DARLINGSERVER_INIT_PROCESS "/sbin/launchd" CACHE STRING "The init process darlingserver should execute (default is \"/sbin/launchd\")")
set(DARLINGSERVER_XDG_USER_DIR_CMD "xdg-user-dir" CACHE STRING "The xdg-user-dir binary darlingserver should execute (default is \"xdg-user-dir\")")

include_directories(
	include
	internal-include
	${CMAKE_CURRENT_BINARY_DIR}/include
	${CMAKE_CURRENT_BINARY_DIR}/internal-include
	${CMAKE_BINARY_DIR}/src/startup # for `rtsig.h`
)

add_custom_command(
	OUTPUT
		${CMAKE_CURRENT_BINARY_DIR}/include/darlingserver/rpc.h
		${CMAKE_CURRENT_BINARY_DIR}/internal-include/darlingserver/rpc.internal.h
		${CMAKE_CURRENT_BINARY_DIR}/src/rpc.c
	COMMAND
		${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-rpc-wrappers.py
		${CMAKE_CURRENT_BINARY_DIR}/include/darlingserver/rpc.h
		${CMAKE_CURRENT_BINARY_DIR}/internal-include/darlingserver/rpc.internal.h
		${CMAKE_CURRENT_BINARY_DIR}/src/rpc.c
		\"../include/darlingserver/rpc.h\"
	VERBATIM
	MAIN_DEPENDENCY
		${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-rpc-wrappers.py
)

add_custom_target(generate_dserver_rpc_wrappers
	ALL
	DEPENDS
		${CMAKE_CURRENT_BINARY_DIR}/include/darlingserver/rpc.h
		${CMAKE_CURRENT_BINARY_DIR}/internal-include/darlingserver/rpc.internal.h
		${CMAKE_CURRENT_BINARY_DIR}/src/rpc.c
)

add_executable(darlingserver
	src/darlingserver.cpp
	src/server.cpp
	src/message.cpp
	src/call.cpp
	src/registry.cpp
	src/logging.cpp
	src/process.cpp
	src/thread.cpp
	src/utility.cpp
	src/kqchan.cpp
	src/async-writer.cpp
	src/stack-pool.cpp
	src/metrics.cpp
	src/test-diagnostics.cpp
	src/runtime-mode.cpp
	src/ring.cpp
)
target_compile_definitions(darlingserver PRIVATE
	DARLING_RUNTIME_EUNION_CAPABLE=$<BOOL:${DARLING_EUNION}>
)

add_executable(dserver_process_identity_tests
	tests/process_identity_test.cpp
)

target_compile_options(dserver_process_identity_tests PRIVATE
	-std=c++17
)

add_custom_target(dserver_process_identity_tests_run
	COMMAND $<TARGET_FILE:dserver_process_identity_tests>
	DEPENDS dserver_process_identity_tests
)

add_executable(dserver_runtime_mode_tests
	tests/runtime_mode_test.cpp
	src/runtime-mode.cpp
)
target_compile_options(dserver_runtime_mode_tests PRIVATE
	-std=c++17
)
target_include_directories(dserver_runtime_mode_tests PRIVATE
	include
)

add_dependencies(darlingserver
	generate_dserver_rpc_wrappers
	rtsig_h
)

target_link_libraries(darlingserver PRIVATE
	darlingserver_duct_tape
)

target_compile_options(darlingserver PRIVATE
	-pthread
	-std=c++17
)
target_link_options(darlingserver PRIVATE
	-pthread
	-Wl,--gc-sections
)

if (DSERVER_ASAN OR DSERVER_UBSAN)
	target_compile_options(darlingserver PRIVATE
		-fsanitize=${dserver_sanitizers}
		-fsanitize-recover=all
		-fno-omit-frame-pointer
		-g
		-Og
	)
	target_link_options(darlingserver PRIVATE
		-fsanitize=${dserver_sanitizers}
		-fsanitize-recover=all
	)
endif()

if (DSERVER_EXTENDED_DEBUG)
	target_compile_options(darlingserver PRIVATE
		-fno-omit-frame-pointer
	)
endif()

install(TARGETS darlingserver DESTINATION bin)

#file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${DARLING_SDK_RELATIVE_PATH}/usr/include/darlingserver")
#create_symlink(
#	"${DARLING_ROOT_RELATIVE_TO_SDK}/../../../src/darlingserver/include/darlingserver/rpc.h"
#	"${CMAKE_BINARY_DIR}/${DARLING_SDK_RELATIVE_PATH}/usr/include/darlingserver/rpc.h"
#)

if (DSERVER_RING_TRANSPORT)
	target_compile_definitions(darlingserver PRIVATE
		DSERVER_RING_TRANSPORT=1
	)
endif()

if (NOT "${DARLINGSERVER_INIT_PROCESS}" STREQUAL "")
	target_compile_definitions(darlingserver PRIVATE
		DARLINGSERVER_INIT_PROCESS="${DARLINGSERVER_INIT_PROCESS}"
	)
endif()

if (NOT "${DARLINGSERVER_XDG_USER_DIR_CMD}" STREQUAL "")
	target_compile_definitions(darlingserver PRIVATE
		DARLINGSERVER_XDG_USER_DIR_CMD="${DARLINGSERVER_XDG_USER_DIR_CMD}"
	)
endif()
