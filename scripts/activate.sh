#!/usr/bin/env bash
#
# scripts/activate.sh — install + activate MacLinuxGPU non-interactively.
#
# What it does:
#   0. Runs scripts/bootstrap.sh when setup is missing or stale (via make):
#      fetches the pinned Linux submodule, applies patches/linux/, and
#      fetches the locked linux-firmware files into build/firmware.
#   1. Rebuilds the KMD static lib (make lib-dext).
#   2. Builds the Xcode project (the iig codegen + the host app + the dext).
#   3. [If --replace] Removes the old host app after validating the new package.
#   4. Stages the signed app and preserves the previous copy during replacement.
#   5. Moves the freshly-built app to /Applications/.
#   6. [If --install-hsa] Installs HSA headers to /usr/local/ and the
#      HSA runtime to /Library/MacAMDGPU/runtime and /usr/local/lib.
#   6b. Installs the bundled amdgpu firmware directory (a whole
#      linux-firmware amdgpu/ tree with WHENCE and license files) to
#      /Library/Application Support/MacLinuxGPU/firmware/amdgpu, from where
#      the GPU-initializing process serves the driver's firmware requests.
#   7. Enables developer-mode dext staging (sudo).
#   8. Launches the host app, waits for approval/registration, and checks
#      that its UserClient responds to ping.
#
# Usage:
#   scripts/activate.sh                          # Debug build (default)
#   scripts/activate.sh --release                # Release build
#   scripts/activate.sh --identity "..."         # Override the signing identity
#   scripts/activate.sh --profile <name>         # Override the provisioning profile
#   scripts/activate.sh --install-hsa            # Install the HSA runtime library
#   scripts/activate.sh --replace                # Remove the old host app after staging the new one
#   scripts/activate.sh --build-only             # Build, sign, verify; do not install
#   scripts/activate.sh --display                # Enable Display Core in the dext personality
#   scripts/activate.sh --uninstall              # Uninstall the NEW mac_linuxgpu driver
#   scripts/activate.sh --help                   # Show this help
#
# Display: by default the dext probes compute-only (amdgpu.dc=0). --display
# sets the optional personality key MacLinuxGPUDisplay=true in the built
# dext's Info.plist before it is signed, so the driver brings up Display
# Core on the GPU's own outputs (amdgpu.dc=-1) and the display test
# (scripts/display-test.py, MacLinuxGPUHost display-*) can run. The source
# Info.plist is not changed; installing again without --display turns
# display off. macOS replaces an installed extension only for a new
# CFBundleVersion, so switching between the two needs a version bump.
#
# Firmware: by default the locked set in firmware/firmware.lock is bundled
# (fetched into build/firmware/amdgpu). Set LINUX_FIRMWARE_DIR to a
# linux-firmware checkout (or its amdgpu/ directory) to bundle the complete
# upstream amdgpu firmware set instead; `scripts/fetch-firmware.sh --all`
# fetches one at the locked commit into build/firmware/full.
#
# Examples:
#   # Full install with signing + HSA runtime + replace the old driver:
#   scripts/activate.sh --release \
#     --identity "Developer ID Application: Your Name (TEAMID)" \
#     --profile "MacLinuxGPU Profile" \
#     --install-hsa \
#     --replace
#
#   # Build and verify the signed package without installing it:
#   scripts/activate.sh --release --build-only
#
# Notes:
#   - Step 7 needs sudo. We'll prompt; if you'd rather it not need sudo,
#     run `sudo systemextensionsctl developer on` once by hand.
#   - The first time this runs Apple's security UI will pop. After you click
#     Allow the dext is permanent until you uninstall.
#   - Device attachment requires an AMD GPU attached over Thunderbolt. The
#     installer checks attachment by opening its UserClient and calling Ping.
#   - The old and new dexts share the same approved bundle identifier. sysextd
#     replaces the old registration after the new package has been staged.
#   - The script preserves the existing installed app until the new package
#     passes build and signature checks.

