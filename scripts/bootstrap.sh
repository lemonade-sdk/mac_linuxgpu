#!/usr/bin/env bash
#
# scripts/bootstrap.sh — prepare a clone for building. Safe to re-run.
#
#   1. Checks the tools the build uses.
#   2. Fetches the pinned upstream Linux submodule (third_party/linux) as a
#      shallow, partial, sparse checkout of only the paths the build reads.
#   3. Applies the declared patches in patches/linux/ to the submodule
#      working tree (already-applied patches are detected and skipped).
#   4. Fetches and verifies the locked linux-firmware files (build/firmware).
#   5. Writes the setup stamp the Makefile checks (build/setup/).
#
# `make`, scripts/activate.sh and the Xcode build run this automatically when
# the stamp is missing or stale.
#
# Options:
#   --skip-firmware   do not fetch firmware (offline builds with EMBED_FIRMWARE=0)
#   --reset-linux     discard every change in the submodule working tree, then
#                     re-apply the patches (use after editing a patch)
#   --from-make       quieter tool check; used by the Makefile
#   -h, --help        show this help

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

skip_firmware=0
reset_linux=0
from_make=0
for arg in "$@"; do
  case "$arg" in
    --skip-firmware) skip_firmware=1 ;;
    --reset-linux) reset_linux=1 ;;
    --from-make) from_make=1 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "bootstrap: unknown argument: $arg (see --help)" >&2; exit 2 ;;
  esac
done
[[ -z "${MAC_LINUXGPU_SKIP_FIRMWARE:-}" ]] || skip_firmware=1

step() { printf '\n==> %s\n' "$*"; }
say() { printf '    %s\n' "$*"; }
warn() { printf '    warning: %s\n' "$*" >&2; }
die() { printf '\nbootstrap: error: %s\n' "$*" >&2; exit 1; }

command -v python3 >/dev/null || die "python3 is required (install the Xcode command line tools)"
command -v git >/dev/null || die "git is required (install the Xcode command line tools)"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
  die "not a git checkout; clone the repository with git so the submodule can be fetched"

manifest() { # dotted key -> JSON value as text (one line per list entry)
  python3 - "$1" <<'PY'
import json, sys
value = json.load(open("patches/manifest.json"))
for key in sys.argv[1].split("."):
    value = value[key]
if isinstance(value, list):
    for item in value:
        print(item if isinstance(item, str) else json.dumps(item))
else:
    print(value)
PY
}

SUB="$(manifest linux.path)"
PIN="$(manifest linux.pin)"

# ---------------------------------------------------------------------------
step "Checking tools"
# ---------------------------------------------------------------------------
missing=0
for tool in clang make curl shasum awk; do
  command -v "$tool" >/dev/null || { warn "$tool not found"; missing=1; }
