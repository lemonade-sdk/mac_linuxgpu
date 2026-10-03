# mac_linuxgpu — top-level Makefile
#
# Builds the unmodified upstream Linux amdgpu/amdkfd sources (pinned git
# submodule third_party/linux) against the linuxu userspace kernel shim.
#
# Targets:
#   make              same as `make lib`
#   make setup        run scripts/bootstrap.sh (submodule, patches, firmware)
#   make all          compile the upstream and linuxu objects -> build/
#   make lib          build/libmacamgdu.a (host platform, used by the tests)
#   make lib-dext     build-dk/libmacamgdu-dk.a (DriverKit platform, linked
#                     into the dext by the Xcode project)
#   make dext         link the dext shell + lib-dext with make (unsigned
#                     unless DIDENTITY is set)
#   make test         build and run the offline test suite
#   make hsa          the userspace HSA runtime (CMake, build/hsa)
#   make hsa-test     the HSA runtime host unit tests
#   make verify-source  check the submodule pin, patches and interventions
#   make config-check report CONFIG table vs autoconf.h mismatches
#   make clean        remove build outputs (keeps build/setup, build/firmware)
#   make distclean    remove build/ and build-dk/ entirely
#
# Setup runs automatically: every target except clean/distclean includes
# build/setup/bootstrap-<pin>.mk, whose rule runs scripts/bootstrap.sh when
# the stamp is missing or older than the patch set, the upstream manifest or
# the firmware lock. The pin is part of the stamp name, so moving the
# submodule commit also re-runs setup.
#
# Source layout:
#   third_party/linux/  pinned upstream Linux (sparse submodule, never edited
#                       except by the declared patches in patches/linux/)
#   linuxu/             the shim: replacement headers, implementation, tests
#   mk/upstream_sources.mk  the explicit list of upstream sources built

.DEFAULT_GOAL := lib

include mk/upstream_sources.mk
include mk/kernel_config.mk
include mk/host_clang.mk
include mk/dext.mk

BUILD := build
DRM := $(LINUX)/drivers/gpu/drm
AMD := $(DRM)/amd

# ---------------------------------------------------------------------------
# Automatic setup (scripts/bootstrap.sh). The stamp is a makefile, so make
# re-reads this file (and the source lists) after setup has run.
# ---------------------------------------------------------------------------
LINUX_PIN := $(shell git ls-files -s -- $(LINUX) 2>/dev/null | awk '{print $$2}')
SETUP_STAMP := $(BUILD)/setup/bootstrap-$(LINUX_PIN).mk
SETUP_INPUTS := patches/manifest.json $(wildcard patches/linux/*.patch) \
	firmware/firmware.lock scripts/bootstrap.sh scripts/fetch-firmware.sh

ifneq ($(filter-out clean distclean setup hsa hsa-test,$(or $(MAKECMDGOALS),lib)),)
-include $(SETUP_STAMP)
endif

$(SETUP_STAMP): $(SETUP_INPUTS)
	@bash scripts/bootstrap.sh --from-make

setup:
	@bash scripts/bootstrap.sh

.PHONY: setup

# ---------------------------------------------------------------------------
# Sources and objects.
# ---------------------------------------------------------------------------
# Upstream objects keep the driver/ layout under build/ (see
# mk/upstream_sources.mk): amd/<dir> -> driver/<dir>, ttm and scheduler by
# name, DRM core files -> driver/drm-core.
upstream_objs = $(addprefix $(1)/,$(patsubst $(DRM)/%.c,driver/drm-core/%.o,\
	$(patsubst $(DRM)/scheduler/%.c,driver/scheduler/%.o,\
	$(patsubst $(DRM)/ttm/%.c,driver/ttm/%.o,\
	$(patsubst $(AMD)/%.c,driver/%.o,$(UPSTREAM_DRIVER_SRCS))))))

DRIVER_SRCS := $(UPSTREAM_DRIVER_SRCS)
DRIVER_OBJS := $(call upstream_objs,$(BUILD))

# linuxu/src/**/*.c, plus the upstream library helpers (UPSTREAM_HELPERS),
# which keep linuxu object names. Objects mirror the source subdirectories.
LINUXU_SRCS := $(shell find linuxu/src -name '*.c' 2>/dev/null | sort)
HELPER_OBJ_NAMES := $(foreach h,$(UPSTREAM_HELPERS),$(word 1,$(subst :, ,$(h))))
LINUXU_OBJS := $(addprefix $(BUILD)/linuxu/,$(LINUXU_SRCS:linuxu/src/%.c=%.o)) \
	$(addprefix $(BUILD)/linuxu/,$(addsuffix .o,$(HELPER_OBJ_NAMES)))

# Optional embedded firmware fallback (scripts/fw2rodata.py).
# request_firmware() asks the host firmware servicer first (it reads the
# installed linux-firmware amdgpu directory); this table is only a fallback
# and is never assumed to be complete. EMBED_FIRMWARE=0 builds an empty
# table. FW_EMBED_DIR defaults to the locked set that scripts/fetch-firmware.sh
# places in build/firmware/amdgpu; it may point at any amdgpu firmware
# directory, e.g. a linux-firmware checkout's amdgpu/ (expect a large dext in
# that case). The generated source lives in the build tree.
EMBED_FIRMWARE ?= 1
FW_EMBED_DIR ?= $(BUILD)/firmware/amdgpu
FW_GEN := $(BUILD)/gen/fw_rodata_generated.c
FW_GEN_CONFIG := $(BUILD)/gen/fw-embed.config
FW_OBJ := $(BUILD)/gen/fw_rodata_generated.o
ifeq ($(EMBED_FIRMWARE),1)
FW_GEN_ARGS := --firmware-dir $(FW_EMBED_DIR)
FW_EMBED_FILES := $(shell find $(FW_EMBED_DIR) -type f 2>/dev/null | sort)
else
FW_GEN_ARGS := --empty
FW_EMBED_FILES :=
endif
# The embedded set is part of the archive like any linuxu object.
LINUXU_OBJS += $(FW_OBJ)

# Rewritten only when the selection changes, so the table regenerates when
# EMBED_FIRMWARE/FW_EMBED_DIR change as well as when files change.
$(FW_GEN_CONFIG): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(FW_GEN_ARGS)' | cmp -s - $@ 2>/dev/null || printf '%s\n' '$(FW_GEN_ARGS)' > $@

$(FW_GEN): $(FW_GEN_CONFIG) $(FW_EMBED_FILES) scripts/fw2rodata.py
	@echo "== fw-table: generating $(FW_GEN) ($(FW_GEN_ARGS)) =="
	python3 scripts/fw2rodata.py --output $@ $(FW_GEN_ARGS)

$(FW_OBJ): $(FW_GEN) linuxu/src/fw/fw_rodata.h
	$(CC) $(HOSTCFLAGS) $(INCPATHS) -Ilinuxu/src -c $< -o $@

# Standalone target: regenerate the embedded firmware table.
fw-table: $(FW_GEN)
	@echo "fw-table: OK ($(FW_GEN))"

.PHONY: FORCE fw-table
FORCE:

all: $(DRIVER_OBJS) $(LINUXU_OBJS)
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/liblinuxu.a.tmp
	@ar rcs $(BUILD)/liblinuxu.a.tmp $(LINUXU_OBJS)
	@mv $(BUILD)/liblinuxu.a.tmp $(BUILD)/liblinuxu.a
	@echo "== all: $(words $(DRIVER_OBJS)) upstream .o, $(words $(LINUXU_OBJS)) linuxu .o (incl. firmware rodata) =="

# One upstream library helper: $(1) = output root, $(2) = flags variable,
# $(3) = linuxu object name, $(4) = source path inside the submodule.
define helper_rule
$(1)/linuxu/$(3).o: $(LINUX)/$(4)
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(2)) $$(INCPATHS) -c $$< -o $$@
endef

