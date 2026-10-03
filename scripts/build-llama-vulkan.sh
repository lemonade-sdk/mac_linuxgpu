#!/usr/bin/env bash
#
# scripts/build-llama-vulkan.sh — build the pinned llama.cpp
# (third_party/llama.cpp) with its Vulkan backend for macOS, against the
# Vulkan loader, to run on RADV (scripts/build-radv.sh) over mac_linuxgpu.
#
#   1. Sets up third_party/llama.cpp (scripts/bootstrap.sh --llama-only).
#   2. Builds RADV first when build/radv/install has no driver.
#   3. Configures llama.cpp with CMake: GGML_VULKAN on; Metal, BLAS and
#      libcurl off, so every GPU operation goes through Vulkan.
#   4. Builds llama-bench, llama-completion and test-backend-ops into
#      build/llama.cpp/bin.
#
# Run them on RADV with scripts/llama-radv.sh, which selects the RADV ICD
# alone (not MoltenVK):
#   scripts/llama-radv.sh llama-bench -m model.gguf
#
# Needs: cmake, the Vulkan loader and headers, and shaderc's glslc (brew
# install cmake vulkan-loader vulkan-headers shaderc), or the LunarG Vulkan
# SDK with VULKAN_SDK set.
#
# Options:
#   --reconfigure  run the CMake configuration again
#   -h, --help     show this help

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

reconfigure=0
for arg in "$@"; do
  case "$arg" in
    --reconfigure) reconfigure=1 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-llama-vulkan: unknown argument: $arg (see --help)" >&2; exit 2 ;;
  esac
done

step() { printf '\n==> %s\n' "$*"; }
die() { printf '\nbuild-llama-vulkan: error: %s\n' "$*" >&2; exit 1; }

SRC="$ROOT/third_party/llama.cpp"
BUILD="$ROOT/build/llama.cpp"
ICD="$ROOT/build/radv/install/share/vulkan/icd.d/radeon_icd.json"

command -v cmake >/dev/null || die "cmake not found (brew install cmake)"
command -v glslc >/dev/null || [[ -x "${VULKAN_SDK:-/nonexistent}/bin/glslc" ]] ||
  die "glslc not found: llama.cpp compiles its Vulkan shaders with shaderc's glslc (brew install shaderc)"

# The Vulkan loader and headers: the SDK's, else Homebrew's.
vk_args=()
if [[ -n "${VULKAN_SDK:-}" ]]; then
  vk_args+=(-DVulkan_INCLUDE_DIR="$VULKAN_SDK/include" -DVulkan_LIBRARY="$VULKAN_SDK/lib/libvulkan.dylib")
  [[ -x "$VULKAN_SDK/bin/glslc" ]] && vk_args+=(-DVulkan_GLSLC_EXECUTABLE="$VULKAN_SDK/bin/glslc")
elif brew_prefix="$(brew --prefix 2>/dev/null)" && [[ -f "$brew_prefix/lib/libvulkan.dylib" ]]; then
  [[ -f "$brew_prefix/include/vulkan/vulkan.h" ]] ||
    die "Vulkan headers not found (brew install vulkan-headers)"
  vk_args+=(-DVulkan_INCLUDE_DIR="$brew_prefix/include" -DVulkan_LIBRARY="$brew_prefix/lib/libvulkan.dylib")
else
  die "the Vulkan loader was not found (brew install vulkan-loader vulkan-headers, or set VULKAN_SDK)"
fi

step "llama.cpp (third_party/llama.cpp)"
bash scripts/bootstrap.sh --llama-only

if [[ ! -f "$ICD" ]]; then
  step "RADV (not built yet)"
  bash scripts/build-radv.sh
fi

step "Configuring llama.cpp (Vulkan backend)"
if [[ ! -f "$BUILD/CMakeCache.txt" ]] || (( reconfigure )); then
  cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DGGML_VULKAN=ON -DGGML_METAL=OFF -DGGML_BLAS=OFF -DGGML_NATIVE=ON \
    -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=ON -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=OFF -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$BUILD/bin" \
    "${vk_args[@]}"
fi

step "Building llama-bench, llama-completion and test-backend-ops"
cmake --build "$BUILD" --config Release -j "$(sysctl -n hw.ncpu)" \
  --target llama-bench llama-completion test-backend-ops

step "llama.cpp built"
for bin in llama-bench llama-completion test-backend-ops; do
  [[ -x "$BUILD/bin/$bin" ]] || die "$bin was not built"
  echo "    $BUILD/bin/$bin"
done
echo "    run on RADV: scripts/llama-radv.sh llama-bench -m <model.gguf>"
