#!/usr/bin/env bash
# Upstream kfd_create_process on a linuxu process: the unmodified KFD process
# lifecycle (open, mmu notifier registration, lookup by mm, exit_mm release,
# descriptor close, deferred free) on the linuxu process substrate.
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
	@printf '%s\n' $(filter-out -MMD -MP,$(HOSTCFLAGS))
MAKE
include_string=$(make -s -f Makefile -f "$test_dir/flags.mk" print-includes | tr '\n' ' ')
read -r -a includes <<< "$include_string"
host_string=$(make -s -f Makefile -f "$test_dir/flags.mk" print-hostcflags | tr '\n' ' ')
read -r -a hostcflags <<< "$host_string"
cflags=("${hostcflags[@]}" -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections "${includes[@]}")
# Upstream KFD: the process lifecycle and the per-process services it
# initializes and tears down (events, queue manager, apertures, debug trap
# state, SMI events, doorbells, debugfs). The test supplies a one-node
# topology and tripwires for every device-only entry point.
sources=(linuxu/tests/test_kfd_process.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_process.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_events.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_flat_memory.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_debug.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_smi_events.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_doorbell.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_debugfs.c
  linuxu/src/shims/process.c linuxu/src/shims/task.c linuxu/src/shims/fd.c
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
for source in "${sources[@]}"; do
  object="$test_dir/$(echo "$source" | tr '/' '_').o"
  extra=()
  # Same per-object flags as the Makefile.
  case "$source" in
    third_party/linux/lib/kfifo.c) extra=(-include linux/kernel.h -include linux/bug.h) ;;
    third_party/linux/lib/sort.c) extra=(-include linux/compiler.h -include linux/preempt.h) ;;
  esac
  clang "${cflags[@]}" "${extra[@]}" -c "$source" -o "$object"
  objects+=("$object")
done
clang "${cflags[@]}" "${objects[@]}" -Wl,-dead_strip -lpthread \
  -o "$test_dir/test_kfd_process"
"$test_dir/test_kfd_process"