set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT_ROOT="$(pwd)"
APP_NAME="MacLinuxGPUHost"
DEXT_BUNDLE_ID="com.geramyloveless.MacAMDGPUHost.MacAMDGPU"
HOST_BUNDLE_ID="com.geramyloveless.MacAMDGPUHost"
# The previous-generation host app with the same bundle family (for --replace):
OLD_APP_NAME="MacAMDGPUHost"

config=Debug
uninstall=0
install_hsa=0
replace=0
build_only=0
display=0
signing_identity=""
provisioning_profile=""

usage() {
  sed -n '1,/^$/p' "$0"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --release) config=Release; shift ;;
    --uninstall) uninstall=1; shift ;;
    --install-hsa) install_hsa=1; shift ;;
    --replace) replace=1; shift ;;
    --build-only) build_only=1; shift ;;
    --display) display=1; shift ;;
    --identity|--profile)
      option="$1"
      [[ $# -ge 2 && -n "$2" ]] || { echo "missing value for $option" >&2; exit 2; }
      if [[ "$option" == --identity ]]; then signing_identity="$2"; else provisioning_profile="$2"; fi
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------------------
# --uninstall: remove the NEW mac_linuxgpu driver (not the old mac_amdgpu).
# ---------------------------------------------------------------------------
if [[ $uninstall -eq 1 ]]; then
  echo "==> uninstalling ${DEXT_BUNDLE_ID}"
  team_id="$(codesign -dv "/Applications/${APP_NAME}.app" 2>&1 \
    | sed -n 's/^TeamIdentifier=//p' | head -1 || true)"
  team_id="${team_id:-${XCODE_TEAM_ID:-YBQ9BU6Q6F}}"
  if [[ -n "${team_id:-}" ]]; then
    sudo systemextensionsctl uninstall "$team_id" "$DEXT_BUNDLE_ID" || true
  else
    echo "warning: couldn't find team id; run \`systemextensionsctl uninstall <TEAM> $DEXT_BUNDLE_ID\` manually" >&2
  fi
  echo "==> removing /Applications/${APP_NAME}.app"
  sudo rm -rf "/Applications/${APP_NAME}.app"
  echo "==> removing installed firmware"
  sudo rm -rf "/Library/Application Support/MacLinuxGPU/firmware"
  echo "done."
  exit 0
fi

# The old and new drivers use the same Apple-approved bundle identifiers.
# Leave the registered extension in place so sysextd can replace it after the
# new app has passed build and signature checks.

# ---------------------------------------------------------------------------
# 1. Rebuild the KMD archive before packaging the dext. Release rebuilds all
#    DriverKit objects because make does not track changes to compile flags.
#    make runs scripts/bootstrap.sh first when setup is missing or stale.
# ---------------------------------------------------------------------------
if [[ "$config" == Release ]]; then
  echo "==> rebuilding KMD static lib for Release (make lib-dext)"
  rm -rf "$PROJECT_ROOT/build-dk"
  make -C "$PROJECT_ROOT" lib-dext
else
  echo "==> building KMD static lib (make lib-dext)"
  make -C "$PROJECT_ROOT" lib-dext
fi

# ---------------------------------------------------------------------------
# 2. Build the HSA runtime dylib (if --install-hsa).
# ---------------------------------------------------------------------------
if [[ $install_hsa -eq 1 ]]; then
  echo "==> building HSA runtime dylib (make hsa)"
  make -C "$PROJECT_ROOT" hsa
  for source in "$PROJECT_ROOT/build/hsa/libhsa-runtime64.0.1.0.dylib"; do
    [[ -f "$source" ]] || {
      echo "error: missing HSA runtime: $source" >&2
      exit 1
    }
    codesign --verify --strict "$source"
  done
fi

# ---------------------------------------------------------------------------
# 3. Build the Xcode project (the iig codegen + the host app + the dext).
#    Automatic signing is primary. If no Xcode account is configured, build
#    unsigned and sign using approved local profiles and Developer ID.
# ---------------------------------------------------------------------------
: "${XCODE_TEAM_ID:=YBQ9BU6Q6F}"
echo "==> building ${APP_NAME} ($config) — team ${XCODE_TEAM_ID}"

# Build the xcodebuild args.
xcodebuild_args=(
  -project "$PROJECT_ROOT/mac_linuxgpu.xcodeproj"
  -scheme "$APP_NAME"
  -configuration "$config"
  -derivedDataPath "$PROJECT_ROOT/build/xcode"
  -destination 'platform=macOS'
  DEVELOPMENT_TEAM="$XCODE_TEAM_ID"
  -allowProvisioningUpdates
)

# Use Xcode automatic signing first. If it fails, use the local approved
# profiles and Developer ID identity to sign the unsigned build.
if [[ -n "$provisioning_profile" ]]; then
  xcodebuild_args+=(PROVISIONING_PROFILE_SPECIFIER="$provisioning_profile")
fi

find_profile() {
  python3 - "$1" <<'PY'
import datetime
import glob
import os
import plistlib
import subprocess
import sys

identifier = sys.argv[1]
directory = os.path.expanduser("~/Library/Developer/Xcode/UserData/Provisioning Profiles")
matches = []
for path in glob.glob(directory + "/*"):
    result = subprocess.run(["openssl", "cms", "-verify", "-inform", "DER",
                             "-in", path, "-noverify"], capture_output=True)
    if result.returncode:
        continue
    profile = plistlib.loads(result.stdout)
    app_id = profile.get("Entitlements", {}).get("com.apple.application-identifier")
    expiration = profile.get("ExpirationDate")
    if app_id == identifier and expiration and expiration > datetime.datetime.utcnow():
        matches.append((expiration, path))
if matches:
    print(max(matches)[1])
PY
}

build_log=$(mktemp)
unsigned_fallback=0
if [[ -n "$signing_identity" ]]; then
  echo "==> building unsigned for explicit signing identity $signing_identity"
  unsigned_fallback=1
  if ! xcodebuild "${xcodebuild_args[@]}" CODE_SIGNING_ALLOWED=NO build > "$build_log" 2>&1; then
    echo "build failed — last 30 lines of build log:" >&2
    tail -30 "$build_log" >&2
    rm -f "$build_log"
    exit 1
  fi
elif ! xcodebuild "${xcodebuild_args[@]}" build > "$build_log" 2>&1; then
  echo "==> automatic signing failed; trying the local Developer ID and approved profiles"
  tail -12 "$build_log" >&2
  unsigned_fallback=1
  if ! xcodebuild "${xcodebuild_args[@]}" CODE_SIGNING_ALLOWED=NO build > "$build_log" 2>&1; then
    echo "build failed — last 30 lines of build log:" >&2
    tail -30 "$build_log" >&2
    rm -f "$build_log"
    exit 1
  fi
fi
rm -f "$build_log"

# ---------------------------------------------------------------------------
# 4. Find the built app and check the approved bundle identifiers.
# ---------------------------------------------------------------------------
BUILT_APP="$PROJECT_ROOT/build/xcode/Build/Products/${config}/${APP_NAME}.app"
DEXT_IN_APP="$BUILT_APP/Contents/Library/SystemExtensions/${DEXT_BUNDLE_ID}.dext"
if [[ ! -d "$DEXT_IN_APP" ]]; then
  echo "error: expected dext missing: $DEXT_IN_APP" >&2
  exit 1
fi
if [[ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$BUILT_APP/Contents/Info.plist")" != "$HOST_BUNDLE_ID" ||
      "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$DEXT_IN_APP/Info.plist")" != "$DEXT_BUNDLE_ID" ]]; then
  echo "error: built host or dext bundle identifier does not match the activation request" >&2
  exit 1
fi
# The package is verified after its final Developer ID signature is applied.

# --display: MacLinuxGPUDisplay=true in every MacLinuxGPU personality of the
# built dext, before its final signature. Without --display the built
# Info.plist is checked to carry no such key (display stays off).
set_display_personality() {
  python3 - "$DEXT_IN_APP/Info.plist" "$1" <<'PY'
import plistlib
import sys

path, enable = sys.argv[1], sys.argv[2] == "1"
with open(path, "rb") as handle:
    info = plistlib.load(handle)
personalities = [p for p in info.get("IOKitPersonalities", {}).values()
                 if p.get("IOUserClass") == "MacLinuxGPU"]
if not personalities:
    sys.exit("error: the dext has no MacLinuxGPU personality")
for personality in personalities:
    if enable:
        personality["MacLinuxGPUDisplay"] = True
    elif personality.get("MacLinuxGPUDisplay") is True:
        sys.exit("error: the built dext enables display, but --display was not given")
if enable:
    with open(path, "wb") as handle:
        plistlib.dump(info, handle)
print(f"{len(personalities)} personalit{'y' if len(personalities) == 1 else 'ies'}: "
      f"display {'on (MacLinuxGPUDisplay=true)' if enable else 'off'}")
PY
}
echo "==> dext display: $(set_display_personality "$display")"

# Replace the bundled firmware with a whole amdgpu firmware directory when
# LINUX_FIRMWARE_DIR is set. Accepts a linux-firmware checkout (containing
# amdgpu/) or an amdgpu directory itself. Licensing files travel with it.
firmware_staged=0
stage_firmware_payload() {
  local source="${LINUX_FIRMWARE_DIR:-}"
  [[ -n "$source" ]] || return 0
  local amdgpu="$source"
  [[ -d "$source/amdgpu" ]] && amdgpu="$source/amdgpu"
  [[ -d "$amdgpu" ]] || {
    echo "error: LINUX_FIRMWARE_DIR has no amdgpu firmware directory: $source" >&2
    return 1
  }
  local payload="$BUILT_APP/Contents/Resources/firmware"
  [[ ! -L "$payload" ]] || { echo "error: firmware payload is a symbolic link" >&2; return 1; }
  rm -rf "$payload"
  ditto "$amdgpu" "$payload"
  local name
  for name in WHENCE WHENCE.amdgpu LICENSE.amdgpu LICENSE; do
    if [[ -f "$source/$name" && ! -e "$payload/$name" ]]; then
      cp "$source/$name" "$payload/$name"
    fi
  done
  [[ -n "$(find "$payload" -maxdepth 1 -name 'WHENCE*' -print -quit)" ]] || {
    echo "error: firmware payload lacks a WHENCE file (keep linux-firmware provenance)" >&2
    return 1
  }
  echo "==> bundled amdgpu firmware from $amdgpu ($(find "$payload" -type f | wc -l | tr -d ' ') files)"
  firmware_staged=1
}

stage_runtime_payload() {
  local payload_identity="$1"
  local payload="$BUILT_APP/Contents/Resources/Runtime"
  [[ ! -L "$payload" ]] || {
    echo "error: runtime payload directory is a symbolic link" >&2
    return 1
  }
  rm -rf "$payload"
  mkdir -p "$payload"
  for source in "$PROJECT_ROOT/build/hsa/libhsa-runtime64.0.1.0.dylib"; do
    [[ -f "$source" && ! -L "$source" ]] || {
      echo "error: expected a regular runtime library: $source" >&2
      return 1
    }
    ditto "$source" "$payload/$(basename "$source")"
    codesign --force --sign "$payload_identity" "$payload/$(basename "$source")"
    codesign --verify --strict "$payload/$(basename "$source")"
    local signed_team
    signed_team="$(codesign -dv "$payload/$(basename "$source")" 2>&1 | sed -n 's/^TeamIdentifier=//p' | head -1)"
    [[ "$signed_team" == "$XCODE_TEAM_ID" ]] || {
      echo "error: runtime library has team $signed_team, expected $XCODE_TEAM_ID" >&2
      return 1
    }
  done
  ln -sfn libhsa-runtime64.0.1.0.dylib "$payload/libhsa-runtime64.1.dylib"
  ln -sfn libhsa-runtime64.1.dylib "$payload/libhsa-runtime64.dylib"
}

# ---------------------------------------------------------------------------
# 4b. Automatic signing is the primary path. The local-profile fallback
#     signs with the full entitlement payload approved in each profile.
# ---------------------------------------------------------------------------
if [[ $unsigned_fallback -eq 1 ]]; then
  host_profile="${HOST_PROFILE:-$(find_profile "$XCODE_TEAM_ID.$HOST_BUNDLE_ID")}"
  dext_profile="${DEXT_PROFILE:-$(find_profile "$XCODE_TEAM_ID.$DEXT_BUNDLE_ID")}"
  [[ -f "$host_profile" && -f "$dext_profile" ]] || {
    echo "error: approved host and dext provisioning profiles are required for manual signing" >&2
    exit 1
  }
  # A valid certificate alone is insufficient. Taskgated rejects a binary
  # when its embedded provisioning profile authorizes a different certificate.
  identity_selection="$(python3 - "$host_profile" "$dext_profile" "$signing_identity" <<'PY'
import hashlib
import plistlib
import re
import subprocess
import sys

def authorized(path):
    result = subprocess.run(["openssl", "cms", "-verify", "-inform", "DER",
                             "-in", path, "-noverify"], capture_output=True,
                            check=True)
    profile = plistlib.loads(result.stdout)
    return {hashlib.sha1(cert).hexdigest().upper()
            for cert in profile.get("DeveloperCertificates", [])}

common = authorized(sys.argv[1]) & authorized(sys.argv[2])
identities = subprocess.run(["security", "find-identity", "-v", "-p",
                             "codesigning"], capture_output=True, text=True,
                            check=True).stdout
available = [(digest.upper(), name) for digest, name in
             re.findall(r'\b([0-9A-Fa-f]{40})\s+"([^"]+)"', identities)]
requested = sys.argv[3]
matches = [(digest, name) for digest, name in available
           if digest in common and (not requested or
              requested in (digest, name))]
if not matches:
    print("error: no valid signing identity is authorized by both embedded "
          "provisioning profiles", file=sys.stderr)
    sys.exit(1)
matches.sort(key=lambda item: (not item[1].startswith("Apple Development:"),
                               item[1]))
print(matches[0][0] + "|" + matches[0][1])
PY
)" || exit 1
  signing_identity="${identity_selection%%|*}"
  signing_name="${identity_selection#*|}"
  cp "$host_profile" "$BUILT_APP/Contents/embedded.provisionprofile"
  cp "$dext_profile" "$DEXT_IN_APP/embedded.provisionprofile"
  ent_dir="$(mktemp -d)"
  trap 'rm -rf "$ent_dir"' EXIT
  python3 - "$host_profile" "$ent_dir/host.plist" "$dext_profile" "$ent_dir/dext.plist" "$signing_name" <<'PY'
import plistlib
import subprocess
import sys

for kind, source, destination in (("host", sys.argv[1], sys.argv[2]),
                                  ("dext", sys.argv[3], sys.argv[4])):
    result = subprocess.run(["openssl", "cms", "-verify", "-inform", "DER",
                             "-in", source, "-noverify"], capture_output=True, check=True)
    entitlements = plistlib.loads(result.stdout)["Entitlements"]
    # Match Xcode's working development signature: the profile's optional
    # keychain wildcard is not needed by either bundle.
    entitlements.pop("keychain-access-groups", None)
    if kind == "host" and sys.argv[5].startswith("Apple Development:"):
        entitlements["com.apple.security.get-task-allow"] = True
    with open(destination, "wb") as output:
        plistlib.dump(entitlements, output)
PY
  echo "==> signing with local approved profiles and $signing_name"
  stage_firmware_payload
  if [[ $install_hsa -eq 1 ]]; then stage_runtime_payload "$signing_identity"; fi
  codesign --force --sign "$signing_identity" -o library,runtime \
    --entitlements "$ent_dir/dext.plist" "$DEXT_IN_APP"
  codesign --force --sign "$signing_identity" --options 0 \
    --entitlements "$ent_dir/host.plist" "$BUILT_APP"
else
  stage_firmware_payload
  if [[ $display -eq 1 ]]; then
    # The personality changed after Xcode signed the dext: sign it again
    # with the same identity, entitlements and flags, then the app.
    dext_identity="$(codesign -dv --verbose=4 "$DEXT_IN_APP" 2>&1 |
      sed -n 's/^Authority=//p' | head -1)"
    [[ -n "$dext_identity" ]] || { echo "error: missing dext signing identity" >&2; exit 1; }
    codesign --force --sign "$dext_identity" \
      --preserve-metadata=entitlements,requirements,flags,runtime "$DEXT_IN_APP"
  fi
  if [[ $install_hsa -eq 1 || $firmware_staged -eq 1 || $display -eq 1 ]]; then
    payload_identity="$(codesign -dv --verbose=4 "$BUILT_APP" 2>&1 |
      sed -n 's/^Authority=//p' | head -1)"
    [[ -n "$payload_identity" ]] || { echo "error: missing app signing identity" >&2; exit 1; }
    if [[ $install_hsa -eq 1 ]]; then stage_runtime_payload "$payload_identity"; fi
    codesign --force --sign "$payload_identity" \
      --preserve-metadata=entitlements,requirements,flags,runtime "$BUILT_APP"
  fi