done
(( missing == 0 )) || die "install the Xcode command line tools (xcode-select --install)"
say "python3 $(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])'), $(git --version), $(clang --version | head -1)"
if xcode_path="$(xcode-select -p 2>/dev/null)" && [[ "$xcode_path" == *.app/* ]]; then
  say "Xcode: $(xcodebuild -version 2>/dev/null | head -1) ($xcode_path)"
  if dk_sdk="$(xcrun --sdk driverkit --show-sdk-path 2>/dev/null)"; then
    say "DriverKit SDK: $dk_sdk"
  else
    warn "DriverKit SDK not found; make lib-dext, make dext and the Xcode build need it"
  fi
else
  warn "full Xcode not selected (xcode-select -p: ${xcode_path:-none}); the host"
  warn "library and tests build, but lib-dext, the dext and the app need Xcode"
fi
if (( ! from_make )); then
  if command -v cmake >/dev/null; then
    say "cmake: $(cmake --version | head -1) (for make hsa)"
  else
    warn "cmake not found; make hsa / make hsa-test need it (brew install cmake)"
  fi
  command -v rg >/dev/null || warn "ripgrep (rg) not found; some test scripts use it (brew install ripgrep)"
  say "LLVM with the AMDGPU target is only needed to regenerate"
  say "hsa/src/signal_kernels_code.h (hsa/tools/build-signal-kernels.sh)"
fi

# ---------------------------------------------------------------------------
step "Upstream Linux submodule ($SUB @ ${PIN:0:12})"
# ---------------------------------------------------------------------------
gitlink="$(git ls-files -s -- "$SUB" | awk '{ print $2 }')"
[[ -n "$gitlink" ]] || die "$SUB is not a submodule in this checkout"
[[ "$gitlink" == "$PIN" ]] ||
  die "$SUB is recorded at $gitlink but patches/manifest.json pins $PIN; update one to match"

git submodule init -- "$SUB" >/dev/null
url="$(git config "submodule.$SUB.url")"
gitdir="$(git rev-parse --git-path "modules/$SUB")"
sparse_file="$(mktemp)"
trap 'rm -f "$sparse_file"' EXIT
manifest linux.sparse_checkout > "$sparse_file"

# Prepare the submodule repository before git checks anything out, so the
# first checkout is already sparse and fetches only the blobs it needs.
configure_gitdir() {
  git --git-dir="$gitdir" config extensions.partialClone origin
  git --git-dir="$gitdir" config remote.origin.promisor true
  git --git-dir="$gitdir" config remote.origin.partialCloneFilter blob:none
  git --git-dir="$gitdir" config remote.origin.tagOpt --no-tags
  git --git-dir="$gitdir" config core.sparseCheckout true
  git --git-dir="$gitdir" config core.sparseCheckoutCone false
  mkdir -p "$gitdir/info"
  cp "$sparse_file" "$gitdir/info/sparse-checkout"
}

# In a partial clone, asking git about a commit it does not have makes it
# fetch that commit lazily together with its entire history. So the pinned
# commit is fetched first, shallow and by SHA, into a ref; `git submodule
# update --depth 1` then finds it locally and only checks it out, fetching
# just the blobs of the sparse set.
fetch_pin() {
  say "fetching $PIN (shallow, by SHA; the first run downloads about 70 MB)"
  git --git-dir="$gitdir" fetch --quiet --depth 1 --filter=blob:none --no-tags \
    origin "+$PIN:refs/remotes/origin/pinned"
}

update_submodule() {
  if fetch_pin; then
    git submodule update --init --depth 1 -- "$SUB" ||
      die "git submodule update failed for $SUB"
  else
    # The server refused a fetch by SHA: let git locate the commit itself.
    # This downloads the full commit history (without file contents).
    warn "fetching the pinned commit by SHA failed; falling back to a full-history submodule update"
    git submodule update --init -- "$SUB" || die "could not fetch $PIN from $url"
  fi
}

if [[ ! -e "$SUB/.git" ]]; then
  if [[ ! -d "$gitdir" ]]; then
    say "creating a partial, sparse repository for $url"
    mkdir -p "$(dirname "$gitdir")"
    git init --quiet --bare "$gitdir"
    git --git-dir="$gitdir" config core.bare false
    git --git-dir="$gitdir" remote add origin "$url"
  fi
  configure_gitdir
  update_submodule
fi

[[ -e "$SUB/.git" ]] || die "$SUB was not populated"
# A plain `git clone --recurse-submodules` or `git submodule update --init`
# produces a full checkout. On case-insensitive APFS that checkout already
# shows the kernel's case-only filename pairs as modified, and
# `sparse-checkout reapply` keeps modified files. Clear the working tree once
# and check out only the sparse set from the objects already downloaded.
was_sparse="$(git --git-dir="$gitdir" config --bool core.sparseCheckout 2>/dev/null || true)"
configure_gitdir
if [[ "$was_sparse" != "true" ]]; then
  say "converting the full checkout of $SUB to the sparse set"
  git -C "$SUB" ls-files -z | (cd "$SUB" && xargs -0 rm -f)
  git -C "$SUB" reset --quiet --hard
fi
head="$(git -C "$SUB" rev-parse HEAD 2>/dev/null || true)"
if [[ -n "$head" ]] && { (( reset_linux )) || [[ "$head" != "$PIN" ]]; }; then
  if [[ -n "$(git -C "$SUB" status --porcelain --untracked-files=no)" ]]; then
    say "discarding working-tree changes in $SUB (patches are re-applied below)"
  fi
  git -C "$SUB" reset --quiet --hard
fi
if [[ "$head" != "$PIN" ]]; then
  say "moving $SUB from ${head:-nothing} to $PIN"
  update_submodule
fi
# Apply the sparse pattern set (a no-op when it is unchanged).
git -C "$SUB" sparse-checkout reapply
[[ "$(git -C "$SUB" rev-parse HEAD)" == "$PIN" ]] || die "$SUB is not at $PIN"
say "checked out $(git -C "$SUB" ls-files -t | grep -c '^H ') files ($(du -sh "$SUB" | cut -f1))"

# ---------------------------------------------------------------------------
step "Applying patches (patches/linux)"
# ---------------------------------------------------------------------------
applied=0
already=0
while IFS= read -r patch; do
  [[ -n "$patch" ]] || continue
  [[ -f "$patch" ]] || die "declared patch missing: $patch"
  if git -C "$SUB" apply --check "$ROOT/$patch" 2>/dev/null; then
    git -C "$SUB" apply "$ROOT/$patch"
    say "applied $(basename "$patch")"
    applied=$((applied + 1))
  elif git -C "$SUB" apply --check --reverse "$ROOT/$patch" 2>/dev/null; then
    already=$((already + 1))
  else
    die "$patch neither applies nor is already applied; run scripts/bootstrap.sh --reset-linux"
  fi
done < <(python3 -c 'import json
for p in json.load(open("patches/manifest.json"))["patches"]: print(p["patch"])')
say "$applied applied, $already already present"

# ---------------------------------------------------------------------------
step "Firmware (firmware/firmware.lock)"
# ---------------------------------------------------------------------------
if (( skip_firmware )); then
  say "skipped (--skip-firmware); build with EMBED_FIRMWARE=0 or run scripts/fetch-firmware.sh later"
else
  bash scripts/fetch-firmware.sh
fi

# ---------------------------------------------------------------------------
step "Verifying the upstream tree"
# ---------------------------------------------------------------------------
python3 scripts/verify-upstream.py

mkdir -p build/setup
rm -f build/setup/bootstrap-*.mk
{
  echo "# Written by scripts/bootstrap.sh; make re-runs setup when this file is"
  echo "# missing or older than patches/, firmware/firmware.lock or the scripts."
  echo "SETUP_LINUX_PIN := $PIN"
} > "build/setup/bootstrap-$PIN.mk"
step "Setup complete"
say "next: make lib-dext (DriverKit library), make test, or scripts/activate.sh"
