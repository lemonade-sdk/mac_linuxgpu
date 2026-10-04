#!/usr/bin/env bash
#
# scripts/build-quake3e.sh — build Quake3e (github.com/ec-/Quake3e) for
# macOS arm64 with its Vulkan renderer only, the first game run on RADV
# with the GPU's own scanout (build/mlg-run --quake3).
#
#   1. Fetches the pinned commit into build/quake3e/src (shallow).
#   2. Builds the client alone, Vulkan only (no OpenGL renderer, so no
#      path to the Apple GPU), with the SDL2 it bundles for macOS.
#   3. Puts quake3e.aarch64 and its libSDL2-2.0.0.dylib in build/quake3e.
#
# Game data is not fetched and never goes in the repository: Quake III
# Arena's baseq3 from an installation, or OpenArena 0.8.8 (free, from
# openarena.ws / SourceForge) unpacked outside the tree, which Quake3e runs
# as fs_basegame baseoa (mlg-run --basepath DIR --basegame baseoa).
#
# Options:
#   --clean   build from scratch
#   -h        show this help
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

URL=https://github.com/ec-/Quake3e.git
PIN=2b375bd1e29a86cefce5d5ff1f67909ff9cfa497	# 2026-09-22

clean=0
for arg in "$@"; do
  case "$arg" in
    --clean) clean=1 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-quake3e: unknown argument: $arg (see --help)" >&2; exit 2 ;;
  esac
done

step() { printf '\n==> %s\n' "$*"; }
die() { printf '\nbuild-quake3e: error: %s\n' "$*" >&2; exit 1; }

OUT="$ROOT/build/quake3e"
SRC="$OUT/src"

step "Quake3e $PIN"
if [[ ! -d "$SRC/.git" ]]; then
  mkdir -p "$SRC"
  git -C "$SRC" init --quiet
  git -C "$SRC" remote add origin "$URL"
fi
if [[ "$(git -C "$SRC" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$PIN" ]]; then
  git -C "$SRC" fetch --quiet --depth 1 origin "$PIN" || die "fetching $PIN from $URL failed"
  git -C "$SRC" checkout --quiet --detach FETCH_HEAD
fi
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$PIN" ]] || die "$SRC is not at $PIN"
[[ -z "$(git -C "$SRC" status --porcelain)" ]] || die "$SRC has local changes"

step "Building the client (Vulkan renderer only)"
args=(BUILD_SERVER=0 BUILD_CLIENT=1 USE_RENDERER_DLOPEN=0 USE_OPENGL=0 USE_OPENGL2=0
      USE_VULKAN=1 RENDERER_DEFAULT=vulkan ARCH=aarch64)
(( clean )) && make -C "$SRC" "${args[@]}" clean >/dev/null
make -C "$SRC" -j"$(sysctl -n hw.ncpu)" release "${args[@]}"

bin="$SRC/build/release-darwin-aarch64/quake3e.aarch64"
[[ -x "$bin" ]] || die "no $bin after the build"
mkdir -p "$OUT"
install -m 755 "$bin" "$OUT/"
install -m 644 "$SRC/code/libsdl/macosx/libSDL2-2.0.0.dylib" "$OUT/"
# The client finds SDL next to itself.
otool -L "$OUT/quake3e.aarch64" | grep -q '@executable_path/libSDL2-2.0.0.dylib' ||
  die "quake3e.aarch64 does not load SDL from next to itself"
if otool -L "$OUT/quake3e.aarch64" | grep -qi -e 'OpenGL' -e 'MoltenVK'; then
  die "quake3e.aarch64 links OpenGL or MoltenVK"
fi
codesign --force --sign - "$OUT/libSDL2-2.0.0.dylib" "$OUT/quake3e.aarch64" 2>/dev/null

step "Quake3e built"
echo "    $OUT/quake3e.aarch64"
echo "    run: build/mlg-run --quake3 --mode fullscreen --basepath <dir with baseoa> --basegame baseoa -- $OUT/quake3e.aarch64"