fi
codesign --verify --deep --strict "$BUILT_APP" || {
  echo "error: signed app failed strict verification; installed app was not changed" >&2
  exit 1
}
for bundle in "$BUILT_APP" "$DEXT_IN_APP"; do
  signed_team="$(codesign -dv "$bundle" 2>&1 | sed -n 's/^TeamIdentifier=//p' | head -1)"
  [[ "$signed_team" == "$XCODE_TEAM_ID" ]] || {
    echo "error: $bundle was signed by team $signed_team, expected $XCODE_TEAM_ID" >&2
    exit 1
  }
done
host_entitlements="$(codesign -d --entitlements - "$BUILT_APP" 2>/dev/null)"
dext_entitlements="$(codesign -d --entitlements - "$DEXT_IN_APP" 2>/dev/null)"
if [[ "$host_entitlements" != *"$XCODE_TEAM_ID.$HOST_BUNDLE_ID"* ||
      "$host_entitlements" != *"com.apple.developer.system-extension.install"* ||
      "$dext_entitlements" != *"$XCODE_TEAM_ID.$DEXT_BUNDLE_ID"* ||
      "$dext_entitlements" != *"com.apple.developer.driverkit"* ||
      "$dext_entitlements" != *"com.apple.developer.driverkit.transport.pci"* ]]; then
  echo "error: signed host or dext is missing the approved application identifier or DriverKit entitlements" >&2
  exit 1
