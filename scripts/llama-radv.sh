#!/usr/bin/env bash
#
# scripts/llama-radv.sh — run a llama.cpp tool (scripts/build-llama-vulkan.sh)
# on the GPU through RADV (scripts/build-radv.sh) and mac_linuxgpu.
#
#   scripts/llama-radv.sh llama-bench -m model.gguf
#   scripts/llama-radv.sh test-backend-ops -o MUL_MAT
#   scripts/llama-radv.sh vulkaninfo --summary      (any program works)
#
# The Vulkan loader is pointed at the RADV ICD alone (VK_DRIVER_FILES), so a
# MoltenVK installation is not used. RADV_DEBUG, MESA_* and GGML_VK_*
# variables pass through. MLG_PRELOAD names a library to insert into the
# tool alone (DYLD_INSERT_LIBRARIES does not survive the system shell);
# scripts/test-llama-offline.sh uses it to bring up the fixture device.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ICD="$ROOT/build/radv/install/share/vulkan/icd.d/radeon_icd.json"

[[ $# -ge 1 ]] || { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[[ -f "$ICD" ]] || { echo "llama-radv: $ICD missing; run scripts/build-radv.sh" >&2; exit 1; }

tool="$1"
shift
if [[ "$tool" != */* && -x "$ROOT/build/llama.cpp/bin/$tool" ]]; then
  tool="$ROOT/build/llama.cpp/bin/$tool"
fi
export VK_DRIVER_FILES="$ICD"
unset VK_ICD_FILENAMES
if [[ -n "${MLG_PRELOAD:-}" ]]; then
  export DYLD_INSERT_LIBRARIES="$MLG_PRELOAD"
fi
exec "$tool" "$@"
