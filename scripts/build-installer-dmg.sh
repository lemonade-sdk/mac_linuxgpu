#!/usr/bin/env bash
# Build a distributable disk image containing the signed MacLinuxGPU host app.

set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
host_id="com.geramyloveless.MacAMDGPUHost"
dext_id="${host_id}.MacAMDGPU"
team_id="${XCODE_TEAM_ID:-YBQ9BU6Q6F}"
app_name="MacLinuxGPUHost.app"
app_path="$project_root/build/xcode/Build/Products/Release/$app_name"
output_dir="$project_root/build/installer"
skip_build=0
signing_identity="${SIGN_IDENTITY:-}"
identity_explicit=0
if [[ -n "${SIGN_IDENTITY:-}" ]]; then
  identity_explicit=1
fi

usage() {
  cat <<'EOF'
Usage: scripts/build-installer-dmg.sh [--no-build] [--identity IDENTITY] [--output PATH]

Build and sign the Release host app and embedded dext, then package the app in
a signed DMG. The host app presents the installation flow when opened.

The app bundles a whole amdgpu firmware directory (Contents/Resources/firmware,
installed to /Library/Application Support/MacLinuxGPU/firmware/amdgpu). Set
LINUX_FIRMWARE_DIR to a linux-firmware checkout to bundle the complete upstream
set (scripts/fetch-firmware.sh --all fetches one into build/firmware/full);
otherwise the locked set from firmware/firmware.lock (build/firmware/amdgpu)
is used.

  --no-build            Package an existing signed Release app
  --identity IDENTITY   Signing identity for the app and DMG (must match profiles)
  --output PATH         DMG path (default: build/installer/MacLinuxGPU-VERSION.dmg)
EOF
}

output_path=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build|--skip-build) skip_build=1; shift ;;
    --identity|--output)
      option="$1"
      [[ $# -ge 2 && -n "$2" ]] || { echo "missing value for $option" >&2; exit 2; }
      if [[ "$option" == --identity ]]; then
        signing_identity="$2"
        identity_explicit=1
      else
        output_path="$2"
      fi
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ $skip_build -eq 0 ]]; then
  build_args=(--release --install-hsa --build-only)
  if [[ $identity_explicit -eq 1 ]]; then
    build_args+=(--identity "$signing_identity")
  fi
  "$project_root/scripts/activate.sh" "${build_args[@]}"
fi

plist_value() {
  /usr/libexec/PlistBuddy -c "Print $2" "$1" 2>/dev/null
}

dext_path="$app_path/Contents/Library/SystemExtensions/$dext_id.dext"
[[ -d "$app_path" && -d "$dext_path" ]] || {
  echo "error: Release app or embedded dext is missing; build it first" >&2
  exit 1
}
[[ "$(plist_value "$app_path/Contents/Info.plist" CFBundleIdentifier)" == "$host_id" &&
   "$(plist_value "$dext_path/Info.plist" CFBundleIdentifier)" == "$dext_id" ]] || {
  echo "error: host or dext bundle identifier does not match the approved IDs" >&2
  exit 1
}

for bundle in "$app_path" "$dext_path"; do
  codesign --verify --strict --verbose=2 "$bundle"
  actual_team="$(codesign -dv "$bundle" 2>&1 | sed -n 's/^TeamIdentifier=//p' | head -1)"
  [[ "$actual_team" == "$team_id" ]] || {
    echo "error: $bundle is signed by team $actual_team; expected $team_id" >&2
    exit 1
  }
done
codesign --verify --deep --strict "$app_path"
if [[ $identity_explicit -eq 0 ]]; then
  signing_identity="$(codesign -dv --verbose=4 "$app_path" 2>&1 |
    sed -n 's/^Authority=//p' | head -1)"
  [[ -n "$signing_identity" ]] || {
    echo "error: could not read app signing identity" >&2
    exit 1
  }
fi

host_entitlements="$(codesign -d --entitlements - "$app_path" 2>/dev/null)"
dext_entitlements="$(codesign -d --entitlements - "$dext_path" 2>/dev/null)"
[[ "$host_entitlements" == *"$team_id.$host_id"* &&
   "$host_entitlements" == *"com.apple.developer.system-extension.install"* &&
   "$dext_entitlements" == *"$team_id.$dext_id"* &&
   "$dext_entitlements" == *"com.apple.developer.driverkit"* &&
   "$dext_entitlements" == *"com.apple.developer.driverkit.transport.pci"* ]] || {
  echo "error: signed app is missing required system extension entitlements" >&2
  exit 1
}

version="$(plist_value "$app_path/Contents/Info.plist" CFBundleShortVersionString)"
build="$(plist_value "$app_path/Contents/Info.plist" CFBundleVersion)"
[[ "$version" =~ ^[A-Za-z0-9._-]+$ && "$build" =~ ^[A-Za-z0-9._-]+$ ]] || {
  echo "error: app version is missing or contains unsafe filename characters" >&2
  exit 1
}
if [[ -z "$output_path" ]]; then
  output_path="$output_dir/MacLinuxGPU-$version-$build.dmg"
fi
mkdir -p "$(dirname "$output_path")"
output_path="$(cd "$(dirname "$output_path")" && pwd)/$(basename "$output_path")"

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/maclinuxgpu-dmg.XXXXXX")"
mount_path="$work_dir/mount"
mounted=0
cleanup() {
  if [[ $mounted -eq 1 ]]; then
    hdiutil detach "$mount_path" -quiet || hdiutil detach -force "$mount_path" -quiet || true
  fi
  rm -rf "$work_dir"
}
trap cleanup EXIT

