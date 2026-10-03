#!/usr/bin/env bash
#
# scripts/build-radv.sh — build Mesa's RADV Vulkan driver for macOS (arm64)
# on mac_linuxgpu, and package it as a Vulkan ICD.
#
#   1. Sets up the pinned Mesa (third_party/mesa, patches/mesa/) with
#      scripts/bootstrap.sh --mesa-only.
#   2. Builds libdrm-mlg (make libdrm-mlg): libdrm and libdrm_amdgpu over
#      libmlg_drm, with the pkg-config files Mesa looks for.
#   3. Configures and builds RADV with Meson: the ACO compiler and no LLVM,
#      no window-system platforms (headless WSI only), no GL, no video.
#   4. Installs into build/radv/install:
#        lib/libvulkan_radeon.dylib, lib/libdrm_mlg.dylib, lib/libmlg_drm.dylib
#        share/vulkan/icd.d/radeon_icd.json   (library path relative to it)
#      and checks that every symbol the driver needs resolves.
#
# Use it with the Vulkan loader (Homebrew vulkan-loader or the LunarG SDK):
#   VK_DRIVER_FILES=$PWD/build/radv/install/share/vulkan/icd.d/radeon_icd.json vulkaninfo --summary
#
# Needs: meson, ninja, pkg-config and glslangValidator (brew install meson
# ninja pkg-config glslang), and Python with mako, PyYAML and packaging (a
# virtualenv with them is created in build/radv/venv when python3 lacks them).
#
# Options:
#   --debug        a debug build (default: release)
#   --reconfigure  run meson setup again
#   -h, --help     show this help

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

buildtype=release
reconfigure=0
for arg in "$@"; do
  case "$arg" in
    --debug) buildtype=debug ;;
    --reconfigure) reconfigure=1 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-radv: unknown argument: $arg (see --help)" >&2; exit 2 ;;
  esac
done

step() { printf '\n==> %s\n' "$*"; }
die() { printf '\nbuild-radv: error: %s\n' "$*" >&2; exit 1; }

OUT="$ROOT/build/radv"
BUILD="$OUT/build"
PREFIX="$OUT/install"
LIBDRM="$ROOT/build/libdrm-mlg"

for tool in meson ninja pkg-config glslangValidator; do
  command -v "$tool" >/dev/null || die "$tool not found (brew install meson ninja pkg-config glslang)"
done

step "Mesa (third_party/mesa)"
bash scripts/bootstrap.sh --mesa-only

step "Python modules for Mesa's generators"
python=python3
if ! python3 -c 'import mako, yaml, packaging' 2>/dev/null; then
  if [[ ! -x "$OUT/venv/bin/python" ]] || ! "$OUT/venv/bin/python" -c 'import mako, yaml, packaging' 2>/dev/null; then
    echo "    creating $OUT/venv with mako, PyYAML and packaging"
    python3 -m venv "$OUT/venv"
    "$OUT/venv/bin/pip" install --quiet mako pyyaml packaging
  fi
  python="$OUT/venv/bin/python"
  export PATH="$OUT/venv/bin:$PATH"
fi
echo "    $("$python" -c 'import sys, mako, yaml; print("python", sys.version.split()[0], "mako", mako.__version__, "PyYAML", yaml.__version__)')"

step "libdrm-mlg"
make -s libdrm-mlg
export PKG_CONFIG_PATH="$LIBDRM/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
echo "    libdrm $(pkg-config --modversion libdrm) from $(pkg-config --variable=prefix libdrm)"

step "Configuring RADV ($buildtype)"
options=(
  --buildtype="$buildtype" --prefix="$PREFIX" --libdir=lib
  -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dllvm=disabled
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Degl=disabled
  -Dgbm=disabled -Dvulkan-layers= -Dtools= -Dvideo-codecs= -Dbuild-tests=false
  -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dspirv-tools=disabled
)
if [[ ! -f "$BUILD/build.ninja" ]]; then
  meson setup "$BUILD" third_party/mesa "${options[@]}"
elif (( reconfigure )); then
  meson setup --reconfigure "$BUILD" third_party/mesa "${options[@]}"
fi

step "Building RADV"
ninja -C "$BUILD" src/amd/vulkan/libvulkan_radeon.dylib

step "Installing into build/radv/install"
mkdir -p "$PREFIX/lib" "$PREFIX/share/vulkan/icd.d"
install -m 755 "$BUILD/src/amd/vulkan/libvulkan_radeon.dylib" "$PREFIX/lib/"
install -m 755 "$LIBDRM/lib/libdrm_mlg.dylib" "$LIBDRM/lib/libmlg_drm.dylib" "$PREFIX/lib/"
# The driver finds libdrm-mlg next to itself, wherever the tree is moved.
lib="$PREFIX/lib/libvulkan_radeon.dylib"
while read -r rpath; do
  install_name_tool -delete_rpath "$rpath" "$lib" 2>/dev/null || true
done < <(otool -l "$lib" | awk '/LC_RPATH/ { getline; getline; print $2 }')
install_name_tool -add_rpath @loader_path "$lib" 2>/dev/null
for dylib in "$lib" "$PREFIX/lib/libdrm_mlg.dylib" "$PREFIX/lib/libmlg_drm.dylib"; do
  codesign --force --sign - "$dylib" 2>/dev/null
done
api_version=$("$python" - "$BUILD/src/amd/vulkan" <<'PY'
import glob, json, sys
for path in glob.glob(sys.argv[1] + "/radeon_*icd*.json"):
    print(json.load(open(path))["ICD"]["api_version"]); break
else:
    print("1.4.335")
PY
)
cat > "$PREFIX/share/vulkan/icd.d/radeon_icd.json" <<EOF
{
    "ICD": {
        "api_version": "$api_version",
        "library_path": "../../../lib/libvulkan_radeon.dylib"
    },
    "file_format_version": "1.0.1"
}
EOF

step "Checking the driver's symbols"
# The driver links with -undefined dynamic_lookup (patches/mesa); only the
# weak, optional layer entrypoints may be left to it.
unresolved=$(nm -mu "$lib" | grep 'dynamically looked up' | grep -v ' weak ' || true)
[[ -z "$unresolved" ]] || { echo "$unresolved" >&2; die "libvulkan_radeon.dylib has unresolved symbols"; }
foreign=$(otool -L "$lib" | awk 'NR > 1 { print $1 }' | grep -v -e '^/usr/lib/' -e '^/System/' \
  -e '^@rpath/libvulkan_radeon' -e '^@rpath/libdrm_mlg.dylib' || true)
[[ -z "$foreign" ]] || { echo "$foreign" >&2; die "libvulkan_radeon.dylib links libraries outside the package"; }
echo "    every symbol resolves; links only the system and libdrm-mlg"

step "RADV built"
echo "    $PREFIX/lib/libvulkan_radeon.dylib"
echo "    ICD: $PREFIX/share/vulkan/icd.d/radeon_icd.json (api $api_version)"
echo "    try: VK_DRIVER_FILES=$PREFIX/share/vulkan/icd.d/radeon_icd.json vulkaninfo --summary"
