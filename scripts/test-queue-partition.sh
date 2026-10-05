#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
cat > "$test_dir/flags.mk" <<'MAKE'
.PHONY: print-includes
print-includes:
	@printf '%s\n' $(INCPATHS)
MAKE
include_string=$(make -s -f Makefile -f "$test_dir/flags.mk" print-includes | tr '\n' ' ')
read -r -a includes <<< "$include_string"
# Upstream KFD's kernel-queue init (init_mqd_hiq) shifts 1 into bit 31 as
# int, which the kernel build tolerates; UBSan's shift check stays on for the
# code under test.
printf '[shift-base]\nsrc:third_party/linux/drivers/gpu/drm/amd/amdkfd/*\n' > "$test_dir/ignore.txt"
cflags=(-w -std=gnu11 -D__KERNEL__ -include linux/autoconf.h
  -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
  -fsanitize-ignorelist="$test_dir/ignore.txt"
  -ffunction-sections -fdata-sections "${includes[@]}")
# Upstream amdgpu_amdkfd.c derives KFD's cp_queue_bitmap; the DQM file
# supplies KFD's queue accounting; the per-family DQM and MQD manager files
# build the MQD; gfx_v11_0.c / gfx_v12_0.c (through queue_partition_gfx*.c)
# provide amdgpu's kernel compute queue MQD builders. Unreferenced upstream
# code is stripped.
sources=(linuxu/tests/test_queue_partition.c linuxu/src/kmem/device_string.c
  third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager_v11.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager_v12.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager.c third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c
  third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c
  linuxu/tests/queue_partition_gfx11.c linuxu/tests/queue_partition_gfx12.c
  linuxu/src/amdgpu-rt/tmpring_gc11.c linuxu/src/amdgpu-rt/tmpring_gc12.c
  linuxu/src/amdgpu-rt/tmpring_gc12_1.c
  linuxu/src/shims/bitmap.c linuxu/src/shims/printk.c linuxu/src/bug.c
  linuxu/src/sync.c linuxu/src/delay.c
  linuxu/src/mm/uaccess.c linuxu/src/mm/mm.c linuxu/src/shims/task.c
  linuxu/src/rwsem.c)
objects=()
for source in "${sources[@]}"; do
  object="$test_dir/$(echo "$source" | tr '/' '_').o"
  clang "${cflags[@]}" -c "$source" -o "$object"
  objects+=("$object")
done
build() { # queue.c variant, output binary
  clang "${cflags[@]}" -c "$1" -o "$test_dir/queue.o"
  clang "${cflags[@]}" "${objects[@]}" "$test_dir/queue.o" \
    -Wl,-dead_strip -lpthread -o "$2"
}
build linuxu/src/amdgpu-rt/queue.c "$test_dir/test_queue_partition"
"$test_dir/test_queue_partition"

# Mutation check: undoing any legacy kernel queue fixup in queue.c (each is
# tagged "legacy-fixup: <name>") must make the test fail.
mutant_dir="$test_dir/mutant"
mkdir -p "$mutant_dir"
for fixup in doorbell qswitch debug privilege; do
  tag="legacy-fixup: $fixup"
  grep -q "$tag" linuxu/src/amdgpu-rt/queue.c || { echo "missing fixup tag: $tag"; exit 1; }
  if [ "$fixup" = privilege ]; then
    # Build with KFD's user-queue manager instead of its kernel-queue one.
    sed "/$tag/s/KFD_MQD_TYPE_HIQ/KFD_MQD_TYPE_CP/" linuxu/src/amdgpu-rt/queue.c \
      > "$mutant_dir/queue.c"
  else
    sed "/$tag/d" linuxu/src/amdgpu-rt/queue.c > "$mutant_dir/queue.c"
  fi
  cmp -s linuxu/src/amdgpu-rt/queue.c "$mutant_dir/queue.c" && { echo "mutation $fixup changed nothing"; exit 1; }
  build "$mutant_dir/queue.c" "$mutant_dir/test_queue_partition"
  # A subshell keeps the expected abort's job message out of the output.
  if bash -c '"$0"; exit $?' "$mutant_dir/test_queue_partition" >"$mutant_dir/log" 2>&1; then
    echo "mutation without the $fixup fixup still passes"; exit 1
  fi
done
echo "queue partition mutation check: removing any legacy kernel queue fixup fails the test"
