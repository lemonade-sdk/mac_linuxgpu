#!/usr/bin/env bash
# KFD compute sessions (linuxu/src/amdgpu-rt/kfd_session.c) and the dext's
# KFD-backed queue ABI (dext/sources/dext_kfd.mm) over the unmodified
# upstream KFD: chardev ioctls and mmap, process and queue managers, the
# GFX 12 device queue manager and MQD manager, queue buffer validation and
# doorbells, on the linuxu process substrate, with a fixture amdgpu device
# (KGD memory, MES, SDMA, render node, command processor).
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
cat > "$test_dir/flags.mk" <<'MAKE'
.PHONY: print-includes
print-includes:
	@printf '%s\n' $(INCPATHS)
.PHONY: print-hostcflags
print-hostcflags:
	@printf '%s\n' $(filter-out -MMD -MP -DLINUXU_RT_HOST_SHADOW=1,$(HOSTCFLAGS))
MAKE
include_string=$(make -s -f Makefile -f "$test_dir/flags.mk" print-includes | tr '\n' ' ')
read -r -a includes <<< "$include_string"
host_string=$(make -s -f Makefile -f "$test_dir/flags.mk" print-hostcflags | tr '\n' ' ')
read -r -a hostcflags <<< "$host_string"
# Plain memory MMIO: the fixture's doorbell BAR is host memory. The uapi
# KFD_IOC_ALLOC_MEM_FLAGS_* are (1 << 31) style int shifts, which the kernel
# compiles with -fno-strict-overflow semantics; UBSan's shift-base check is
# off for them, every other check stays fatal.
cflags=("${hostcflags[@]}" -g -O1 -fsanitize=address,undefined -fno-sanitize=shift-base
  -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections "${includes[@]}")
sources=(linuxu/tests/test_kfd_session.c linuxu/tests/kfd_session_fixture.c
  linuxu/src/amdgpu-rt/tmpring_gc12.c
  linuxu/src/amdgpu-rt/kfd_session.c linuxu/src/amdgpu-rt/process_file.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_process.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager_v12.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_queue.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_doorbell.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_flat_memory.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_events.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_debug.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_smi_events.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_debugfs.c
  linuxu/src/shims/process.c linuxu/src/shims/task.c linuxu/src/shims/fd.c
  linuxu/src/shims/chrdev.c linuxu/src/shims/module.c linuxu/src/shims/sysinfo.c
  linuxu/src/drm/dma_resv.c linuxu/src/sync/ww_mutex.c
  linuxu/src/mm/mm.c linuxu/src/mm/mmu_notifier.c linuxu/src/mm/uaccess.c
  linuxu/src/rcu.c linuxu/src/rwsem.c linuxu/src/sync.c linuxu/src/delay.c
  linuxu/src/work.c linuxu/src/timer.c linuxu/src/xarray.c linuxu/src/bug.c
  linuxu/src/spinlock.c linuxu/src/sysfs.c
  linuxu/src/shims/kthread.c third_party/linux/lib/rbtree.c
  linuxu/src/shims/printk.c linuxu/src/shims/debugfs.c
  linuxu/src/shims/seq_file.c linuxu/src/shims/simple.c
  third_party/linux/lib/kfifo.c linuxu/src/shims/bitmap.c third_party/linux/lib/sort.c
  linuxu/src/shims/timekeeping.c linuxu/src/shims/dma_fence.c
  linuxu/src/kmem/kmemalloc.c linuxu/src/kmem/kmemcheck.c
  linuxu/src/kmem/kobject.c linuxu/src/kmem/slab.c linuxu/src/shims/sysfs.c
  linuxu/src/shims/platform_policy.c linuxu/src/mm/page.c linuxu/src/dart/dart.c)
objects=()
pids=()
for source in "${sources[@]}"; do
  object="$test_dir/$(echo "$source" | tr '/' '_').o"
  extra=()
  # Same per-object flags as the Makefile.
  case "$source" in
    third_party/linux/lib/kfifo.c) extra=(-include linux/kernel.h -include linux/bug.h) ;;
    third_party/linux/lib/sort.c) extra=(-include linux/compiler.h -include linux/preempt.h) ;;
  esac
  clang "${cflags[@]}" "${extra[@]}" -c "$source" -o "$object" &
  pids+=($!)
  objects+=("$object")
done
failed=0
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
[ "$failed" = 0 ] || exit 1
shared=()
for object in "${objects[@]}"; do
  case "$object" in *linuxu_tests_test_kfd_session.c.o) ;; *) shared+=("$object") ;; esac
done
clang "${cflags[@]}" "$test_dir/linuxu_tests_test_kfd_session.c.o" "${shared[@]}" \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_kfd_session"
"$test_dir/test_kfd_session"

# The dext's queue ABI as C++, with DriverKit's allocator from a stub.
mkdir -p "$test_dir/include/DriverKit"
cat > "$test_dir/include/DriverKit/IOLib.h" <<'HDR'
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
static inline void *IOMalloc(size_t size) { return malloc(size); }
static inline void *IOMallocZero(size_t size) { return calloc(1, size); }
static inline void IOFree(void *address, size_t size) { (void)size; free(address); }
static inline void IOSleep(uint64_t ms) { usleep((useconds_t)(ms * 1000)); }
HDR
cxxflags=(-std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1
  -fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections
  -I"$test_dir/include" -Idext/sources -Ilinuxu/tests -idirafter linuxu/headers)
clang++ "${cxxflags[@]}" -x c++ -c dext/sources/dext_kfd.mm -o "$test_dir/dext_kfd.o"
clang++ "${cxxflags[@]}" -c linuxu/tests/test_dext_kfd.cpp -o "$test_dir/test_dext_kfd.o"
clang++ -fsanitize=address,undefined "$test_dir/test_dext_kfd.o" "$test_dir/dext_kfd.o" \
  "${shared[@]}" -Wl,-dead_strip -lpthread -o "$test_dir/test_dext_kfd"
"$test_dir/test_dext_kfd"
