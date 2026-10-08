cmake_minimum_required(VERSION 3.20)
project(MacLinuxGPUHSA LANGUAGES C CXX)
if(NOT APPLE)
  message(FATAL_ERROR "The IOKit transport requires macOS")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# The userspace HSA runtime: a separate dylib (NOT compiled into the dext).
# Host processes link against it; it talks to the dext over the IOKit
# user-client selector-RPC. The transport is abstracted (the Connection
# interface in src/transport.h); the IOKit transport (src/transport_iokit.cpp)
# is the real one, and the fake (src/transport_fake.cpp) is the host-testing
# backend selected by MAC_LINUXGPU_FAKE_TRANSPORT=1.
set(HSA_RUNTIME_SOURCES
  src/runtime.cpp
  src/gpu_signals.cpp
  src/gpu_signal_service.cpp
  src/memory.cpp
  src/bar_writes.cpp
  src/queues.cpp
  src/power.cpp
  src/device_init.cpp
  src/isa.cpp
  src/isa_target.cpp
  src/signal_kernels.cpp
  src/code_object.cpp
  src/code_heap.cpp
  src/executable.cpp
  src/virtual_memory.cpp
  src/host_services.cpp
  src/host_window.cpp
  src/allocation_census.cpp
  src/ipc_memory.cpp
  src/ipc_signal.cpp
  src/platform_extensions.cpp
  src/transport_iokit.cpp
  src/transport_fake.cpp)

# The on-demand firmware servicer the Linux-shim driver needs while InitDevice
# runs (protocol: linuxu/headers/rt/fw_mailbox.h). Compiled in place from
# host/, which the host app's Xcode target also builds, so there is one copy.
set(FW_SERVICER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../host)
set(FW_SERVICER_SOURCES
  ${FW_SERVICER_DIR}/fw_mailbox_service.c
  ${FW_SERVICER_DIR}/fw_mailbox_iokit.c)

# iOS/iPadOS: an app may not dlopen a runtime from outside its bundle, so the
# runtime is a static library linked into the app (the IOKit transport and
# the firmware servicer are the same). Host unit tests run on macOS only.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(HSA_RUNTIME_LIBRARY_TYPE STATIC)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
else()
  set(HSA_RUNTIME_LIBRARY_TYPE SHARED)
endif()
add_library(hsa-runtime64 ${HSA_RUNTIME_LIBRARY_TYPE} ${HSA_RUNTIME_SOURCES} ${FW_SERVICER_SOURCES})
target_compile_definitions(hsa-runtime64 PRIVATE HSA_EXPORT)
target_include_directories(hsa-runtime64 PUBLIC include third_party/hsa/include)
# host/selector_call.h (async session calls, with the driver's selector
# rules from dext/sources/session_state.h) is shared with the host app.
target_include_directories(hsa-runtime64 PRIVATE ${FW_SERVICER_DIR})
# Optimized whatever the build type: `make hsa` configures Debug for the
# tests' asserts, and this library is the one the installer ships. Every
# signal load, queue index update and doorbell runs through it.
target_compile_options(hsa-runtime64 PRIVATE -Wall -Wextra -Werror -O2)
target_link_libraries(hsa-runtime64 PRIVATE "-framework IOKit" "-framework CoreFoundation")
if(HSA_RUNTIME_LIBRARY_TYPE STREQUAL "SHARED")
  set_target_properties(hsa-runtime64 PROPERTIES VERSION 0.1.0 SOVERSION 1)
endif()