fi
if [[ $build_only -eq 1 ]]; then
  echo "==> build-only package verified: $BUILT_APP"
  exit 0
fi

# ---------------------------------------------------------------------------
# 4. Replace the installed app only after validating the new build.
# ---------------------------------------------------------------------------
installed_app="/Applications/${APP_NAME}.app"
staged_app="/Applications/.${APP_NAME}.stage.$$"
backup_app="/Applications/.${APP_NAME}.backup.$$"
echo "==> staging signed app at $staged_app"
sudo ditto "$BUILT_APP" "$staged_app"
sudo codesign --verify --deep --strict "$staged_app" || {
  sudo rm -rf "$staged_app"
  echo "error: staged app failed signature verification; installed app was not changed" >&2
  exit 1
}
if [[ -e "$installed_app" ]]; then
  sudo mv "$installed_app" "$backup_app"
fi
if ! sudo mv "$staged_app" "$installed_app"; then
  [[ ! -e "$backup_app" ]] || sudo mv "$backup_app" "$installed_app"
  echo "error: could not install staged app; previous app restored" >&2
  exit 1
fi
echo "==> dext embedded in app: $(du -sh "$DEXT_IN_APP" | cut -f1)"

# ---------------------------------------------------------------------------
# 5b. Install the bundled amdgpu firmware directory where firmware servicers
#     look for it (a /lib/firmware equivalent; names carry "amdgpu/").
# ---------------------------------------------------------------------------
firmware_root="/Library/Application Support/MacLinuxGPU/firmware"
if [[ -d "$installed_app/Contents/Resources/firmware" ]]; then
  echo "==> installing amdgpu firmware to $firmware_root/amdgpu"
  sudo mkdir -p "$firmware_root"
  [[ -d "$firmware_root" && ! -L "$firmware_root" ]] || {
    echo "error: $firmware_root is not a directory" >&2; exit 1; }
  sudo rm -rf "$firmware_root/.amdgpu.stage.$$"
  sudo ditto "$installed_app/Contents/Resources/firmware" "$firmware_root/.amdgpu.stage.$$"
  sudo rm -rf "$firmware_root/amdgpu"
  sudo mv "$firmware_root/.amdgpu.stage.$$" "$firmware_root/amdgpu"