# Compile rules for one output tree: $(1) = output root, $(2) = flags variable.
# Upstream sources map back from the driver/ object layout.
define compile_rules
$(1)/driver/drm-core/%.o: $(DRM)/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(2)) $$(INCPATHS) -c $$< -o $$@

$(1)/driver/ttm/%.o: $(DRM)/ttm/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(2)) $$(INCPATHS) -c $$< -o $$@

$(1)/driver/scheduler/%.o: $(DRM)/scheduler/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(2)) $$(INCPATHS) -c $$< -o $$@

$(1)/driver/%.o: $(AMD)/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(2)) $$(INCPATHS) -c $$< -o $$@

$(1)/linuxu/%.o: linuxu/src/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(2)) $$(INCPATHS) -c $$< -o $$@

endef

$(eval $(call compile_rules,$(BUILD),HOSTCFLAGS))
$(foreach h,$(UPSTREAM_HELPERS),$(eval $(call helper_rule,$(BUILD),HOSTCFLAGS,$(word 1,$(subst :, ,$(h))),$(word 2,$(subst :, ,$(h))))))

# Declared compile interventions (patches/manifest.json, checked by
# scripts/verify-upstream.py). Route only the GFX12 GMC location call through
# the host-window policy; amdgpu_gmc.c retains its original symbol.
$(BUILD)/driver/amdgpu/gmc_v12_0.o: HOSTCFLAGS += -Damdgpu_gmc_gart_location=rt_amdgpu_gmc_gart_location

# These upstream BUILD_BUG_ON checks fold table accesses or sizeof-derived
# local variables. Linux uses optimization; -O0 cannot prove the conditions.
FOLDED_ASSERT_OBJECTS := drm-core/drm_edid.o \
	pm/swsmu/smu11/arcturus_ppt.o pm/swsmu/smu11/navi10_ppt.o \
	pm/swsmu/smu11/sienna_cichlid_ppt.o pm/swsmu/smu13/aldebaran_ppt.o
$(addprefix $(BUILD)/driver/,$(FOLDED_ASSERT_OBJECTS)): HOSTCFLAGS += -O2 -fno-strict-aliasing

# Pinned generic FIFO helper expects the Linux allocator's core macro includes.
$(BUILD)/linuxu/shims/kfifo.o: HOSTCFLAGS += -include linux/kernel.h -include linux/bug.h
$(BUILD)/linuxu/shims/sort.o: HOSTCFLAGS += -include linux/compiler.h -include linux/preempt.h

# ---------------------------------------------------------------------------
# make lib — static archive of the full KMD + shim object set (host platform).
# ---------------------------------------------------------------------------
lib: all
	@echo "== lib: $(BUILD)/libmacamgdu.a ($(words $(DRIVER_OBJS)) upstream + $(words $(LINUXU_OBJS)) linuxu objects) =="
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/libmacamgdu.a.tmp
	@ar rcs $(BUILD)/libmacamgdu.a.tmp $(DRIVER_OBJS) $(LINUXU_OBJS)
	@mv $(BUILD)/libmacamgdu.a.tmp $(BUILD)/libmacamgdu.a
	@echo "lib: OK ($$(du -h $(BUILD)/libmacamgdu.a | cut -f1))"

# ---------------------------------------------------------------------------
# make lib-dext — the same objects rebuilt for the driverKit platform. A
# macOS-built archive cannot be linked into a driverKit-platform dext (ld
# rejects cross-platform objects), so the objects go in a parallel build-dk/
# tree. Flags: -target arm64-apple-driverkit + -isystem $(MSDK)/usr/include
# (C headers from the MacOSX SDK; the DriverKit SDK has no usr/include).
# The upstream sources are unmodified; only the target differs.
# ---------------------------------------------------------------------------
DK_BUILD   := build-dk
DK_DRIVER_OBJS := $(call upstream_objs,$(DK_BUILD))
DK_LINUXU_OBJS := $(patsubst $(BUILD)/%,$(DK_BUILD)/%,$(LINUXU_OBJS))
DK_FW_OBJ  := $(DK_BUILD)/gen/fw_rodata_generated.o
DK_LIB     := $(DK_BUILD)/libmacamgdu-dk.a
# driverKit platform flags for the KMD objects (parallel to HOSTCFLAGS but
# -target driverKit + C headers from the MacOSX SDK).
DK_HOSTCFLAGS := -std=gnu11 -D__KERNEL__ -DCONFIG_DRM_FBDEV_OVERALLOC=0 -DLINUXU_DEXT_DK=1 -include linux/autoconf.h -w -MMD -MP -Wno-incompatible-function-pointer-types
DK_CFLAGS     := -target arm64-apple-driverkit -isystem $(MSDK)/usr/include $(DK_HOSTCFLAGS)
DK_CFLAGS += -include rt/device_string.h -mstrict-align \
	-fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove \
	-fno-builtin-memcmp -fno-builtin-bzero

# Changing the memory backend must rebuild every KMD object, including ones
# whose old depfiles predate the forced header.
$(DK_DRIVER_OBJS) $(DK_LINUXU_OBJS): linuxu/headers/rt/device_string.h

$(eval $(call compile_rules,$(DK_BUILD),DK_CFLAGS))
$(foreach h,$(UPSTREAM_HELPERS),$(eval $(call helper_rule,$(DK_BUILD),DK_CFLAGS,$(word 1,$(subst :, ,$(h))),$(word 2,$(subst :, ,$(h))))))

