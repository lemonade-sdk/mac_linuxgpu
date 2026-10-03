# linuxu tests

Standalone unit tests for the linuxu shim layer. Each test is a plain
`main()` program using a local `EXPECT()` macro (no test framework —
the dext cannot link gtest/libc-test deps).

## Build & run

```sh
cd <repo>/linuxu
# host build (no DriverKit): -fsyntax-only first, then real link
clang -std=gnu11 -D__KERNEL__ -I<repo>/linuxu/headers \
  <test>.c <required shim .c files> -lpthread -o <test>
./<test>
```

The `make test` target (top-level Makefile) builds and runs all of
them.  Per-test required shim files:

| test              | links with (shim sources)                                   |
|-------------------|-------------------------------------------------------------|
| test_kmemcheck    | kmem/kmemcheck.c kmem/kmemalloc.c kmem/string.c shims/printk.c |
| test_fake_mmio    | pci/pdev_mmio.c                                            |
| test_vram         | amdgpu-rt/vram.c                                           |
| test_irq          | amdgpu-rt/device.c pci/pci_stub.c shims/printk.c           |
| test_workqueue    | work.c sync.c shims/printk.c                               |
| test_timer        | timer.c sync.c shims/printk.c                              |
| test_xarray       | xarray.c                                                   |
| test_rcu          | rcu.c shims/printk.c                                       |

## What is REAL vs STUB

- **REAL** (assertions exercise the actual algorithm): kmemcheck
  canaries, fake-MMIO token table + 16KB slots + readl/writel dispatch,
  vram bump allocator + pfn mapping, irq register/inject/unregister,
  workqueue (pthread worker + jiffy-delayed promotion +
  cancel_delayed_work_sync), timer_list (4ms reaper), xarray
  (node tree + xa_alloc), rcu (per-thread defer list + synchronize
  barrier).
- **STUB** (documented, no assertions beyond "does not crash"):
  i2c (xfer → -ENOENT), acpi, mmu_notifier, dev_coredump, module,
  firmware (loader is REAL for the host build; the dext build swaps
  in the embedded-table path), debugfs/sysfs (table no-ops — could be
  extended with a `test_debugfs` later).

## Running without a GPU

All tests run in-process on the host with no GPU, no IOKit, and no
root.  The fake-MMIO and vram backends use host shadow buffers; the
`LINUXU_DEXT` compile flag switches them to the IOPCIDevice
backends (compiled only for the dext bundle, not for these tests).
