#!/usr/bin/env bash
#
# scripts/fetch-firmware.sh — fetch linux-firmware files at a pinned commit.
#
# firmware/firmware.lock names a linux-firmware commit and the files the
# build needs, each with its SHA-256 and size. This script downloads them
# into build/firmware/ (linux-firmware layout) and verifies every file. The
# amdgpu images land in build/firmware/amdgpu/, together with copies of WHENCE
# and LICENSE.amdgpu, so that directory is a self-contained amdgpu firmware
# directory: the host app bundles it and the embedded fallback table
# (scripts/fw2rodata.py) reads it.
#
# Usage:
#   scripts/fetch-firmware.sh              fetch and verify the locked set
#   scripts/fetch-firmware.sh --check      verify what is present, no network
#   scripts/fetch-firmware.sh --add amdgpu/gc_11_0_0_mes.bin [...]
#                                          fetch more files at the pinned
#                                          commit and record them in the lock
#   scripts/fetch-firmware.sh --all        also fetch the whole amdgpu/
#                                          directory at the pinned commit into
#                                          build/firmware/full (git, sparse)
#   scripts/fetch-firmware.sh --relock     recompute every hash after the
#                                          lock's tag/commit was changed
#
# Environment: FIRMWARE_OUT overrides the output directory (build/firmware).

set -euo pipefail
cd "$(dirname "$0")/.."

LOCK=firmware/firmware.lock
OUT="${FIRMWARE_OUT:-build/firmware}"

say() { printf '    %s\n' "$*"; }
die() { printf 'fetch-firmware: error: %s\n' "$*" >&2; exit 1; }

[[ -f "$LOCK" ]] || die "missing $LOCK"
lock_value() { awk -v k="$1" '$1 == k { print $2; exit }' "$LOCK"; }
commit="$(lock_value commit)"
tag="$(lock_value tag)"
repo="$(lock_value repo)"
[[ "$commit" =~ ^[0-9a-f]{40}$ ]] || die "$LOCK: commit must be a 40-digit hash"
urls=()
while IFS= read -r template; do urls+=("$template"); done \
  < <(awk '$1 == "url" { print $2 }' "$LOCK")