$(DK_BUILD)/driver/amdgpu/gmc_v12_0.o: DK_CFLAGS += -Damdgpu_gmc_gart_location=rt_amdgpu_gmc_gart_location
$(addprefix $(DK_BUILD)/driver/,$(FOLDED_ASSERT_OBJECTS)): DK_CFLAGS += -O2 -fno-strict-aliasing
$(DK_BUILD)/linuxu/shims/kfifo.o: DK_CFLAGS += -include linux/kernel.h -include linux/bug.h
$(DK_BUILD)/linuxu/shims/sort.o: DK_CFLAGS += -include linux/compiler.h -include linux/preempt.h

$(DK_FW_OBJ): $(FW_GEN) linuxu/src/fw/fw_rodata.h
	@mkdir -p $(dir $@)
	$(CC) $(DK_CFLAGS) $(INCPATHS) -Ilinuxu/src -c $< -o $@

lib-dext: verify-source $(DK_DRIVER_OBJS) $(DK_LINUXU_OBJS)
	@echo "== lib-dext: $(DK_LIB) ($(words $(DK_DRIVER_OBJS)) upstream + $(words $(DK_LINUXU_OBJS)) linuxu objects incl. firmware rodata, driverKit platform) =="
	@mkdir -p $(DK_BUILD)
	@rm -f $(DK_LIB).tmp
	@ar rcs $(DK_LIB).tmp $(DK_DRIVER_OBJS) $(DK_LINUXU_OBJS)
	@mv $(DK_LIB).tmp $(DK_LIB)
	@echo "lib-dext: OK ($$(du -h $(DK_LIB) | cut -f1))"

# ---------------------------------------------------------------------------
# make test — build each linuxu/tests/test_*.c against all of build/linuxu/*.o
# and run it. Prints a pass/fail summary.
# ---------------------------------------------------------------------------
TEST_SRCS := $(wildcard linuxu/tests/test_*.c)
TEST_BINS := $(addprefix $(BUILD)/tests/,$(notdir $(TEST_SRCS:.c=)))

# per-test extra sources (beyond $(LINUXU_OBJS)); tests with no entry
# link against all of build/linuxu/*.o
define test_srcs
test_vram=$(BUILD)/linuxu/amdgpu-rt/vram.o
test_fake_mmio=$(BUILD)/linuxu/pci/pdev_mmio.o $(BUILD)/linuxu/pci/pci_irq_seam.o $(BUILD)/linuxu/mm/page.o
test_xarray=$(BUILD)/linuxu/xarray.o $(BUILD)/linuxu/sync.o
test_rcu=$(BUILD)/linuxu/rcu.o
test_kmemcheck=$(BUILD)/linuxu/kmem/kmemcheck.o $(BUILD)/linuxu/kmem/kmemalloc.o $(BUILD)/linuxu/shims/printk.o
test_workqueue=$(BUILD)/linuxu/work.o $(BUILD)/linuxu/shims/printk.o $(BUILD)/linuxu/timer.o $(BUILD)/linuxu/delay.o $(BUILD)/linuxu/mm/page.o
test_timer=$(BUILD)/linuxu/timer.o $(BUILD)/linuxu/delay.o $(BUILD)/linuxu/mm/page.o
test_irq=$(BUILD)/linuxu/amdgpu-rt/device.o $(BUILD)/linuxu/amdgpu-rt/irq.o $(BUILD)/linuxu/pci/pci_stub.o $(BUILD)/linuxu/dart/dma_mask.o $(BUILD)/linuxu/pci/pdev_mmio.o $(BUILD)/linuxu/shims/printk.o
test_firmware=$(BUILD)/linuxu/shims/firmware.o $(BUILD)/linuxu/shims/printk.o $(BUILD)/linuxu/fw/fw_table.o $(BUILD)/linuxu/fw/fw_mailbox.o $(FW_OBJ) $(BUILD)/linuxu/kmem/kmemalloc.o $(BUILD)/linuxu/kmem/kmemcheck.o
test_dma_dart=$(BUILD)/linuxu/dart/dart.o $(BUILD)/linuxu/dart/dma_mask.o $(BUILD)/linuxu/pci/pci_stub.o $(BUILD)/linuxu/pci/pdev_mmio.o $(BUILD)/linuxu/pci/pci_irq_seam.o $(BUILD)/linuxu/mm/page.o $(BUILD)/linuxu/kmem/kmemalloc.o $(BUILD)/linuxu/kmem/kmemcheck.o $(BUILD)/linuxu/shims/printk.o

endef
export test_srcs