stage_path="$work_dir/stage"
mkdir -p "$stage_path"
ditto "$app_path" "$stage_path/$app_name"
runtime_path="$stage_path/$app_name/Contents/Resources/Runtime"
validate_runtime_payload() {
  local runtime="$1" name entry actual_team
  [[ -d "$runtime" && ! -L "$runtime" ]] || {
    echo "error: app runtime payload missing or linked; build with --install-hsa" >&2
    return 1
  }
  for entry in "$runtime"/* "$runtime"/.[!.]* "$runtime"/..?*; do
    [[ -e "$entry" || -L "$entry" ]] || continue
    case "${entry##*/}" in
      libhsa-runtime64.0.1.0.dylib|libhsa-runtime64.1.dylib|libhsa-runtime64.dylib) ;;
      *) echo "error: unexpected runtime payload entry: $entry" >&2; return 1 ;;
    esac
  done
  for name in libhsa-runtime64.0.1.0.dylib; do
    [[ -f "$runtime/$name" && ! -L "$runtime/$name" ]] || {
      echo "error: required runtime library is missing or linked: $name" >&2
      return 1
    }
    codesign --verify --strict "$runtime/$name"
    actual_team="$(codesign -dv "$runtime/$name" 2>&1 | sed -n 's/^TeamIdentifier=//p' | head -1)"
    [[ "$actual_team" == "$team_id" ]] || {
      echo "error: runtime library $name is signed by team $actual_team" >&2
      return 1
    }
  done
  [[ -L "$runtime/libhsa-runtime64.1.dylib" &&
     "$(readlink "$runtime/libhsa-runtime64.1.dylib")" == libhsa-runtime64.0.1.0.dylib &&
     -L "$runtime/libhsa-runtime64.dylib" &&
     "$(readlink "$runtime/libhsa-runtime64.dylib")" == libhsa-runtime64.1.dylib ]] || {
    echo "error: runtime payload symlink chain is incomplete or malformed" >&2
    return 1
  }
}
validate_runtime_payload "$runtime_path"
validate_firmware_payload() {
  local firmware="$1"
  [[ -d "$firmware" && ! -L "$firmware" ]] || {
    echo "error: app firmware directory missing or linked" >&2
    return 1
  }
  [[ -n "$(find "$firmware" -maxdepth 1 -name 'WHENCE*' -print -quit)" ]] || {
    echo "error: app firmware directory lacks its WHENCE provenance file" >&2
    return 1
  }
  [[ -n "$(find "$firmware" -name '*.bin' -print -quit)" ]] || {
    echo "error: app firmware directory contains no firmware images" >&2
    return 1
  }
}
validate_firmware_payload "$stage_path/$app_name/Contents/Resources/firmware"
cat > "$stage_path/Install MacLinuxGPU.txt" <<'EOF'
Double-click MacLinuxGPUHost.app to install and activate the driver.
Follow the app's instructions for macOS approval.
Keep the app in /Applications after installation so macOS can manage updates.
The app also installs the bundled HSA runtime library and the bundled AMD GPU
firmware (linux-firmware, see its WHENCE file) to
/Library/Application Support/MacLinuxGPU/firmware.
EOF

image_path="$work_dir/MacLinuxGPU.dmg"
read_write_path="$work_dir/MacLinuxGPU.read-write.dmg"
echo "==> creating MacLinuxGPU $version ($build) installer DMG"
# APFS preserves the signed bundle's symlinks and metadata. Populate a blank
# image instead of using -srcfolder, which has altered FinderInfo on signed
# bundles on some DiskImages versions.
# Size the image from the payload: a full amdgpu firmware directory is far
# larger than the app itself.
image_mb=$(( $(du -sm "$stage_path" | cut -f1) * 5 / 4 + 64 ))
hdiutil create -size "${image_mb}m" -fs APFS -volname "MacLinuxGPU Installer" \
  "$read_write_path"
mkdir -p "$mount_path"
hdiutil attach -quiet -nobrowse -mountpoint "$mount_path" "$read_write_path"
mounted=1
ditto "$stage_path" "$mount_path"
codesign --verify --deep --strict "$mount_path/$app_name"
validate_runtime_payload "$mount_path/$app_name/Contents/Resources/Runtime"
hdiutil detach "$mount_path" -quiet || hdiutil detach -force "$mount_path" -quiet
mounted=0
hdiutil convert "$read_write_path" -format UDZO -o "$image_path" -ov
hdiutil verify "$image_path"
codesign --force --sign "$signing_identity" --timestamp "$image_path"
codesign --verify --strict --verbose=2 "$image_path"

hdiutil attach -quiet -readonly -nobrowse -mountpoint "$mount_path" "$image_path"
mounted=1
codesign --verify --deep --strict "$mount_path/$app_name"
[[ "$(plist_value "$mount_path/$app_name/Contents/Info.plist" CFBundleIdentifier)" == "$host_id" ]]
validate_runtime_payload "$mount_path/$app_name/Contents/Resources/Runtime"
hdiutil detach "$mount_path" -quiet || hdiutil detach -force "$mount_path" -quiet
mounted=0

mv -f "$image_path" "$output_path"
echo "==> verified signed installer DMG: $output_path"