(( ${#urls[@]} )) || die "$LOCK: no url templates"

sha256() { shasum -a 256 "$1" | awk '{ print $1 }'; }
size_of() { wc -c < "$1" | tr -d ' '; }

url_for() { # template path
  local url="${1//\{commit\}/$commit}"
  printf '%s' "${url//\{path\}/$2}"
}

# download <path> <dest> [mirror index]: try each mirror in order, or only
# the given one.
download() {
  local path="$1" dest="$2" only="${3:-}" i
  mkdir -p "$(dirname "$dest")"
  for i in "${!urls[@]}"; do
    [[ -z "$only" || "$only" == "$i" ]] || continue
    if curl -fsSL --retry 3 --connect-timeout 20 -o "$dest.part" "$(url_for "${urls[$i]}" "$path")"; then
      mv "$dest.part" "$dest"
      return 0
    fi
  done
  rm -f "$dest.part"
  return 1
}

matches() { # file sha size
  [[ -f "$1" && "$(size_of "$1")" == "$3" && "$(sha256 "$1")" == "$2" ]]
}

entries() { awk '$1 == "file" { print $2, $3, $4 }' "$LOCK"; }

# The amdgpu directory carries its own provenance and license copies.
finish_amdgpu_dir() {
  local dir="$OUT/amdgpu"
  mkdir -p "$dir"
  [[ ! -f "$OUT/WHENCE" ]] || cp "$OUT/WHENCE" "$dir/WHENCE"
  [[ ! -f "$OUT/LICENSES/LICENSE.amdgpu" ]] || cp "$OUT/LICENSES/LICENSE.amdgpu" "$dir/LICENSE.amdgpu"
}

fetch_locked() {
  local path sha size fetched=0 cached=0
  while read -r path sha size; do
    local dest="$OUT/$path"
    if matches "$dest" "$sha" "$size"; then
      cached=$((cached + 1)); continue
    fi
    download "$path" "$dest" || die "could not download $path at $commit"
    matches "$dest" "$sha" "$size" || {
      rm -f "$dest"
      die "$path: SHA-256 or size mismatch (expected $sha, $size bytes)"
    }
    fetched=$((fetched + 1))
  done < <(entries)
  finish_amdgpu_dir
  say "linux-firmware $tag ($commit): $fetched downloaded, $cached already present, all verified"
  say "amdgpu firmware directory: $OUT/amdgpu"
}

check_locked() {
  local path sha size bad=0
  while read -r path sha size; do
    if ! matches "$OUT/$path" "$sha" "$size"; then
      printf 'missing or modified: %s\n' "$OUT/$path" >&2; bad=1
    fi
  done < <(entries)
  (( bad == 0 )) || die "firmware check failed; run scripts/fetch-firmware.sh"
  say "firmware matches $LOCK"
}

add_files() {
  local name path tmp
  (( $# )) || die "--add needs at least one file name"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  for name in "$@"; do
    path="$name"
    [[ "$path" == */* ]] || path="amdgpu/$path"
    [[ "$path" != *..* && "$path" != /* ]] || die "invalid path: $name"
    if awk -v p="$path" '$1 == "file" && $2 == p { found = 1 } END { exit !found }' "$LOCK"; then
      say "$path is already locked"; continue
    fi
    download "$path" "$tmp/a" 0 || die "could not download $path at $commit"
    # Cross-check against the second mirror when it answers.
    if (( ${#urls[@]} > 1 )) && download "$path" "$tmp/b" 1; then
      cmp -s "$tmp/a" "$tmp/b" || die "$path differs between mirrors"
    else
      say "warning: $path was not cross-checked against a second mirror"
    fi
    mkdir -p "$(dirname "$OUT/$path")"
    mv "$tmp/a" "$OUT/$path"
    printf 'file  %s  %s  %s\n' "$path" "$(sha256 "$OUT/$path")" "$(size_of "$OUT/$path")" >> "$LOCK"
    say "added $path to $LOCK"
  done
  fetch_locked
}

relock() {
  local path sha size tmp new
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  new="$tmp/lock"
  awk '$1 != "file"' "$LOCK" > "$new"
  while read -r path sha size; do
    download "$path" "$tmp/f" || die "could not download $path at $commit"
    printf 'file  %s  %s  %s\n' "$path" "$(sha256 "$tmp/f")" "$(size_of "$tmp/f")" >> "$new"
  done < <(entries)
  cp "$new" "$LOCK"
  say "relocked $(entries | wc -l | tr -d ' ') files at $tag ($commit)"
  fetch_locked
}

fetch_all() {
  local full="$OUT/full"
  [[ -n "$repo" && -n "$tag" ]] || die "$LOCK needs repo and tag for --all"
  if [[ ! -d "$full/.git" ]]; then
    say "cloning linux-firmware $tag (amdgpu/ only) into $full"
    rm -rf "$full"
    git -c advice.detachedHead=false clone --quiet --depth 1 --filter=blob:none --sparse \
      --branch "$tag" "$repo" "$full"
    git -C "$full" sparse-checkout set --no-cone /amdgpu/ /LICENSES/LICENSE.amdgpu /WHENCE
  fi
  [[ "$(git -C "$full" rev-parse HEAD)" == "$commit" ]] ||
    die "$full is not at the locked commit $commit (tag $tag moved?)"
  cp "$full/WHENCE" "$full/amdgpu/WHENCE"
  cp "$full/LICENSES/LICENSE.amdgpu" "$full/amdgpu/LICENSE.amdgpu"
  local path sha size
  while read -r path sha size; do
    matches "$full/$path" "$sha" "$size" || die "$full/$path does not match $LOCK"
  done < <(entries)
  say "whole amdgpu directory: $full/amdgpu ($(find "$full/amdgpu" -type f | wc -l | tr -d ' ') files)"
  say "bundle it with: LINUX_FIRMWARE_DIR=$full scripts/activate.sh"
}

mode=fetch
case "${1:-}" in
  "") ;;
  --check) mode=check ;;
  --add) mode=add; shift ;;
  --all) mode=all ;;
  --relock) mode=relock ;;
  -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) die "unknown argument: $1 (see --help)" ;;
esac

case "$mode" in
  fetch) fetch_locked ;;
  check) check_locked ;;
  add) add_files "$@" ;;
  all) fetch_locked; fetch_all ;;
  relock) relock ;;
esac
