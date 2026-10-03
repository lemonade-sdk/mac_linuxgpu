"""Stub policy for scripts/gen_stubs.py.

Every symbol in linuxu/UNDEFINED_SYMBOLS.txt is classified here. The
generator fails on any unclassified symbol, on an `implement` entry, and on
a body that would return a negative errno through an unsigned, enum, bool
or pointer type. See the gen_stubs.py docstring for the category contract.

Entry forms:
  "sym": ("category", "reason or defining file")
  "sym": {"cat": ..., "why": ..., optional keys}
Optional keys:
  header       disabled-inline: header under third_party/linux/include whose
               !CONFIG static inline body is copied.
  body/marker  disabled-inline whose vendor stub is macro-generated: the
               body to emit and a string the vendor header must contain.
  ret          unreachable: benign return expression instead of 0/NULL/false.
  err_encoded  unreachable: allow an IS_ERR_VALUE() encoded errno through an
               `unsigned long` return (Linux vm_mmap() convention).
  proto        full prototype when no declaration can be extracted.
  def          data: the object definition, emitted verbatim.
  where        external: provenance when the definition is macro-generated.

Reach analysis: scripts/stub_reach.py lists which generated stubs upstream
code names from amdgpu_device_init, amdgpu_driver_open_kms and
kfd_create_process; scripts/test-stub-policy.sh requires that none of
those returns a negative value.
"""

# Headers the generated file needs beyond the declaring headers that the
# generator adds automatically.
STUB_INCLUDES = [
    "linux/kernel.h",
    "linux/smp.h",
    "linux/suspend.h",
    "amdgpu.h",
    "amdgpu_ras_mgr.h",
    "drm/drm_crtc.h",
    "drm/drm_modeset_helper_vtables.h",
    "drm/drm_privacy_screen_consumer.h",
    "linux/vgaarb.h",
    "linux/eventfd.h",
    "linux/acpi.h",
]

_UNIRAS = ("unified RAS manager (ras/ tree) is deferred; callers are "
           "gated by amdgpu_uniras_enabled(), false in ras_policy.c, or "
           "belong to unsupported GFX/GMC 12.1 interrupt sources")
_VF_RAS = "SR-IOV virtual-function RAS; the DriverKit service is bare metal"
_IRQ_DOMAIN = ("Linux irq_domain for ACP/ISP child devices (both =n) and "
               "the ih_v6_1 APU path; not used by supported dGPUs")