fi

# ---------------------------------------------------------------------------
# 6. [If --install-hsa] Install the HSA runtime dylib + headers to /usr/local/.
# ---------------------------------------------------------------------------
if [[ $install_hsa -eq 1 ]]; then
  echo "==> installing HSA runtime to /usr/local/"

  # Ensure the target directories exist.
  sudo mkdir -p /usr/local/lib /usr/local/include/hsa

  # Copy the dylib and preserve its versioned install name and symlink chain.
  HSA_DYLIB="$PROJECT_ROOT/build/hsa/libhsa-runtime64.dylib"
  if [[ ! -f "$HSA_DYLIB" ]]; then
    echo "error: HSA dylib not found at $HSA_DYLIB (run 'make hsa' first)" >&2
    exit 1
  fi
  echo "==>   installing libhsa-runtime64.dylib to /usr/local/lib/"
  sudo cp -f "$PROJECT_ROOT/build/hsa/libhsa-runtime64.0.1.0.dylib" \
    /usr/local/lib/libhsa-runtime64.0.1.0.dylib
  sudo ln -sfn libhsa-runtime64.0.1.0.dylib /usr/local/lib/libhsa-runtime64.1.dylib
  sudo ln -sfn libhsa-runtime64.1.dylib /usr/local/lib/libhsa-runtime64.dylib

  # Copy the public header.
  echo "==>   copying mac_hsa.h to /usr/local/include/"
  sudo cp -f "$PROJECT_ROOT/hsa/include/mac_hsa.h" /usr/local/include/mac_hsa.h

  # Copy the HSA standard headers (so #include <hsa/hsa.h> works).
  echo "==>   copying HSA standard headers to /usr/local/include/hsa/"
  if [[ -d "$PROJECT_ROOT/hsa/third_party/hsa/include/hsa" ]]; then
    sudo cp -f "$PROJECT_ROOT/hsa/third_party/hsa/include/hsa/"*.h /usr/local/include/hsa/
  fi

  echo "==> HSA runtime installed:"
  echo "    /usr/local/lib/libhsa-runtime64.dylib"
  echo "    /usr/local/include/mac_hsa.h"
  echo "    /usr/local/include/hsa/ (HSA standard headers)"
  echo ""
  echo "    To link against it:"
  echo "      #include <mac_hsa.h>"
  echo "      #include <hsa/hsa.h>"
  echo "      gcc ... -L/usr/local/lib -lhsa-runtime64 -Wl,-rpath,/usr/local/lib"

  # Applications open the HSA runtime by basename (found in /usr/local/lib)
  # or by this absolute path. HRX and Loom belong to those applications.
  runtime_dir=/Library/MacAMDGPU/runtime
  echo "==> installing the HSA runtime to $runtime_dir"
  sudo mkdir -p "$runtime_dir"
  sudo ditto "$PROJECT_ROOT/build/hsa/libhsa-runtime64.0.1.0.dylib" "$runtime_dir/libhsa-runtime64.0.1.0.dylib"
  sudo ln -sfn libhsa-runtime64.0.1.0.dylib "$runtime_dir/libhsa-runtime64.1.dylib"
  sudo ln -sfn libhsa-runtime64.1.dylib "$runtime_dir/libhsa-runtime64.dylib"
  # Earlier builds also installed HRX/Loom here, which shadowed the copies
  # applications bundle.
  sudo rm -f "$runtime_dir"/libhrx*.dylib "$runtime_dir"/libloomc*.dylib
