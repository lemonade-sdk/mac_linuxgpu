#!/usr/bin/env bash
# Compute power transitions (linuxu/src/amdgpu-rt/power.c) through upstream
# amdkfd's PM entry points, kgd2kfd_suspend/kgd2kfd_resume from the
# unmodified kfd_device.c, over the KFD session fixture: a KFD process with
# a MES queue whose AQL work the fixture's command processor executes.
#
# kfd_device.c is linked for its PM half and its kfd_is_locked; the
# fixture keeps its own stand-ins for the device-init helpers kfd_device.c
# also defines (they need kgd2kfd_device_init's GTT sub-allocator), so this
# build gives kfd_device.c's copies other names.
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
# As test-kfd-session.sh: plain memory MMIO, UBSan without shift-base.
cflags=("${hostcflags[@]}" -g -O1 -fsanitize=address,undefined -fno-sanitize=shift-base
  -fno-sanitize-recover=all -DFIXTURE_UPSTREAM_KFD_DEVICE
  -ffunction-sections -fdata-sections "${includes[@]}")
amdkfd=third_party/linux/drivers/gpu/drm/amd/amdkfd
sources=(linuxu/tests/test_kfd_power.c linuxu/tests/kfd_session_fixture.c linuxu/src/kmem/device_string.c
  linuxu/src/amdgpu-rt/power.c linuxu/src/amdgpu-rt/tmpring_gc12.c
  linuxu/src/amdgpu-rt/kfd_session.c linuxu/src/amdgpu-rt/process_file.c
  linuxu/src/amdgpu-rt/lx_files.c linuxu/src/amdgpu-rt/lx_frame.c linuxu/src/amdgpu-rt/lx_describe.c linuxu/src/amdgpu-rt/lx_timing.c
  $amdkfd/kfd_device.c
  $amdkfd/kfd_chardev.c $amdkfd/kfd_process.c $amdkfd/kfd_process_queue_manager.c
  $amdkfd/kfd_device_queue_manager.c $amdkfd/kfd_device_queue_manager_v12.c
  $amdkfd/kfd_mqd_manager.c $amdkfd/kfd_mqd_manager_v12.c
  $amdkfd/kfd_queue.c $amdkfd/kfd_doorbell.c $amdkfd/kfd_flat_memory.c
  $amdkfd/kfd_events.c $amdkfd/kfd_debug.c $amdkfd/kfd_smi_events.c $amdkfd/kfd_debugfs.c $amdkfd/kfd_int_process_v11.c
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
  case "$source" in
    # kmemcheck tracks the heap (the leak check below): debug build.
    linuxu/src/kmem/kmemalloc.c|linuxu/src/kmem/kmemcheck.c) extra=(-DDEBUG=1) ;;
    third_party/linux/lib/kfifo.c) extra=(-include linux/kernel.h -include linux/bug.h) ;;
    third_party/linux/lib/sort.c) extra=(-include linux/compiler.h -include linux/preempt.h) ;;
    */kfd_device.c) extra=(-Dkfd_gtt_sa_allocate=upstream_kfd_gtt_sa_allocate
      -Dkfd_gtt_sa_free=upstream_kfd_gtt_sa_free
      -Dkfd_get_num_sdma_engines=upstream_kfd_get_num_sdma_engines
      -Dkfd_get_num_xgmi_sdma_engines=upstream_kfd_get_num_xgmi_sdma_engines
      -Dkfd_inc_compute_active=upstream_kfd_inc_compute_active
      -Dkfd_dec_compute_active=upstream_kfd_dec_compute_active
      -Dkfd_debugfs_hang_hws=upstream_kfd_debugfs_hang_hws) ;;
  esac
  clang "${cflags[@]}" ${extra[@]+"${extra[@]}"} -c "$source" -o "$object" &
  pids+=($!)
  objects+=("$object")
done
failed=0
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
[ "$failed" = 0 ] || exit 1
clang "${cflags[@]}" "${objects[@]}" -Wl,-dead_strip -lpthread -o "$test_dir/test_kfd_power"
"$test_dir/test_kfd_power"