POLICY = {
    # ---------------------------------------------------------------- data
    "__cpu_online_mask": {
        "cat": "data", "why": "one CPU online for cpumask iteration",
        "def": "struct cpumask __cpu_online_mask = { .bits = { 1 } };"},
    "system_state": {
        "cat": "data",
        "why": "never equals a shutdown state; the shim's SYSTEM_* numbering "
               "differs from Linux enum system_states",
        "def": "int system_state = 0;"},
    "ras_v1_0_ip_block": {
        "cat": "data",
        "why": "MP0 13.0.6/12/14 only; a named block without callbacks keeps "
               "amdgpu_device_ip_block_add() and the IP walkers from "
               "dereferencing a NULL funcs table",
        "def": "static const struct amd_ip_funcs ras_v1_0_unsupported_funcs = {\n"
               "\t.name = \"ras_v1_0 (unsupported)\",\n"
               "};\n"
               "const struct amdgpu_ip_block_version ras_v1_0_ip_block = {\n"
               "\t.type = AMD_IP_BLOCK_TYPE_RAS,\n"
               "\t.major = 1,\n"
               "\t.minor = 0,\n"
               "\t.rev = 0,\n"
               "\t.funcs = &ras_v1_0_unsupported_funcs,\n"
               "};"},

    # ----------------------------------------------------- disabled-inline
    "acpi_get_table": {
        "cat": "disabled-inline", "header": "acpi/platform/aclinux.h",
        "marker": "static ACPI_INLINE prototype {return(AE_NOT_CONFIGURED);}",
        "why": "CONFIG_ACPI=n ACPICA stub (ACPI_EXTERNAL_RETURN_STATUS)",
        "body": "(void)signature; (void)instance; (void)table;\n"
                "\treturn 0x001C; /* AE_NOT_CONFIGURED */"},
    "acpi_put_table": {
        "cat": "disabled-inline", "header": "acpi/platform/aclinux.h",
        "marker": "static ACPI_INLINE prototype {return;}",
        "why": "CONFIG_ACPI=n ACPICA stub (ACPI_EXTERNAL_RETURN_VOID)",
        "body": "(void)table;"},
    "vga_client_register": {
        "cat": "disabled-inline", "header": "linux/vgaarb.h",
        "why": "CONFIG_VGA_ARB=n"},
    "vga_client_unregister": {
        "cat": "disabled-inline", "header": "linux/vgaarb.h",
        "why": "upstream inline wrapper over vga_client_register()"},
    "eventfd_ctx_put": {
        "cat": "disabled-inline", "header": "linux/eventfd.h",
        "why": "CONFIG_EVENTFD=n; eventfd_ctx_fdget() never hands out a context"},

    # ------------------------------------------------------------- ok-zero
    "component_add": ("ok-zero",
                      "no aggregate master ever binds; Linux component_add() "
                      "also returns 0 when no master is waiting"),
    "video_get_options": ("ok-zero",
                          "no kernel command line, so no video= option exists"),

    # ---------------------------------------------------------------- noop
    "add_taint": ("noop", "no kernel taint flags in a userspace service"),
    "component_del": ("noop", "pairs with component_add(); nothing registered"),
    "unmap_mapping_range": (
        "noop",
        "no Linux CPU page tables map BO address spaces; DriverKit client "
        "mappings are owned by the dext memory descriptors"),

    # --------------------------------------------------------- unreachable
    "amdgpu_ras_mgr_dispatch_interrupt": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_gen_ras_event_seqno": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_handle_consumer_interrupt": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_handle_controller_interrupt": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_handle_fatal_interrupt": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_handle_ras_cmd": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_is_rma": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_lookup_bad_pages_in_a_row": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_post_reset": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_pre_reset": ("unreachable", _UNIRAS),
    "amdgpu_ras_mgr_update_ras_ecc": ("unreachable", _UNIRAS),
    "amdgpu_virt_ras_check_address_validity": ("unreachable", _VF_RAS),
    "amdgpu_virt_ras_convert_retired_address": ("unreachable", _VF_RAS),
    "amdgpu_virt_ras_set_remote_uniras": ("unreachable", _VF_RAS),



    "handle_simple_irq": {"cat": "unreachable", "why": _IRQ_DOMAIN,
                          "ret": "IRQ_NONE"},
    "irq_create_mapping": ("unreachable", _IRQ_DOMAIN + "; 0 = no mapping"),
    "irq_domain_create_linear": (
        "unreachable",
        _IRQ_DOMAIN + "; NULL makes amdgpu_irq_add_domain() fail cleanly"),
    "irq_domain_remove": ("unreachable", _IRQ_DOMAIN),
    "irq_set_chip_and_handler": ("unreachable", _IRQ_DOMAIN),

    "vm_mmap": {
        "cat": "unreachable", "err_encoded": True, "ret": "-ENODEV",
        "why": "only kfd_process_init_cwsr_apu() (APU CWSR trap area); the "
               "IS_ERR_VALUE() error makes KFD fail the APU mmap instead of "
               "copying the trap handler through a NULL kernel address"},
}