test: all test-ttm-device-pool test-page-alloc-dk test-dext-alloc test-dext-time test-dext-stdio test-pci-dext test-pci-lifecycle test-platform-devres test-dext-sync test-dma-mask test-rwsem test-debugfs-lifecycle test-task-kthread test-rbtree test-dext-threads test-mutex-completion test-rcu-dk test-bootstrap test-driver-bootstrap-integration test-pseudo-fs test-jiffies test-spinlock test-wait-event test-chrdev test-class-device test-platform-policy test-xarray-limit test-drm-lifecycle
	@mkdir -p $(BUILD)/tests
	@pass=0; fail=0; skip=0; \
	if [ -z "$(TEST_SRCS)" ]; then \
		echo "test: no linuxu/tests/test_*.c found"; skip=1; \
	else \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_vram.c" ]; then \
		b=$(BUILD)/tests/test_vram; \
		echo "-- linuxu/tests/test_vram.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_vram.c build/linuxu/amdgpu-rt/vram.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_vram.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_vram.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_vram.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_vram.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_vram.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_fake_mmio.c" ]; then \
		b=$(BUILD)/tests/test_fake_mmio; \
		echo "-- linuxu/tests/test_fake_mmio.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_fake_mmio.c build/linuxu/pci/pdev_mmio.o build/linuxu/pci/pci_irq_seam.o build/linuxu/mm/page.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_fake_mmio.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_fake_mmio.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_fake_mmio.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_fake_mmio.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_fake_mmio.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_xarray.c" ]; then \
		b=$(BUILD)/tests/test_xarray; \
		echo "-- linuxu/tests/test_xarray.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_xarray.c build/linuxu/xarray.o build/linuxu/sync.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/rcu.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_xarray.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_xarray.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_xarray.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_xarray.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_xarray.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_rcu.c" ]; then \
		b=$(BUILD)/tests/test_rcu; \
		echo "-- linuxu/tests/test_rcu.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_rcu.c build/linuxu/rcu.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_rcu.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_rcu.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_rcu.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_rcu.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_rcu.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_kmemcheck.c" ]; then \
		b=$(BUILD)/tests/test_kmemcheck; \
		echo "-- linuxu/tests/test_kmemcheck.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_kmemcheck.c build/linuxu/kmem/kmemcheck.o build/linuxu/kmem/kmemalloc.o build/linuxu/shims/printk.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_kmemcheck.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_kmemcheck.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_kmemcheck.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_kmemcheck.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_kmemcheck.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_workqueue.c" ]; then \
		b=$(BUILD)/tests/test_workqueue; \
		echo "-- linuxu/tests/test_workqueue.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_workqueue.c build/linuxu/work.o build/linuxu/timer.o build/linuxu/delay.o build/linuxu/mm/page.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/shims/printk.o build/linuxu/sync.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_workqueue.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_workqueue.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_workqueue.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_workqueue.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_workqueue.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_timer.c" ]; then \
		b=$(BUILD)/tests/test_timer; \
		echo "-- linuxu/tests/test_timer.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_timer.c build/linuxu/timer.o build/linuxu/delay.o build/linuxu/mm/page.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_timer.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_timer.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_timer.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_timer.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_timer.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_firmware.c" ]; then \
		b=$(BUILD)/tests/test_firmware; \
		echo "-- linuxu/tests/test_firmware.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) -Ilinuxu/src linuxu/tests/test_firmware.c build/linuxu/shims/firmware.o build/linuxu/shims/printk.o build/linuxu/fw/fw_table.o build/linuxu/fw/fw_mailbox.o $(FW_OBJ) build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_firmware.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_firmware.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_firmware.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_firmware.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_firmware.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_dma_dart.c" ]; then \
		b=$(BUILD)/tests/test_dma_dart; \
		echo "-- linuxu/tests/test_dma_dart.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_dma_dart.c build/linuxu/dart/dart.o build/linuxu/dart/dma_mask.o build/linuxu/pci/pci_stub.o build/linuxu/kmem/slab.o build/linuxu/pci/pdev_mmio.o build/linuxu/pci/pci_irq_seam.o build/linuxu/mm/page.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/shims/printk.o build/linuxu/spinlock.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_dma_dart.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_dma_dart.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_dma_dart.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_dma_dart.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_dma_dart.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ -f "linuxu/tests/test_dart_dext_seam.c" ]; then \
		b=$(BUILD)/tests/test_dart_dext_seam; \
		echo "-- linuxu/tests/test_dart_dext_seam.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_dart_dext_seam.c build/linuxu/dart/dart.o build/linuxu/dart/dma_mask.o build/linuxu/pci/pci_stub.o build/linuxu/kmem/slab.o build/linuxu/pci/pdev_mmio.o build/linuxu/pci/pci_irq_seam.o build/linuxu/mm/page.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/shims/printk.o build/linuxu/spinlock.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_dart_dext_seam.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_dart_dext_seam.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_dart_dext_seam.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_dart_dext_seam.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_dart_dext_seam.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_irq.c" ]; then \
		b=$(BUILD)/tests/test_irq; \
		echo "-- linuxu/tests/test_irq.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_irq.c build/linuxu/amdgpu-rt/device.o build/linuxu/amdgpu-rt/irq.o build/linuxu/pci/pci_stub.o build/linuxu/kmem/slab.o build/linuxu/dart/dma_mask.o build/linuxu/pci/pdev_mmio.o build/linuxu/shims/printk.o build/linuxu/mm/page.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/spinlock.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_irq.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_irq.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_irq.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_irq.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_irq.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ -f "linuxu/tests/test_fence.c" ]; then \
		b=$(BUILD)/tests/test_fence; \
		echo "-- linuxu/tests/test_fence.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_fence.c build/linuxu/shims/timekeeping.o build/linuxu/rcu.o build/linuxu/bug.o build/linuxu/shims/dma_fence.o build/linuxu/shims/dma_fence_chain.o build/linuxu/drm/dma_resv.o build/linuxu/sync/ww_mutex.o build/linuxu/sync.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/kmem/slab.o build/linuxu/kmem/kobject.o build/linuxu/shims/sysfs.o build/linuxu/shims/printk.o build/linuxu/delay.o build/linuxu/atomic_long.o build/linuxu/refcount.o build/linuxu/work.o build/linuxu/mm/page.o build/linuxu/rwsem.o build/linuxu/rwlock.o build/linuxu/dart/dart.o build/linuxu/drm/ttm.o build/linuxu/spinlock.o build/linuxu/shims/task.o build/linuxu/shims/kthread.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_fence.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_fence.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_fence.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_fence.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_fence.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_workqueue_barrier.c" ]; then \
		b=$(BUILD)/tests/test_workqueue_barrier; \
		echo "-- linuxu/tests/test_workqueue_barrier.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_workqueue_barrier.c build/linuxu/work.o build/linuxu/shims/printk.o build/linuxu/timer.o build/linuxu/delay.o build/linuxu/sync.o build/linuxu/rwsem.o build/linuxu/mm/page.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/atomic_long.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_workqueue_barrier.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_workqueue_barrier.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_workqueue_barrier.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_workqueue_barrier.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_workqueue_barrier.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_page_alloc.c" ]; then \
		b=$(BUILD)/tests/test_page_alloc; \
		echo "-- linuxu/tests/test_page_alloc.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_page_alloc.c -O1 build/linuxu/mm/page.o build/linuxu/mm/page_ref.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/shims/printk.o build/linuxu/dart/dart.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_page_alloc.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_page_alloc.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_page_alloc.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_page_alloc.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_page_alloc.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	if [ "$(TEST_SRCS)" = "$(TEST_SRCS)" ] && [ -f "linuxu/tests/test_bo_mmap.c" ]; then \
		b=$(BUILD)/tests/test_bo_mmap; \
		echo "-- linuxu/tests/test_bo_mmap.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_bo_mmap.c build/linuxu/drm/ttm.o build/linuxu/dart/dart.o build/linuxu/mm/page.o build/linuxu/kmem/kmemalloc.o build/linuxu/kmem/kmemcheck.o build/linuxu/atomic_long.o -Wl,-dead_strip -lpthread -o $$b 2>$(BUILD)/tests/err.test_bo_mmap.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_bo_mmap.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_bo_mmap.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_bo_mmap.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_bo_mmap.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	fi; \
	if [ -f "linuxu/tests/test_dext_compute.c" ]; then \
		b=$(BUILD)/tests/test_dext_compute; \
		echo "-- linuxu/tests/test_dext_compute.c"; \
		if $(CC) $(HOSTCFLAGS) $(INCPATHS) -Idext/sources linuxu/tests/test_dext_compute.c dext/sources/dext_compute.c -o $$b 2>$(BUILD)/tests/err.test_dext_compute.log; then \
			if $$b; then echo "   PASS linuxu/tests/test_dext_compute.c"; pass=$$((pass+1)); \
			else echo "   FAIL linuxu/tests/test_dext_compute.c (runtime)"; fail=$$((fail+1)); fi; \
		else \
			echo "   FAIL linuxu/tests/test_dext_compute.c (build)"; sed -n '1,10p' $(BUILD)/tests/err.test_dext_compute.log; fail=$$((fail+1)); \
		fi; \
	fi; \
	echo "== test summary: $$pass passed, $$fail failed, $$skip skipped =="; \
	if [ $$fail -ne 0 ]; then exit 1; fi