fi

# ---------------------------------------------------------------------------
# 7. Enable developer-mode dext staging (needs sudo).
# ---------------------------------------------------------------------------
dev_state="$(systemextensionsctl developer 2>&1 || true)"
if [[ "$dev_state" == *"Developer mode is enabled"* ]]; then
  echo "==> systemextensionsctl developer mode already on"
else
  echo "==> enabling developer-mode dext staging (sudo)"
  sudo systemextensionsctl developer on || true
fi

# ---------------------------------------------------------------------------
# 8. Run the host app's activation command and wait for its result.
# ---------------------------------------------------------------------------
echo "==> launching /Applications/${APP_NAME}.app for activation"
open -n "$installed_app"

expected_short="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$DEXT_IN_APP/Info.plist")"
expected_build="$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' "$DEXT_IN_APP/Info.plist")"
expected_version="(${expected_short}/${expected_build})"
host_exe="$installed_app/Contents/MacOS/$APP_NAME"
echo "==> waiting for ${DEXT_BUNDLE_ID} ${expected_version} to become enabled"
enabled=0
for attempt in {1..45}; do
  extension_state="$(systemextensionsctl list 2>/dev/null | grep -F "$DEXT_BUNDLE_ID" || true)"
  if printf '%s\n' "$extension_state" \
      | grep -F "$expected_version" | grep -Fq '[activated enabled]'; then
    enabled=1
    break
  fi
  if printf '%s\n' "$extension_state" \
      | grep -F "$expected_version" | grep -Fq 'waiting to upgrade on reboot'; then
    echo "macOS accepted ${DEXT_BUNDLE_ID} ${expected_version}, but replacement is pending; attachment has not been verified." >&2
    echo "The previous app is backed up at $backup_app. Reboot, then rerun this installer to verify attachment." >&2
    exit 2
  fi
  sleep 2
