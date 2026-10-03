#!/bin/bash
#
# "Install MacLinuxGPU.command" — double-click to install the MacLinuxGPU driver.
#
# This is a convenience wrapper around scripts/activate.sh. It:
#   1. Builds the driver (dext + host app + HSA runtime) if not already built.
#   2. Uses Xcode automatic signing, with profile-matched local signing as fallback.
#   3. Replaces the old host app after validating the new package.
#   4. Installs the HSA runtime to /usr/local/.
#   5. Stages the app in /Applications/, launches it, and checks attachment.
#
# Prerequisites:
#   - Xcode + DriverKit SDK installed
#   - A signing certificate authorized by your provisioning profiles
#     (XCODE_TEAM_ID selects the team; see scripts/activate.sh)
#   - An AMD GPU attached over Thunderbolt (for the dext to bind)
#   - Network access on the first build: the pinned Linux sources and the
#     locked linux-firmware files are fetched automatically
#   - Optional: LINUX_FIRMWARE_DIR=<linux-firmware checkout> to bundle the
#     complete upstream amdgpu firmware directory
#
# Double-click this file in Finder, or run:  bash "Install MacLinuxGPU.command"

set -euo pipefail

# Resolve the repo root (this script lives in scripts/)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "========================================================"
echo "  MacLinuxGPU Installer"
echo "========================================================"
echo
echo "  Repo:        $PROJECT_ROOT"
echo "  Team ID:     ${XCODE_TEAM_ID:-YBQ9BU6Q6F}"
echo "  Identity:    certificate authorized by the approved profiles"
echo
echo "  This will:"
echo "    1. Build the driver (dext + host app + HSA runtime)"
echo "    2. Sign with the certificate authorized by your profiles"
echo "    3. Replace the previous host app (if present)"
echo "    4. Install the HSA runtime to /usr/local/"
echo "    5. Copy the app to /Applications/, install the amdgpu firmware"
echo "       directory, and activate the dext"
echo
echo "  Make sure the AMD GPU is attached over Thunderbolt."
echo
read -rp "  Press Enter to continue, or Ctrl+C to cancel..."

# Run the full installer. A successful exit means the exact bundled extension
# is enabled and its attached UserClient answered two pings.
cd "$PROJECT_ROOT"
if ! scripts/activate.sh --release --install-hsa --replace; then
  echo
  echo "========================================================"
  echo "  Installation or driver verification did not complete."
  echo "  Review the error above before treating the driver as active."
  echo "========================================================"
  echo
  read -rp "  Press Enter to close this window..."
  exit 1
fi

echo
echo "========================================================"
echo "  Done. Driver enabled and UserClient ping verified."
echo
echo "  Verify:  systemextensionsctl list"
echo "========================================================"
echo
read -rp "  Press Enter to close this window..."
