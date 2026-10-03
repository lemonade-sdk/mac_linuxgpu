"""Paths of the upstream Linux sources the build uses.

The build compiles an explicit list of files from the pinned submodule
(mk/upstream_sources.mk). The stub tools need the same view: the upstream .c
files that are built, and the driver-local headers they may declare symbols
in (the amd/ tree, TTM, the scheduler and the DRM core headers).
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINUX = os.path.join(ROOT, "third_party", "linux")
LINUX_INCLUDE = os.path.join(LINUX, "include")
DRM = os.path.join(LINUX, "drivers", "gpu", "drm")
SOURCES_MK = os.path.join(ROOT, "mk", "upstream_sources.mk")

# Driver-local header roots, in the order of the include search path.
_AMD_DIRS = ("acp", "amdgpu", "amdkfd", "amdxcp", "display", "include", "pm", "ras")


def _sources_text():
    with open(SOURCES_MK) as f:
        return f.read().replace("\\\n", " ")


def built_driver_sources():
    """Absolute paths of the upstream amdgpu/KFD/TTM/scheduler/DRM sources."""
    text = _sources_text()
    m = re.search(r"^UPSTREAM_DRIVER_SRCS\s*:=(.*)$", text, re.M)
    if not m:
        raise SystemExit("UPSTREAM_DRIVER_SRCS not found in mk/upstream_sources.mk")
    return [os.path.join(LINUX, p[len("$(LINUX)/"):]) for p in m.group(1).split()]


def helper_sources():
    """Absolute paths of the upstream library helpers built into linuxu."""
    text = _sources_text()
    m = re.search(r"^UPSTREAM_HELPERS\s*:=(.*)$", text, re.M)
    if not m:
        raise SystemExit("UPSTREAM_HELPERS not found in mk/upstream_sources.mk")
    return [os.path.join(LINUX, item.split(":", 1)[1]) for item in m.group(1).split()]


def driver_header_files(skip=()):
    """Driver-local headers: the amd/ tree, ttm/, scheduler/, DRM core."""
    out = []
    roots = [os.path.join(DRM, "amd", d) for d in _AMD_DIRS]
    roots += [os.path.join(DRM, "ttm"), os.path.join(DRM, "scheduler")]
    for base in roots:
        for dirpath, dirs, files in os.walk(base):
            dirs.sort()
            if any(k in dirpath + "/" for k in skip):
                continue
            out += [os.path.join(dirpath, f) for f in sorted(files) if f.endswith(".h")]
    out += [os.path.join(DRM, f) for f in sorted(os.listdir(DRM))
            if f.endswith(".h") and os.path.isfile(os.path.join(DRM, f))]
    return out