done
if [[ $enabled -ne 1 ]]; then
  echo "Activation is still pending or failed. Check System Settings → General → Login Items & Extensions → Driver Extensions." >&2
  echo "The installed package is signed, but the driver is not yet enabled." >&2
  echo "The previous app is backed up at $backup_app." >&2
  exit 2
fi

echo "==> extension enabled; checking actual UserClient attachment and ping"
attached=0
for attempt in {1..15}; do
  if "$host_exe" ping >/dev/null 2>&1; then
    attached=1
    break
  fi
  sleep 2
done
if [[ $attached -ne 1 ]]; then
  echo "Extension is enabled, but no responding MacLinuxGPU UserClient is attached." >&2
  echo "Registration alone does not confirm that the driver is attached to a GPU." >&2
  echo "The previous app is backed up at $backup_app." >&2
  exit 3
fi
sleep 5
if ! "$host_exe" ping; then
  echo "UserClient stopped responding after its initial ping." >&2
  echo "The previous app is backed up at $backup_app." >&2
  exit 3
fi
[[ ! -e "$backup_app" ]] || sudo rm -rf "$backup_app"
if [[ $replace -eq 1 && -e "/Applications/${OLD_APP_NAME}.app" ]]; then
  echo "==> removing old host app /Applications/${OLD_APP_NAME}.app"
  sudo rm -rf "/Applications/${OLD_APP_NAME}.app"
fi

echo ""
echo "Driver enabled and UserClient ping passed twice. To inspect registration:"
echo "    systemextensionsctl list"
echo ""
echo "If you used --install-hsa, the HSA runtime is now at:"
echo "    /usr/local/lib/libhsa-runtime64.dylib"
echo "    /usr/local/include/mac_hsa.h"
echo "    /Library/MacAMDGPU/runtime/"
echo ""
echo "If you used --replace, the old MacAMDGPUHost.app has been removed."
