# mk/dext.mk
#
# DriverKit (dext) build settings. The dext is one monolithically-linked
# signed image: no dlopen, everything static.
#
# SDK detection: the DriverKit SDK ships with Xcode; if it is not present
# `make dext` degrades to a helpful message + exit 0 (the desktop build
# targets — spike/all/test — do not need it).

SDK := $(shell xcrun --show-sdk-path --sdk driverkit 2>/dev/null)

# The real DriverKit framework headers live under $(SDK)/System/DriverKit/
# System/Library/Frameworks/{DriverKit,PCIDriverKit}.framework (framework-style
# imports: #import <DriverKit/...>, #import <PCIDriverKit/IOPCIDevice.h>).
# DriverKit25.5.sdk is a symlink to DriverKit.sdk; its top-level Headers/ is
# empty — use -F (framework search) to the frameworks dir, NOT -isystem.
#
# The mach-type conflict: the DriverKit SDK's IORPC.h defines
# mach_msg_type_name_t but not mach_msg_type_number_t, while the MacOSX SDK's
# mach_voucher_types.h expects the latter. Including <mach/mach.h> FIRST (via
# -include) resolves it — the real mach headers define both, and the DriverKit
# IORPC.h typedefs then don't collide. -isysroot points system headers at the
# MacOSX SDK (the DriverKit SDK has no full usr/include).
DK_FW := $(SDK)/System/DriverKit/System/Library/Frameworks
MSDK := $(shell xcrun --show-sdk-path --sdk macosx 2>/dev/null)

# The DriverKit frameworks are built for the 'driverKit' platform, so the
# dext objects + binary must target arm64-apple-driverkit. The DriverKit
# SDK has no full usr/include (no C headers), so the C headers come from the
# MacOSX SDK via -isystem $MSDK/usr/include. -F $(DK_FW) finds the framework
# headers + .tbd link stubs. -include mach/mach.h fixes the mach-type
# conflict (the DriverKit IORPC.h defines mach_msg_type_name_t but not
# mach_msg_type_number_t; including the real mach headers first resolves it).
# -fobjc-arc is NOT used (the driverKit target makes clang reject it; the
# sources use manual retain/release like the mac_amdgpu reference).
DEXT_SDKHDRS := -target arm64-apple-driverkit -isystem $(MSDK)/usr/include \
                -F $(DK_FW) -include mach/mach.h \
                -include linux/autoconf.h
DEXT_COMPILE_TARGET :=
# (DEXT_LDFLAGS is set inside the else block below, where $(SDK) is known.)

# C++ compiler for the dext shell. Use Apple clang++ explicitly (the GNU-make
# default CXX=c++ does not carry the DriverKit framework search paths, and the
# DriverKit headers are C++ so the shell must be ObjC++).
CXX := clang++

# The full KMD + shim object set, prebuilt into a static lib by `make lib`
# (463 driver .o + 59 linuxu .o). The dext links this in — one monolithically
# linked signed image.
# (lazy expansion — BUILD is defined later in the Makefile, after this
# include; `=` resolves $(BUILD) at use-time, not at include-time).
KMD_STATIC = $(BUILD)/libmacamgdu.a

# Code-signing identity for the dext (empty = build unsigned; the dext target
# degrades to "built, unsigned"; the Xcode project and scripts/activate.sh do
# the real signing with a team identity and provisioning profile).
DIDENTITY ?=
DEXT_CODE_SIGN_FLAGS := -o library,runtime

DEXT_SDKFLAGS :=
ifeq ($(SDK),)
DEXTSRC :=
DEXTOBJ :=
DEXTBUILD :=
DEXTPKG :=
else
# The make build compiles the make-build dext sources (the hand-rolled
# MacLinuxGPU.mm + the C API seam).  MacLinuxGPUXcode.mm is Xcode-only (the
# OSMetaClass registration + the UserClient ExternalMethod — it #includes the
# iig-codegen'd headers MacLinuxGPU.h / MacLinuxGPUUserClient.h that only
# exist in the Xcode build), so it is EXCLUDED from the make build.  The
# Xcode build (mac_linuxgpu.xcodeproj) compiles it explicitly.
DEXTSRC := $(filter-out dext/sources/MacLinuxGPUXcode.mm, \
        $(wildcard dext/sources/*.m dext/sources/*.mm dext/sources/*.c))
DEXTOBJ := $(DEXTSRC:dext/sources/%.m=build/dext-obj/%.o)
DEXTOBJ := $(DEXTOBJ:dext/sources/%.mm=build/dext-obj/%.o)
DEXTOBJ := $(DEXTOBJ:dext/sources/%.c=build/dext-obj/%.o)
DEXTBUILD := build/mac_linuxgpu.dext
DEXTPKG  := build/mac_linuxgpu.dext.pkg
# DriverKit does not provide the C++ exception unwinder. RAII cleanup still
# runs on ordinary returns; do not emit exception-runtime references.
DEXTSRCFLAGS := $(DEXT_COMPILE_TARGET) $(DEXT_SDKHDRS) -fno-exceptions -fno-objc-exceptions -Ilinuxu/headers -Ilinuxu/src -Idext/sources
# Link phase: the dext binary must target the driverKit platform (the objects
# are driverKit-platform, the DriverKit/PCIDriverKit .tbds are driverKit-
# platform). -isysroot $(SDK) provides the .tbd link stubs. Only DriverKit +
# PCIDriverKit. Do NOT pass -Wl,-dead_code_stripping,
# no — this ld rejects it.
DEXT_LDFLAGS := -target arm64-apple-driverkit -isysroot $(SDK) -F $(DK_FW) \
                -framework DriverKit -framework PCIDriverKit
endif

# The `dext` packaging tool (xcrun dext / xcodebuild) — guarded at runtime.
DEXTTOOL := $(shell command -v dext 2>/dev/null)