# --------------------------------------------------------------- external
# Defined by hand-written shims, DriverKit adapters (dext/sources) or pinned
# upstream files. The generator checks that each still has a definition
# outside the generated file. Note: kernel_api_stubs_manual.c and
# kernel_api_stubs_incomplete.c are hand-written stubs, not real services.
POLICY.update({
    "__bitmap_and":                              ("external", "linuxu/src/shims/bitmap.c"),
    "__bitmap_complement":                       ("external", "linuxu/src/shims/bitmap.c"),
    "__bitmap_intersects":                       ("external", "linuxu/src/shims/bitmap.c"),
    "__bitmap_or":                               ("external", "linuxu/src/shims/bitmap.c"),
    "__bitmap_weight":                           ("external", "linuxu/src/shims/bitmap.c"),
    "__kfifo_alloc_node":                        ("external", "third_party/linux/lib/kfifo.c"),
    "__kfifo_free":                              ("external", "third_party/linux/lib/kfifo.c"),
    "__kfifo_in":                                ("external", "third_party/linux/lib/kfifo.c"),
    "__kfifo_in_r":                              ("external", "third_party/linux/lib/kfifo.c"),
    "__kfifo_out":                               ("external", "third_party/linux/lib/kfifo.c"),
    "__kfifo_out_linear":                        ("external", "third_party/linux/lib/kfifo.c"),
    "__rb_erase_color":                          ("external", "third_party/linux/lib/rbtree.c"),
    "__rb_insert_augmented":                     ("external", "third_party/linux/lib/rbtree.c"),
    "__sprintf_chk":                             ("external", "linuxu/src/shims/dext_stdio.c"),
    "__stderrp":                                 ("external", "linuxu/src/shims/dext_stdio.c"),
    "__strcpy_chk":                              ("external", "linuxu/src/shims/dext_stdio.c"),
    "aligned_alloc":                             ("external", "linuxu/src/shims/dext_alloc.c"),
    "alloc_anon_inode":                          ("external", "linuxu/src/shims/pseudo_fs.c"),
    "amdgpu_ras_check_bad_page_status":          ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_debugfs_eeprom_size_ops":        ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_debugfs_eeprom_table_ops":       ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_debugfs_set_ret_size":           ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_append":                  ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_check":                   ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_check_err_threshold":     ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_init":                    ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_max_record_count":        ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_read":                    ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_read_idx":                ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_reset_table":             ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_eeprom_update_record_num":       ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_ras_smu_eeprom_supported":           ("external", "third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_ras_eeprom.c"),
    "amdgpu_uniras_enabled":                     ("external", "linuxu/src/shims/ras_policy.c"),
    "amdgpu_xcp_drm_dev_alloc":                  ("external", "third_party/linux/drivers/gpu/drm/amd/amdxcp/amdgpu_xcp_drv.c"),
    "amdgpu_xcp_drm_dev_free":                   ("external", "third_party/linux/drivers/gpu/drm/amd/amdxcp/amdgpu_xcp_drv.c"),
    "anon_inode_getfile":                        ("external", "linuxu/src/shims/fd.c"),
    "bitmap_free":                               ("external", "linuxu/src/shims/bitmap.c"),
    "bitmap_from_arr32":                         ("external", "linuxu/src/shims/bitmap.c"),
    "bitmap_to_arr32":                           ("external", "linuxu/src/shims/bitmap.c"),
    "bitmap_zalloc":                             ("external", "linuxu/src/shims/bitmap.c"),
    "calloc":                                    ("external", "linuxu/src/shims/dext_alloc.c"),
    "cancel_work":                               ("external", "linuxu/src/work.c"),
    "clock_gettime":                             ("external", "linuxu/src/shims/dext_time.c"),
    "crc16":                                     ("external", "third_party/linux/lib/crc/crc16.c"),
    "device_create_file":                        ("external", "linuxu/src/shims/sysfs.c"),
    "device_remove_file":                        ("external", "linuxu/src/shims/sysfs.c"),
    "devm_release_action":                       ("external", "linuxu/src/kmem/slab.c"),
    "dext_dma_alloc_coherent":                   ("external", "dext/sources/iokit_bridge.mm"),
    "dext_dma_free_coherent":                    ("external", "dext/sources/iokit_bridge.mm"),
    "dext_irq_dispatch":                         ("external", "dext/sources/dext_main.mm"),
    "dext_irq_fini":                             ("external", "dext/sources/dext_main.mm"),
    "dext_set_pci":                              ("external", "dext/sources/dext_main.mm"),
    "dma_alloc_attrs":                           ("external", "linuxu/src/mm/page.c"),
    "dma_buf_attach":                            ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_detach":                            ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_dynamic_attach":                    ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_export":                            ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_fd":                                ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_get":                               ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_invalidate_mappings":               ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_map_attachment":                    ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_map_attachment_unlocked":           ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_pin":                               ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_put":                               ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_unmap_attachment":                  ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_unmap_attachment_unlocked":         ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_buf_unpin":                             ("external", "linuxu/src/shims/dma_buf.c"),
    "dma_fence_wait":                            ("external", "linuxu/src/shims/dma_fence.c"),
    "dma_free_attrs":                            ("external", "linuxu/src/mm/page.c"),
    "drm_buddy_print":                           ("external", "third_party/linux/drivers/gpu/drm/drm_buddy.c"),
    "drm_debugfs_dev_fini":                      ("external", "third_party/linux/drivers/gpu/drm/drm_debugfs.c"),
    "drm_debugfs_dev_init":                      ("external", "third_party/linux/drivers/gpu/drm/drm_debugfs.c"),
    "drm_debugfs_dev_register":                  ("external", "third_party/linux/drivers/gpu/drm/drm_debugfs.c"),
    "drm_debugfs_register":                      ("external", "third_party/linux/drivers/gpu/drm/drm_debugfs.c"),
    "drm_debugfs_unregister":                    ("external", "third_party/linux/drivers/gpu/drm/drm_debugfs.c"),
    "drm_dp_channel_eq_ok":                      ("external", "linuxu/src/drm/dp_helper.c"),
    "drm_dp_clock_recovery_ok":                  ("external", "linuxu/src/drm/dp_helper.c"),
    "drm_dp_get_adjust_request_pre_emphasis":    ("external", "linuxu/src/drm/dp_helper.c"),
    "drm_dp_get_adjust_request_voltage":         ("external", "linuxu/src/drm/dp_helper.c"),
    "drm_dp_link_rate_to_bw_code":               ("external", "linuxu/src/drm/dp_helper.c"),
    "drm_dp_link_train_channel_eq_delay":        ("external", "linuxu/src/drm/dp_helper.c"),
    "drm_dp_link_train_clock_recovery_delay":    ("external", "linuxu/src/drm/dp_helper.c"),
    "drm_get_panel_orientation_quirk":           ("external", "linuxu/src/drm/dp_helper.c"),
    "drm_simple_encoder_init":                   ("external", "linuxu/src/shims/kernel_api_stubs_manual.c"),
    "drm_suballoc_free":                         ("external", "third_party/linux/drivers/gpu/drm/drm_suballoc.c"),
    "drm_suballoc_manager_fini":                 ("external", "third_party/linux/drivers/gpu/drm/drm_suballoc.c"),
    "drm_suballoc_manager_init":                 ("external", "third_party/linux/drivers/gpu/drm/drm_suballoc.c"),
    "drm_suballoc_new":                          ("external", "third_party/linux/drivers/gpu/drm/drm_suballoc.c"),
    "dynamic_pr_debug":                          ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "emergency_restart":                         ("external", "linuxu/src/shims/kernel_api_stubs_manual.c"),
    "fflush":                                    ("external", "linuxu/src/shims/dext_stdio.c"),
    "fprintf":                                   ("external", "linuxu/src/shims/dext_stdio.c"),
    "fputc":                                     ("external", "linuxu/src/shims/dext_stdio.c"),
    "free":                                      ("external", "linuxu/src/shims/dext_alloc.c (+1)"),
    "fwnode_handle_put":                         ("external", "linuxu/src/shims/kernel_api_stubs_manual.c"),
    "generic_handle_domain_irq":                 ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "gpu_buddy_alloc_blocks":                    ("external", "third_party/linux/drivers/gpu/buddy.c"),
    "gpu_buddy_block_trim":                      ("external", "third_party/linux/drivers/gpu/buddy.c"),
    "gpu_buddy_fini":                            ("external", "third_party/linux/drivers/gpu/buddy.c"),
    "gpu_buddy_free_list":                       ("external", "third_party/linux/drivers/gpu/buddy.c"),
    "gpu_buddy_init":                            ("external", "third_party/linux/drivers/gpu/buddy.c"),
    "gpu_buddy_reset_clear":                     ("external", "third_party/linux/drivers/gpu/buddy.c"),
    "hdmi_avi_infoframe_init":                   ("external", "third_party/linux/drivers/video/hdmi.c"),
    "hdmi_avi_infoframe_pack":                   ("external", "third_party/linux/drivers/video/hdmi.c"),
    "hdmi_vendor_infoframe_init":                ("external", "third_party/linux/drivers/video/hdmi.c"),
    "hwmon_device_register_with_groups":         ("external", "linuxu/src/shims/hwmon.c"),
    "hwmon_device_unregister":                   ("external", "linuxu/src/shims/hwmon.c"),
    "ida_alloc_max":                             ("external", "linuxu/src/xarray.c"),
    "ida_alloc_range":                           ("external", "linuxu/src/xarray.c"),
    "ida_destroy":                               ("external", "linuxu/src/xarray.c"),
    "ida_free":                                  ("external", "linuxu/src/xarray.c"),
    "idr_alloc":                                 ("external", "linuxu/src/xarray.c"),
    "idr_destroy":                               ("external", "linuxu/src/xarray.c"),
    "idr_find":                                  ("external", "linuxu/src/xarray.c"),
    "idr_for_each":                              ("external", "linuxu/src/xarray.c"),
    "idr_full_find":                             ("external", "linuxu/src/xarray.c"),
    "idr_remove":                                ("external", "linuxu/src/xarray.c"),
    "idr_replace":                               ("external", "linuxu/src/xarray.c"),
    "init_pseudo":                               ("external", "linuxu/src/shims/pseudo_fs.c"),
    "iput":                                      ("external", "linuxu/src/shims/pseudo_fs.c"),
    "isascii":                                   ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "isdigit":                                   ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "isgraph":                                   ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "isspace":                                   ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "isxdigit":                                  ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "kill_anon_super":                           ("external", "linuxu/src/shims/pseudo_fs.c"),
    "ksys_sync_helper":                          ("external", "linuxu/src/shims/kernel_api_stubs_manual.c"),
    "kthread_cancel_work_sync":                  ("external", "linuxu/src/shims/kthread.c"),
    "kthread_create_on_node":                    ("external", "linuxu/src/shims/kthread.c"),
    "kthread_destroy_worker":                    ("external", "linuxu/src/shims/kthread.c"),
    "kthread_flush_work":                        ("external", "linuxu/src/shims/kthread.c"),
    "kthread_flush_worker":                      ("external", "linuxu/src/shims/kthread.c"),
    "kthread_init_work":                         ("external", "linuxu/src/shims/kthread.c"),
    "kthread_queue_work":                        ("external", "linuxu/src/shims/kthread.c"),
    "kthread_run_worker":                        ("external", "linuxu/src/shims/kthread.c"),
    "kthread_should_stop":                       ("external", "linuxu/src/shims/kthread.c"),
    "kthread_stop":                              ("external", "linuxu/src/shims/kthread.c"),
    "ktime_get":                                 ("external", "linuxu/src/shims/timekeeping.c"),
    "ktime_get_boottime":                        ("external", "linuxu/src/shims/timekeeping.c"),
    "ktime_get_boottime_ns":                     ("external", "linuxu/src/shims/timekeeping.c"),
    "ktime_get_mono_fast_ns":                    ("external", "linuxu/src/shims/timekeeping.c"),
    "ktime_get_raw_ns":                          ("external", "linuxu/src/shims/timekeeping.c"),
    "ktime_get_real_seconds":                    ("external", "linuxu/src/shims/timekeeping.c"),
    "ktime_get_ts64":                            ("external", "linuxu/src/shims/timekeeping.c"),
    "linuxu_current_task":                       ("external", "linuxu/src/shims/task.c"),
    "list_lru_add":                              ("external", "linuxu/src/mm/list_lru.c"),
    "list_lru_count":                            ("external", "linuxu/src/mm/list_lru.c"),
    "list_lru_init":                             ("external", "linuxu/src/mm/list_lru.c"),
    "list_lru_isolate":                          ("external", "linuxu/src/mm/list_lru.c"),
    "list_lru_isolate_move":                     ("external", "linuxu/src/mm/list_lru.c"),
    "list_lru_walk":                             ("external", "linuxu/src/mm/list_lru.c"),
    "list_lru_walk_node":                        ("external", "linuxu/src/mm/list_lru.c"),
    "list_sort":                                 ("external", "third_party/linux/lib/list_sort.c"),
    "malloc":                                    ("external", "linuxu/src/shims/dext_alloc.c"),
    "mmap_read_lock":                            ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "mmap_read_unlock":                          ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "mmap_write_lock":                           ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "mmap_write_unlock":                         ("external", "linuxu/src/shims/kernel_api_stubs_incomplete.c"),
    "nanosleep":                                 ("external", "linuxu/src/shims/dext_time.c"),
    "ndelay":                                    ("external", "linuxu/src/delay.c"),
    "pci_rebar_bytes_to_size":                   ("external", "linuxu/src/pci/pci_stub.c"),
    "pci_resize_resource":                       ("external", "linuxu/src/pci/pci_stub.c"),
    "pcie_aspm_enabled":                         ("external", "linuxu/src/pci/pci_stub.c"),
    "pcie_bandwidth_available":                  ("external", "linuxu/src/pci/pci_stub.c"),
    "pcie_find_root_port":                       ("external", "linuxu/src/shims/kernel_api_stubs_manual.c"),
    "pcie_get_mps":                              ("external", "linuxu/src/pci/pci_stub.c"),
    "pcie_get_speed_cap":                        ("external", "linuxu/src/pci/pci_stub.c"),
    "pcie_get_width_cap":                        ("external", "linuxu/src/pci/pci_stub.c"),
    "platform_device_unregister":                ("external", "linuxu/src/shims/platform_device.c"),
    "pm_runtime_autosuspend_expiration":         ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_disable":                        ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_enable":                         ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_get_if_active":                  ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_get_noresume":                   ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_get_sync":                       ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_mark_last_busy":                 ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_put_autosuspend":                ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_resume":                         ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_resume_and_get":                 ("external", "linuxu/src/shims/platform_policy.c"),
    "pm_runtime_suspend":                        ("external", "linuxu/src/shims/platform_policy.c"),
    "pthread_cond_broadcast":                    ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_cond_destroy":                      ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_cond_init":                         ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_cond_signal":                       ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_cond_timedwait_relative_np":        ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_cond_wait":                         ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_create":                            ("external", "linuxu/src/shims/dext_threads.c"),
    "pthread_join":                              ("external", "linuxu/src/shims/dext_threads.c"),
    "pthread_mutex_destroy":                     ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_mutex_init":                        ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_mutex_lock":                        ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_mutex_trylock":                     ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_mutex_unlock":                      ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_once":                              ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_rwlock_init":                       ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_rwlock_rdlock":                     ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_rwlock_tryrdlock":                  ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_rwlock_trywrlock":                  ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_rwlock_unlock":                     ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_rwlock_wrlock":                     ("external", "linuxu/src/shims/dext_sync.c"),
    "pthread_self":                              ("external", "linuxu/src/shims/dext_threads.c"),
    "radix_tree_gang_lookup_tag":                ("external", "linuxu/src/xarray.c"),
    "radix_tree_insert":                         ("external", "linuxu/src/xarray.c"),
    "radix_tree_iter_delete":                    ("external", "linuxu/src/xarray.c"),
    "radix_tree_lookup":                         ("external", "linuxu/src/xarray.c"),
    "radix_tree_tag_clear":                      ("external", "linuxu/src/xarray.c"),
    "radix_tree_tag_set":                        ("external", "linuxu/src/xarray.c"),
    "radix_tree_tagged":                         ("external", "linuxu/src/xarray.c"),
    "rb_erase":                                  ("external", "third_party/linux/lib/rbtree.c"),
    "rb_first_postorder":                        ("external", "third_party/linux/lib/rbtree.c"),
    "rb_insert_color":                           ("external", "third_party/linux/lib/rbtree.c"),
    "rb_next":                                   ("external", "third_party/linux/lib/rbtree.c"),
    "rb_next_postorder":                         ("external", "third_party/linux/lib/rbtree.c"),
    "rb_prev":                                   ("external", "third_party/linux/lib/rbtree.c"),
    "register_chrdev":                           ("external", "linuxu/src/shims/chrdev.c"),
    "seq_buf_printf":                            ("external", "linuxu/src/shims/seq_buf.c"),
    "seq_hex_dump":                              ("external", "linuxu/src/shims/seq_file.c"),
    "seq_lseek":                                 ("external", "linuxu/src/shims/seq_file.c"),
    "seq_printf":                                ("external", "linuxu/src/shims/seq_file.c"),
    "seq_puts":                                  ("external", "linuxu/src/shims/seq_file.c"),
    "seq_read":                                  ("external", "linuxu/src/shims/seq_file.c"),
    "seq_release":                               ("external", "linuxu/src/shims/seq_file.c"),
    "seq_write":                                 ("external", "linuxu/src/shims/seq_file.c"),
    "seqcount_raw_read_begin":                   ("external", "linuxu/src/sync.c"),
    "seqcount_retry":                            ("external", "linuxu/src/sync.c"),
    "shmem_file_setup":                          ("external", "linuxu/src/mm/shmem.c"),
    "shmem_file_setup_with_mnt":                 ("external", "linuxu/src/mm/shmem.c"),
    "shmem_writeout":                            ("external", "linuxu/src/mm/shmem.c"),
    "shrinker_register":                         ("external", "linuxu/src/shims/shrinker.c"),
    "simple_pin_fs":                             ("external", "linuxu/src/shims/pseudo_fs.c"),
    "single_open":                               ("external", "linuxu/src/shims/seq_file.c"),
    "single_release":                            ("external", "linuxu/src/shims/seq_file.c"),
    "srcu_read_lock":                            ("external", "linuxu/src/rcu.c"),
    "srcu_read_unlock":                          ("external", "linuxu/src/rcu.c"),
    "sync_file_get_fence":                       ("external", "linuxu/src/sync.c"),
    "synchronize_srcu":                          ("external", "linuxu/src/rcu.c"),
    "sysfs_create_bin_file":                     ("external", "linuxu/src/shims/sysfs.c"),
    "sysfs_emit_at":                             ("external", "linuxu/src/shims/sysfs.c"),
    "sysfs_remove_bin_file":                     ("external", "linuxu/src/shims/sysfs.c"),
    "unregister_chrdev":                         ("external", "linuxu/src/shims/chrdev.c"),
    "vfprintf":                                  ("external", "linuxu/src/shims/dext_stdio.c"),
    "vfree":                                     ("external", "linuxu/src/kmem/vmalloc.c"),
    "vmalloc":                                   ("external", "linuxu/src/kmem/vmalloc.c"),
    "vmap":                                      ("external", "linuxu/src/kmem/vmalloc.c"),
    "vunmap":                                    ("external", "linuxu/src/kmem/vmalloc.c"),
    "wake_up_process":                           ("external", "linuxu/src/shims/kthread.c"),
    # Display Core, DRM KMS/display helpers and the hrtimer service; these
    # were unreachable stubs while CONFIG_DRM_AMD_DC was off.
    "drm_atomic_helper_check":                    ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_helper.c"),
    "drm_atomic_helper_check_plane_state":        ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_helper.c"),
    "drm_atomic_helper_commit":                   ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_helper.c"),
    "drm_atomic_helper_connector_destroy_state":  ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_connector_duplicate_state": ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_connector_reset":          ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_crtc_destroy_state":       ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_crtc_duplicate_state":     ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_crtc_reset":               ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_dirtyfb":                  ("external", "third_party/linux/drivers/gpu/drm/drm_damage_helper.c"),
    "drm_atomic_helper_disable_plane":            ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_helper.c"),
    "drm_atomic_helper_page_flip":                ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_helper.c"),
    "drm_atomic_helper_plane_destroy_state":      ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_plane_duplicate_state":    ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_plane_reset":              ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_state_helper.c"),
    "drm_atomic_helper_set_config":               ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_helper.c"),
    "drm_atomic_helper_shutdown":                 ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_helper.c"),
    "drm_atomic_helper_update_plane":             ("external", "third_party/linux/drivers/gpu/drm/drm_atomic_helper.c"),
    "drm_crtc_helper_set_config":                 ("external", "third_party/linux/drivers/gpu/drm/drm_crtc_helper.c"),
    "drm_crtc_helper_set_mode":                   ("external", "third_party/linux/drivers/gpu/drm/drm_crtc_helper.c"),
    "drm_crtc_init":                              ("external", "third_party/linux/drivers/gpu/drm/drm_modeset_helper.c"),
    "drm_dp_aux_init":                            ("external", "third_party/linux/drivers/gpu/drm/display/drm_dp_helper.c"),
    "drm_dp_aux_register":                        ("external", "third_party/linux/drivers/gpu/drm/display/drm_dp_helper.c"),
    "drm_dp_aux_unregister":                      ("external", "third_party/linux/drivers/gpu/drm/display/drm_dp_helper.c"),
    "drm_dp_dpcd_read":                           ("external", "third_party/linux/drivers/gpu/drm/display/drm_dp_helper.c"),
    "drm_dp_dpcd_read_link_status":               ("external", "third_party/linux/drivers/gpu/drm/display/drm_dp_helper.c"),
    "drm_dp_dpcd_write":                          ("external", "third_party/linux/drivers/gpu/drm/display/drm_dp_helper.c"),
    "drm_dp_set_subconnector_property":           ("external", "third_party/linux/drivers/gpu/drm/display/drm_dp_helper.c"),
    "drm_gem_fb_create_handle":                   ("external", "third_party/linux/drivers/gpu/drm/drm_gem_framebuffer_helper.c"),
    "drm_gem_fb_destroy":                         ("external", "third_party/linux/drivers/gpu/drm/drm_gem_framebuffer_helper.c"),
    "drm_gem_fb_get_obj":                         ("external", "third_party/linux/drivers/gpu/drm/drm_gem_framebuffer_helper.c"),
    "drm_helper_connector_dpms":                  ("external", "third_party/linux/drivers/gpu/drm/drm_crtc_helper.c"),
    "drm_helper_force_disable_all":               ("external", "third_party/linux/drivers/gpu/drm/drm_crtc_helper.c"),
    "drm_helper_hpd_irq_event":                   ("external", "third_party/linux/drivers/gpu/drm/drm_probe_helper.c"),
    "drm_helper_mode_fill_fb_struct":             ("external", "third_party/linux/drivers/gpu/drm/drm_modeset_helper.c"),
    "drm_kms_helper_hotplug_event":               ("external", "third_party/linux/drivers/gpu/drm/drm_probe_helper.c"),
    "drm_kms_helper_is_poll_worker":              ("external", "third_party/linux/drivers/gpu/drm/drm_probe_helper.c"),
    "drm_kms_helper_poll_disable":                ("external", "third_party/linux/drivers/gpu/drm/drm_probe_helper.c"),
    "drm_kms_helper_poll_enable":                 ("external", "third_party/linux/drivers/gpu/drm/drm_probe_helper.c"),
    "drm_kms_helper_poll_fini":                   ("external", "third_party/linux/drivers/gpu/drm/drm_probe_helper.c"),
    "drm_kms_helper_poll_init":                   ("external", "third_party/linux/drivers/gpu/drm/drm_probe_helper.c"),
    "hrtimer_active":                             ("external", "linuxu/src/timer.c"),
    "hrtimer_cancel":                             ("external", "linuxu/src/timer.c"),
    "hrtimer_cb_get_time":                        ("external", "linuxu/src/timer.c"),
    "hrtimer_forward":                            ("external", "linuxu/src/timer.c"),
    "hrtimer_setup":                              ("external", "linuxu/src/timer.c"),
    "hrtimer_start_range_ns":                     ("external", "linuxu/src/timer.c"),
    "hrtimer_try_to_cancel":                      ("external", "linuxu/src/timer.c"),
    "interval_tree_insert": {"cat": "external", "where": "linuxu/src/shims/interval_tree.c (INTERVAL_TREE_DEFINE)"},
    "interval_tree_iter_first": {"cat": "external", "where": "linuxu/src/shims/interval_tree.c (INTERVAL_TREE_DEFINE)"},
    "interval_tree_iter_next": {"cat": "external", "where": "linuxu/src/shims/interval_tree.c (INTERVAL_TREE_DEFINE)"},
    "interval_tree_remove": {"cat": "external", "where": "linuxu/src/shims/interval_tree.c (INTERVAL_TREE_DEFINE)"},
})