include(CTest)
if(BUILD_TESTING)
  # Host unit tests: exercise the runtime's transport-agnostic logic (packet
  # construction, signal state machine, queue management, memory allocation)
  # against the fake backend (MAC_LINUXGPU_FAKE_TRANSPORT=1, no IOKit, no dext).
  add_executable(hsa-aql-packet-test tests/test_aql_packet.cpp)
  target_link_libraries(hsa-aql-packet-test PRIVATE hsa-runtime64)
  target_compile_options(hsa-aql-packet-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-aql-packet-test PRIVATE src third_party/hsa/include)
  add_test(NAME hsa-aql-packet COMMAND hsa-aql-packet-test)
  set_tests_properties(hsa-aql-packet PROPERTIES
    ENVIRONMENT "MAC_LINUXGPU_FAKE_TRANSPORT=1")

  add_executable(hsa-signal-test tests/test_signal.cpp)
  target_link_libraries(hsa-signal-test PRIVATE hsa-runtime64)
  target_compile_options(hsa-signal-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-signal-test PRIVATE src third_party/hsa/include)
  add_test(NAME hsa-signal COMMAND hsa-signal-test)
  set_tests_properties(hsa-signal PROPERTIES
    ENVIRONMENT "MAC_LINUXGPU_FAKE_TRANSPORT=1")

  add_executable(hsa-queue-test tests/test_queue.cpp)
  target_link_libraries(hsa-queue-test PRIVATE hsa-runtime64)
  target_compile_options(hsa-queue-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-queue-test PRIVATE src third_party/hsa/include)
  add_test(NAME hsa-queue COMMAND hsa-queue-test)
  set_tests_properties(hsa-queue PROPERTIES
    ENVIRONMENT "MAC_LINUXGPU_FAKE_TRANSPORT=1")

  # ISA identity, generic-target compatibility, code-object acceptance and
  # signal-kernel selection across GPU generations. Internal (hidden) code, so
  # the sources are compiled into the test rather than linked from the dylib.
  add_executable(hsa-isa-target-test tests/test_isa_target.cpp src/isa_target.cpp
    src/code_object.cpp src/signal_kernels.cpp src/device_init.cpp)
  target_compile_options(hsa-isa-target-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-isa-target-test PRIVATE src third_party/hsa/include)
  add_test(NAME hsa-isa-target COMMAND hsa-isa-target-test)

  # Agent/ISA info and queue-slot degradation against configurable fake
  # devices of several generations, and device power (suspend, resume, lost
  # device memory) against the fake's power model.
  foreach(name device-info queue-slots power signal-wait memory-fault bar-writes vmem-access)
    string(REPLACE "-" "_" source ${name})
    add_executable(hsa-${name}-test tests/test_${source}.cpp)
    target_link_libraries(hsa-${name}-test PRIVATE hsa-runtime64)
    target_compile_options(hsa-${name}-test PRIVATE -Wall -Wextra -Werror)
    target_include_directories(hsa-${name}-test PRIVATE src third_party/hsa/include)
    add_test(NAME hsa-${name} COMMAND hsa-${name}-test)
    set_tests_properties(hsa-${name} PROPERTIES ENVIRONMENT "MAC_LINUXGPU_FAKE_TRANSPORT=1")
  endforeach()

  # Code-object loading while a persistent queue holds the only queue slot:
  # the loader's code-sync falls back to an AQL packet on that queue. Uses the
  # runtime's bundled kernels (internal code) as the code object.
  add_executable(hsa-code-sync-test tests/test_code_sync.cpp src/signal_kernels.cpp
    src/isa_target.cpp src/code_object.cpp)
  target_link_libraries(hsa-code-sync-test PRIVATE hsa-runtime64)
  target_compile_options(hsa-code-sync-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-code-sync-test PRIVATE src third_party/hsa/include)
  add_test(NAME hsa-code-sync COMMAND hsa-code-sync-test)
  set_tests_properties(hsa-code-sync PROPERTIES ENVIRONMENT "MAC_LINUXGPU_FAKE_TRANSPORT=1")

  # The GPU signal service retiring while another thread holds the session
  # (a large transfer): internal code, against a connection whose "GPU" is a
  # thread running the service kernel's mailbox protocol.
  add_executable(hsa-signal-service-test tests/test_signal_service.cpp src/gpu_signal_service.cpp
    src/code_object.cpp src/signal_kernels.cpp src/isa_target.cpp)
  target_compile_options(hsa-signal-service-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-signal-service-test PRIVATE src include third_party/hsa/include)
  add_test(NAME hsa-signal-service COMMAND hsa-signal-service-test)

  # The code-object heap: its allocator, staged uploads and the loader's
  # use of it (internal code), against the fake transport.
  add_executable(hsa-code-heap-test tests/test_code_heap.cpp src/code_heap.cpp src/signal_kernels.cpp
    src/isa_target.cpp src/code_object.cpp)
  target_link_libraries(hsa-code-heap-test PRIVATE hsa-runtime64)
  target_compile_options(hsa-code-heap-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-code-heap-test PRIVATE src include third_party/hsa/include)
  add_test(NAME hsa-code-heap COMMAND hsa-code-heap-test)
  add_test(NAME hsa-code-heap-legacy COMMAND hsa-code-heap-test legacy)
  set_tests_properties(hsa-code-heap hsa-code-heap-legacy PROPERTIES ENVIRONMENT "MAC_LINUXGPU_FAKE_TRANSPORT=1")

  add_executable(hsa-code-sync-coalesce-test tests/test_code_sync_coalesce.cpp src/signal_kernels.cpp
    src/isa_target.cpp src/code_object.cpp)
  target_link_libraries(hsa-code-sync-coalesce-test PRIVATE hsa-runtime64)
  target_compile_options(hsa-code-sync-coalesce-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-code-sync-coalesce-test PRIVATE src third_party/hsa/include)
  add_test(NAME hsa-code-sync-coalesce COMMAND hsa-code-sync-coalesce-test)
  set_tests_properties(hsa-code-sync-coalesce PROPERTIES ENVIRONMENT "MAC_LINUXGPU_FAKE_TRANSPORT=1")

  add_executable(hsa-memory-test tests/test_memory.cpp)
  target_link_libraries(hsa-memory-test PRIVATE hsa-runtime64)
  target_compile_options(hsa-memory-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-memory-test PRIVATE src third_party/hsa/include)
  add_test(NAME hsa-memory COMMAND hsa-memory-test)
  set_tests_properties(hsa-memory PROPERTIES
    ENVIRONMENT "MAC_LINUXGPU_FAKE_TRANSPORT=1")

  # The host window reservation (against this process's real address space),
  # the host memory budget and the window size asked of the driver.
  add_executable(hsa-host-window-test tests/test_host_window.cpp src/host_window.cpp)
  target_compile_options(hsa-host-window-test PRIVATE -Wall -Wextra -Werror)
  target_include_directories(hsa-host-window-test PRIVATE src)
  add_test(NAME hsa-host-window COMMAND hsa-host-window-test)

  add_executable(hsa-allocation-census-test tests/test_allocation_census.cpp src/allocation_census.cpp)
  target_compile_options(hsa-allocation-census-test PRIVATE -Wall -Wextra -Werror -O1)
  target_include_directories(hsa-allocation-census-test PRIVATE src)
  add_test(NAME hsa-allocation-census COMMAND hsa-allocation-census-test)
endif()