# ---------------------------------------------------------------------------
# make dext — DriverKit extension build.
# ---------------------------------------------------------------------------
dext: lib-dext
ifeq ($(SDK),)
	@echo "DriverKit SDK not found (Xcode + DriverKit entitlement required)"
else
	@echo "== dext: DriverKit SDK = $(SDK) =="
	@mkdir -p $(BUILD)/dext-obj
	@if [ -z "$(DEXTSRC)" ]; then \
		echo "dext: no dext/sources/*.{m,mm} found"; exit 0; \
	fi; \
	ok=1; \
	for src in $(DEXTSRC); do \
		base=$$(basename $$src); \
		o=$(BUILD)/dext-obj/$${base%.*}.o; \
		lang="-x objective-c++"; \
		echo "dext: CC $$src"; \
		$(CXX) $$lang -D__KERNEL__ -DLINUXU_DEXT $(DEXTSRCFLAGS) -c $$src -o $$o 2>$(BUILD)/dext-compile.$${base}.err || { \
			echo "dext: compile FAILED for $$src"; sed -n '1,20p' $(BUILD)/dext-compile.$${base}.err; ok=0; break; }; \
	done; \
	if [ $$ok -ne 1 ]; then exit 1; fi; \
	dextobjs=$$(find $(BUILD)/dext-obj -name '*.o'); \
	if [ -f "$(DK_LIB)" ]; then \
		kmdflag="$(DK_LIB)"; \
	else \
		echo "dext: WARNING $(DK_LIB) missing (run 'make lib-dext') — linking shell only"; kmdflag=; \
	fi; \
	$(CC) -shared -o $(DEXTBUILD) $$dextobjs $$kmdflag $(DEXT_LDFLAGS) \
	&& echo "dext: linked $(DEXTBUILD)"; \
	if [ -n "$(DIDENTITY)" ]; then \
		codesign --force --sign "$(DIDENTITY)" $(DEXT_CODE_SIGN_FLAGS) \
			--entitlements dext/mac_linuxgpu.entitlements $(DEXTBUILD) \
			&& echo "dext: signed $(DEXTBUILD)"; \
	else \
		echo "dext: DIDENTITY unset — built UNSIGNED (set DIDENTITY to sign; the Xcode project signs the shipped dext)"; \
	fi; \
	if [ -n "$(DEXTTOOL)" ]; then \
		dext package -o $(DEXTPKG) $(DEXTBUILD) \
		&& echo "dext: packaged $(DEXTPKG)"; \
	else \
		echo "dext: 'dext' packaging tool not found on PATH - skipping package step"; \
	fi
endif

# ---------------------------------------------------------------------------
# make config-check — report mismatches between this table and the header.
# ---------------------------------------------------------------------------
AUTOCONF := linuxu/headers/linux/autoconf.h

config-check:
	@echo "== config-check: mk/kernel_config.mk vs $(AUTOCONF) =="
	@if [ ! -f $(AUTOCONF) ]; then \
		echo "  ($(AUTOCONF) missing; nothing to check)"; \
	else \
		sh tools/config-check.sh $(AUTOCONF); \
	fi

# clean keeps the setup stamp and the fetched firmware; distclean removes
# everything generated, including build-dk/.
clean:
	find $(BUILD) -mindepth 1 -maxdepth 1 ! -name setup ! -name firmware -exec rm -rf {} + 2>/dev/null || true
	rm -rf $(DK_BUILD)

distclean:
	rm -rf $(BUILD) $(DK_BUILD)

.PHONY: distclean

# ---------------------------------------------------------------------------
# make hsa — the userspace HSA runtime (libhsa-runtime64.dylib) + its host unit
# tests. A separate CMake project (hsa/), additive to the make build: it does
# not touch the KMD/linuxu/client targets. The dylib is what host processes
# link against; it talks to the dext over the IOKit user-client. The host
# tests run against the in-memory fake backend (MAC_LINUXGPU_FAKE_TRANSPORT=1).
# ---------------------------------------------------------------------------
hsa:
	@echo "== hsa: building libhsa-runtime64.dylib (CMake) =="
	@cmake -S hsa -B $(BUILD)/hsa -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON > $(BUILD)/hsa-cmake.log 2>&1 \
		&& cmake --build $(BUILD)/hsa --parallel 4 >> $(BUILD)/hsa-cmake.log 2>&1 \
		&& echo "hsa: built $(BUILD)/hsa/libhsa-runtime64.dylib" \
		|| { echo "hsa: BUILD FAILED (see $(BUILD)/hsa-cmake.log)"; tail -30 $(BUILD)/hsa-cmake.log; exit 1; }

hsa-test:
	@echo "== hsa-test: running the HSA runtime host unit tests =="
	@cmake -S hsa -B $(BUILD)/hsa -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON > $(BUILD)/hsa-cmake.log 2>&1 \
		&& cmake --build $(BUILD)/hsa --parallel 4 >> $(BUILD)/hsa-cmake.log 2>&1 \
		&& ctest --test-dir $(BUILD)/hsa --output-on-failure \
		|| { echo "hsa-test: FAILED (see $(BUILD)/hsa-cmake.log)"; exit 1; }

# depfiles (generated by -MMD -MP)
-include $(BUILD)/*.d $(shell find $(BUILD) -name '*.d' 2>/dev/null)
-include $(shell find $(DK_BUILD) -name '*.d' 2>/dev/null)

test-dext-alloc:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -DLINUXU_TEST_DEXT_ALLOC linuxu/tests/test_dext_alloc.c linuxu/src/shims/dext_alloc.c -o $(BUILD)/tests/test_dext_alloc
	$(BUILD)/tests/test_dext_alloc

verify-source:
	python3 scripts/verify-upstream.py

test-dext-time:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined linuxu/tests/test_dext_time.c -o $(BUILD)/tests/test_dext_time
	$(BUILD)/tests/test_dext_time
	$(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_timekeeping.c linuxu/src/shims/timekeeping.c -o $(BUILD)/tests/test_timekeeping
	$(BUILD)/tests/test_timekeeping

test-dext-stdio:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -fsanitize=address,undefined -Ilinuxu/headers linuxu/tests/test_dext_stdio.c -o $(BUILD)/tests/test_dext_stdio
	$(BUILD)/tests/test_dext_stdio

test-pci-dext: $(BUILD)/linuxu/shims/printk.o
	@mkdir -p $(BUILD)/tests
	$(CC) -w -std=gnu11 -DLINUXU_DEXT_DK=1 -ffunction-sections -fdata-sections $(INCPATHS) linuxu/tests/test_pci_dext.c linuxu/src/pci/pci_stub.c linuxu/src/dart/dma_mask.c $(BUILD)/linuxu/shims/printk.o -Wl,-dead_strip -o $(BUILD)/tests/test_pci_dext
	$(BUILD)/tests/test_pci_dext

test-pci-lifecycle:
	bash scripts/test-pci-lifecycle.sh

test-platform-devres:
	@mkdir -p $(BUILD)/tests
	$(CC) -w -std=gnu11 -D__KERNEL__ -g -O1 -fsanitize=address,undefined -ffunction-sections -fdata-sections -Ilinuxu/headers linuxu/tests/test_platform_devres.c linuxu/src/kmem/slab.c linuxu/src/shims/platform_device.c -Wl,-dead_strip -o $(BUILD)/tests/test_platform_devres
	$(BUILD)/tests/test_platform_devres

test-dext-sync:
	bash scripts/test-dext-sync.sh

test-dma-mask:
	bash scripts/test-dma-mask.sh

test-rwsem:
	bash scripts/test-rwsem.sh

test-debugfs-lifecycle:
	bash scripts/test-debugfs-lifecycle.sh

test-task-kthread:
	bash scripts/test-task-kthread.sh

test-rbtree:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -fsanitize=address,undefined -Ilinuxu/headers linuxu/tests/test_rbtree.c $(LINUX)/lib/rbtree.c -o $(BUILD)/tests/test_rbtree
	$(BUILD)/tests/test_rbtree

test-dext-threads:
	bash scripts/test-dext-threads.sh

test-pseudo-fs:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -fsanitize=address,undefined -Ilinuxu/headers linuxu/tests/test_pseudo_fs.c linuxu/src/shims/pseudo_fs.c -o $(BUILD)/tests/test_pseudo_fs
	$(BUILD)/tests/test_pseudo_fs

test-jiffies:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -fsanitize=address,undefined -Ilinuxu/headers linuxu/tests/test_jiffies.c -o $(BUILD)/tests/test_jiffies
	$(BUILD)/tests/test_jiffies

test-spinlock:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -fsanitize=address,undefined -ffunction-sections -fdata-sections -Ilinuxu/headers linuxu/tests/test_spinlock.c linuxu/src/sync.c -Wl,-dead_strip -o $(BUILD)/tests/test_spinlock
	$(BUILD)/tests/test_spinlock

test-mutex-completion:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -D__KERNEL__ -fsanitize=address,undefined -ffunction-sections -fdata-sections -Ilinuxu/headers linuxu/tests/test_mutex_completion.c linuxu/src/sync.c linuxu/src/shims/task.c linuxu/src/shims/kthread.c linuxu/src/bug.c -Wl,-dead_strip -lpthread -o $(BUILD)/tests/test_mutex_completion
	$(BUILD)/tests/test_mutex_completion

test-rcu-dk:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -D__KERNEL__ -DLINUXU_DEXT_DK=1 -fsanitize=address,undefined -Ilinuxu/headers linuxu/tests/test_rcu.c linuxu/src/rcu.c -Wl,-dead_strip -lpthread -o $(BUILD)/tests/test_rcu_dk
	$(BUILD)/tests/test_rcu_dk

test-wait-event:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -fsanitize=address,undefined -ffunction-sections -fdata-sections -Ilinuxu/headers linuxu/tests/test_wait_event.c linuxu/src/sync.c linuxu/src/delay.c linuxu/src/shims/task.c linuxu/src/shims/kthread.c linuxu/src/bug.c -Wl,-dead_strip -o $(BUILD)/tests/test_wait_event
	$(BUILD)/tests/test_wait_event

test-bootstrap:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -Ilinuxu/headers linuxu/tests/test_bootstrap.c linuxu/src/amdgpu-rt/bootstrap.c -o $(BUILD)/tests/test_bootstrap
	$(BUILD)/tests/test_bootstrap

test-driver-bootstrap-integration: lib
	bash scripts/test-driver-bootstrap-integration.sh

test-chrdev:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -fsanitize=address,undefined -Ilinuxu/headers linuxu/tests/test_chrdev.c linuxu/src/shims/chrdev.c linuxu/src/shims/module.c -o $(BUILD)/tests/test_chrdev
	$(BUILD)/tests/test_chrdev

test-class-device:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -fsanitize=address,undefined -Ilinuxu/headers linuxu/tests/test_class_device.c linuxu/src/kmem/kobject.c linuxu/src/sysfs.c linuxu/src/shims/sysfs.c -o $(BUILD)/tests/test_class_device
	$(BUILD)/tests/test_class_device

test-platform-policy:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -w -Ilinuxu/headers linuxu/tests/test_platform_policy.c linuxu/src/shims/platform_policy.c -o $(BUILD)/tests/test_platform_policy
	$(BUILD)/tests/test_platform_policy
	bash scripts/test-pm-notifier.sh

test-xarray-limit:
	@mkdir -p $(BUILD)/tests
	$(CC) -std=gnu11 -g -O1 -w -fsanitize=address,undefined -Ilinuxu/headers linuxu/tests/test_xarray_limit.c linuxu/src/xarray.c linuxu/src/rcu.c -lpthread -o $(BUILD)/tests/test_xarray_limit
	$(BUILD)/tests/test_xarray_limit

test-drm-lifecycle: lib
	@mkdir -p $(BUILD)/tests
	$(CC) $(HOSTCFLAGS) $(INCPATHS) linuxu/tests/test_drm_lifecycle.c $(BUILD)/libmacamgdu.a -lpthread -o $(BUILD)/tests/test_drm_lifecycle
	$(BUILD)/tests/test_drm_lifecycle

test-ttm-device-pool: lib
	bash scripts/test-ttm-device-pool.sh

test-page-alloc-dk:
	bash scripts/test-page-alloc-dk.sh

test-kmem-cache-lifecycle:
	bash scripts/test-kmem-cache-lifecycle.sh

test-probe-cleanup:
	bash scripts/test-probe-cleanup.sh

test-iokit-dma:
	bash scripts/test-iokit-dma.sh

test-workqueue-lifetime:
	bash scripts/test-workqueue-lifetime.sh

test-workqueue-concurrency:
	bash scripts/test-workqueue-concurrency.sh

test: test-workqueue-concurrency
.PHONY: test-workqueue-concurrency

test-atomic-semantics:
	bash scripts/test-atomic-semantics.sh

test-aql-storage-bounds:
	bash scripts/test-aql-storage-bounds.sh

test-ww-mutex:
	bash scripts/test-ww-mutex.sh

test-compute-dispatch:
	bash scripts/test-compute-dispatch.sh

test-compute-lifetime:
	bash scripts/test-compute-lifetime.sh

test-irq-lifetime:
	bash scripts/test-irq-lifetime.sh

test-xarray-safety:
	bash scripts/test-xarray-safety.sh

test-mm-boundaries:
	bash scripts/test-mm-boundaries.sh

test-page-boundaries:
	bash scripts/test-page-boundaries.sh

test-dext-compute-production:
	bash scripts/test-dext-compute-production.sh

test-drm-exec-lifetime:
	bash scripts/test-drm-exec-lifetime.sh

test-fence-lifetime:
	bash scripts/test-fence-lifetime.sh

test-upstream-pci-failure: lib
	bash scripts/test-upstream-pci-failure.sh

test-bitmap-contracts:
	bash scripts/test-bitmap-contracts.sh

test-file-lifetime:
	bash scripts/test-file-lifetime.sh

test-lock-headers:
	bash scripts/test-lock-headers.sh

test-clock-domains:
	bash scripts/test-clock-domains.sh

test-srcu-lifetime:
	bash scripts/test-srcu-lifetime.sh

test-memory-containers:
	bash scripts/test-memory-containers.sh

test-string-contracts:
	bash scripts/test-string-contracts.sh

test-pci-mmio-bounds:
	bash scripts/test-pci-mmio-bounds.sh

test-rt-memory-ownership:
	bash scripts/test-rt-memory-ownership.sh

test-dma-buf-lifetime:
	bash scripts/test-dma-buf-lifetime.sh

test-hwmon-lifetime:
	bash scripts/test-hwmon-lifetime.sh

test-hsa-shim-init:
	bash scripts/test-hsa-shim-init.sh

test-shmem-ownership:
	bash scripts/test-shmem-ownership.sh

test-hmm-ownership:
	bash scripts/test-hmm-ownership.sh

test-kthread-worker:
	bash scripts/test-kthread-worker.sh

test-waitqueue-tasks:
	bash scripts/test-waitqueue-tasks.sh

test-timer-service:
	bash scripts/test-timer-service.sh

# hrtimer on the timer service; per-task per-CPU copies and kernel FPU
# sections (Display Core's DC_FP_START/END depth).
test-hrtimer:
	bash scripts/test-hrtimer.sh

test-percpu-fpu:
	bash scripts/test-percpu-fpu.sh

test: test-hrtimer test-percpu-fpu
.PHONY: test-hrtimer test-percpu-fpu

test-devres-concurrency:
	bash scripts/test-devres-concurrency.sh

test-native-ipc:
	bash scripts/test-native-ipc.sh

test-hsa-ipc-lifetime:
	bash scripts/test-hsa-ipc-lifetime.sh

test-shmem-driverkit-alias:
	bash scripts/test-shmem-driverkit-alias.sh

test-raw-bar-lease:
	bash scripts/test-raw-bar-lease.sh

test-session-shutdown:
	bash scripts/test-session-shutdown.sh

test-klog:
	bash scripts/test-klog.sh

test-read-driver-log:
	bash scripts/test-read-driver-log.sh

test-upstream-pci-matching:
	bash scripts/test-upstream-pci-matching.sh

test-upstream-rlc-firmware:
	bash scripts/test-upstream-rlc-firmware.sh

test-firmware-cache:
	bash scripts/test-firmware-cache.sh

test-device-matching-plist:
	bash scripts/test-device-matching-plist.sh

test-zero-allocations:
	bash scripts/test-zero-allocations.sh

test-gpu-buddy:
	bash scripts/test-gpu-buddy.sh

test-partial-ttm-cleanup:
	bash scripts/test-partial-ttm-cleanup.sh

test-sysinfo-ttm:
	bash scripts/test-sysinfo-ttm.sh

test-upstream-preempt-sysfs:
	bash scripts/test-upstream-preempt-sysfs.sh

test-kernel-helpers:
	bash scripts/test-kernel-helpers.sh

test-primitive-headers:
	bash scripts/test-primitive-headers.sh

test-upstream-dma-pool:
	bash scripts/test-upstream-dma-pool.sh

test-ttm-lru-cleanup:
	bash scripts/test-ttm-lru-cleanup.sh

# Userspace-only crash regressions: mocked DriverKit/PCI boundaries, real shim
# implementations and upstream error cleanup. No installed driver is opened.
CRASH_PATH_TESTS := test-dext-alloc test-debugfs-lifecycle test-kmem-cache-lifecycle \
	test-probe-cleanup test-upstream-pci-failure test-page-alloc-dk test-iokit-dma \
	test-pci-dext test-pci-lifecycle test-platform-devres test-dext-sync \
	test-dext-threads test-task-kthread test-rwsem test-rcu-dk \
	test-mutex-completion test-wait-event test-drm-lifecycle test-workqueue-lifetime \
	test-atomic-semantics test-aql-storage-bounds test-ww-mutex \
	test-compute-dispatch test-compute-lifetime test-irq-lifetime \
	test-xarray-safety test-mm-boundaries test-page-boundaries \
	test-dext-compute-production test-drm-exec-lifetime test-fence-lifetime

CRASH_PATH_TESTS += test-bitmap-contracts test-file-lifetime test-lock-headers test-clock-domains test-srcu-lifetime test-memory-containers test-string-contracts test-pci-mmio-bounds test-rt-memory-ownership
CRASH_PATH_TESTS += test-dma-buf-lifetime test-hwmon-lifetime test-hsa-shim-init \
	test-shmem-ownership test-kthread-worker test-waitqueue-tasks test-timer-service \
	test-devres-concurrency test-bootstrap test-hmm-ownership
CRASH_PATH_TESTS += test-native-ipc test-hsa-ipc-lifetime test-shmem-driverkit-alias test-raw-bar-lease test-session-shutdown
CRASH_PATH_TESTS += test-klog test-read-driver-log test-upstream-pci-matching
CRASH_PATH_TESTS += test-upstream-rlc-firmware test-zero-allocations
CRASH_PATH_TESTS += test-gpu-buddy test-partial-ttm-cleanup
CRASH_PATH_TESTS += test-sysinfo-ttm test-upstream-preempt-sysfs
CRASH_PATH_TESTS += test-kernel-helpers test-primitive-headers test-upstream-dma-pool
CRASH_PATH_TESTS += test-ttm-lru-cleanup
CRASH_PATH_TESTS += test-device-string
CRASH_PATH_TESTS += test-pci-fault-containment
CRASH_PATH_TESTS += test-sysfs-read test-drm-info test-read-sysfs
CRASH_PATH_TESTS += test-platform-policy
CRASH_PATH_TESTS += test-hrtimer test-percpu-fpu
CRASH_PATH_TESTS += test-queue-partition

test-queue-partition:
	bash scripts/test-queue-partition.sh

.PHONY: test-queue-partition

# linuxu process substrate (task/mm/files, notifiers, uaccess, signals) and
# upstream kfd_create_process driven through it.
CRASH_PATH_TESTS += test-process-substrate test-kfd-process
test: test-process-substrate test-kfd-process

test-process-substrate:
	bash scripts/test-process-substrate.sh

test-kfd-process:
	bash scripts/test-kfd-process.sh

.PHONY: test-process-substrate test-kfd-process

# KFD compute sessions: a runtime client as a KFD process (chardev ioctls,
# GPUVM memory, MES queues) over upstream KFD with a fixture device.
CRASH_PATH_TESTS += test-kfd-session
test: test-kfd-session

test-kfd-session:
	bash scripts/test-kfd-session.sh

.PHONY: test-kfd-session

test-device-topology:
	bash scripts/test-device-topology.sh

.PHONY: test-device-topology
CRASH_PATH_TESTS += test-workqueue-concurrency
CRASH_PATH_TESTS += test-stub-policy

test-pci-fault-containment:
	bash scripts/test-pci-fault-containment.sh

# Fail-safe generated-stub policy, stub reachability from probe/open/KFD
# roots, and the Linux contracts of services that replaced former stubs.
test-stub-policy: lib
	HOSTCFLAGS="$(HOSTCFLAGS)" INCPATHS="$(INCPATHS)" bash scripts/test-stub-policy.sh

.PHONY: test-stub-policy

.PHONY: test-pci-fault-containment

CRASH_PATH_TESTS += test-fatal

test-fatal:
	bash scripts/test-fatal.sh

.PHONY: test-fatal

test-device-string:
	bash scripts/test-device-string.sh

.PHONY: test-device-string

test-crash-paths: export UBSAN_OPTIONS = halt_on_error=1:abort_on_error=0
test-crash-paths: verify-source $(CRASH_PATH_TESTS)
	@echo "Offline crash-path regressions passed; hardware safety is not established by these checks."

test: test-kmem-cache-lifecycle test-probe-cleanup test-iokit-dma test-upstream-pci-failure test-workqueue-lifetime test-atomic-semantics test-aql-storage-bounds
test: test-ww-mutex test-compute-dispatch test-compute-lifetime test-irq-lifetime test-xarray-safety test-mm-boundaries test-page-boundaries test-dext-compute-production
test: test-drm-exec-lifetime test-fence-lifetime
test: test-gpu-buddy test-partial-ttm-cleanup
test: test-sysinfo-ttm test-upstream-preempt-sysfs
test: test-kernel-helpers test-primitive-headers test-upstream-dma-pool
test: test-ttm-lru-cleanup
test: test-queue-partition

.PHONY: test-kernel-helpers test-primitive-headers test-upstream-dma-pool
.PHONY: test-ttm-lru-cleanup

.PHONY: test-gpu-buddy test-partial-ttm-cleanup
.PHONY: test-sysinfo-ttm test-upstream-preempt-sysfs

.PHONY: test-drm-exec-lifetime test-fence-lifetime

.PHONY: test-ww-mutex test-compute-dispatch test-compute-lifetime test-irq-lifetime test-xarray-safety test-mm-boundaries test-page-boundaries test-dext-compute-production

.PHONY: test-crash-paths test-kmem-cache-lifecycle test-probe-cleanup test-iokit-dma test-upstream-pci-failure test-workqueue-lifetime test-atomic-semantics test-aql-storage-bounds

.PHONY: test-ttm-device-pool test-page-alloc-dk all test dext clean config-check hsa hsa-test test-dext-alloc verify-source test-dext-time test-dext-stdio test-pci-dext test-pci-lifecycle test-platform-devres test-dext-sync test-dma-mask test-rwsem test-debugfs-lifecycle test-task-kthread test-rbtree test-dext-threads test-mutex-completion test-rcu-dk test-bootstrap test-driver-bootstrap-integration test-pseudo-fs test-jiffies test-spinlock test-wait-event test-chrdev test-class-device test-platform-policy test-xarray-limit test-drm-lifecycle

test: test-bitmap-contracts test-file-lifetime test-lock-headers test-clock-domains test-srcu-lifetime test-memory-containers test-string-contracts test-pci-mmio-bounds test-rt-memory-ownership
.PHONY: test-bitmap-contracts test-file-lifetime test-lock-headers test-clock-domains test-srcu-lifetime test-memory-containers test-string-contracts test-pci-mmio-bounds test-rt-memory-ownership

test: test-dma-buf-lifetime test-hwmon-lifetime test-hsa-shim-init test-shmem-ownership \
	test-kthread-worker test-waitqueue-tasks test-timer-service test-devres-concurrency test-hmm-ownership
.PHONY: test-dma-buf-lifetime test-hwmon-lifetime test-hsa-shim-init test-shmem-ownership \
	test-kthread-worker test-waitqueue-tasks test-timer-service test-devres-concurrency test-hmm-ownership

test: test-native-ipc test-hsa-ipc-lifetime test-shmem-driverkit-alias test-raw-bar-lease test-session-shutdown
.PHONY: test-native-ipc test-hsa-ipc-lifetime test-shmem-driverkit-alias test-raw-bar-lease test-session-shutdown

test: test-klog test-read-driver-log test-upstream-pci-matching
.PHONY: test-klog test-read-driver-log test-upstream-pci-matching

test: test-upstream-rlc-firmware test-zero-allocations
.PHONY: test-upstream-rlc-firmware test-zero-allocations
test: test-firmware-cache test-device-matching-plist
.PHONY: test-firmware-cache test-device-matching-plist

test: test-stub-policy

# Linux read paths for observers: in-memory sysfs reads (show()/bin read),
# the upstream PM and memory attributes through them, the AMDGPU_INFO
# reader, and the read-sysfs.py client.
test-sysfs-read:
	bash scripts/test-sysfs-read.sh

test-upstream-pm-sysfs:
	bash scripts/test-upstream-pm-sysfs.sh

test-drm-info:
	bash scripts/test-drm-info.sh

test-read-sysfs:
	bash scripts/test-read-sysfs.sh

test: test-sysfs-read test-upstream-pm-sysfs test-drm-info test-read-sysfs
.PHONY: test-sysfs-read test-upstream-pm-sysfs test-drm-info test-read-sysfs
