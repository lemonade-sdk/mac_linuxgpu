/* Run unchanged upstream RLC parsing and GFX12 firmware cleanup on the real
 * packaged images. Only the optional negative-control allocator is mocked;
 * the normal path uses the production shim heap. No hardware is accessed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include "amdgpu.h"

static int legacy_zero;
static unsigned int allocation_calls;
static size_t requested_size;

static void *rlc_test_kmalloc(size_t size, gfp_t flags)
{
    allocation_calls++;
    requested_size = size;
    if (legacy_zero && !size)
        return NULL;
    return kmalloc(size, flags);
}

#define kmalloc rlc_test_kmalloc
#include "upstream_rlc_init.inc"
#undef kmalloc
#include "upstream_rlc_cleanup.inc"

extern int kmemcheck_verify_all(void);

static void check_firmware(const char *path, int expect_failure)
{
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > (long)sizeof(struct rlc_firmware_header_v2_2));
    assert(!fseek(file, 0, SEEK_SET));
    struct firmware *fw = kzalloc(sizeof(*fw), GFP_KERNEL);
    assert(fw);
    fw->size = (size_t)length;
    fw->data = kmalloc(fw->size, GFP_KERNEL);
    assert(fw->data);
    assert(fread((void *)fw->data, 1, fw->size, file) == fw->size);
    assert(!fclose(file));

    const struct rlc_firmware_header_v2_0 *header = (const void *)fw->data;
    assert(le32_to_cpu(header->header.size_bytes) == fw->size);
    assert(le16_to_cpu(header->header.header_version_major) == 2);
    assert(le16_to_cpu(header->header.header_version_minor) == 2);
    assert(le32_to_cpu(header->reg_list_format_size_bytes) == 0);
    assert(le32_to_cpu(header->reg_list_size_bytes) == 0);

    struct amdgpu_device *adev = calloc(1, sizeof(*adev));
    struct device device = { .init_name = "offline-rlc" };
    assert(adev);
    adev->dev = &device;
    adev->gfx.rlc_fw = fw;
    adev->firmware.load_type = AMDGPU_FW_LOAD_PSP;
    allocation_calls = 0;
    requested_size = SIZE_MAX;
    int result = amdgpu_gfx_rlc_init_microcode_v2_0(adev);
    printf("%s: format=0 bytes list=0 bytes allocation=%zu result=%d\n",
           path, requested_size, result);
    assert(allocation_calls == 1 && requested_size == 0);
    assert(result == (expect_failure ? -ENOMEM : 0));
    if (expect_failure) {
        assert(!adev->gfx.rlc.register_list_format);
        assert(!adev->gfx.rlc.register_restore);
        assert(!adev->firmware.fw_size);
    } else {
        assert(adev->gfx.rlc.register_list_format);
        assert(adev->gfx.rlc.register_restore == adev->gfx.rlc.register_list_format);
        assert(adev->gfx.rlc_fw_version == le32_to_cpu(header->header.ucode_version));
        assert(adev->firmware.ucode[AMDGPU_UCODE_ID_RLC_G].fw == fw);
        assert(adev->firmware.fw_size ==
               ALIGN(le32_to_cpu(header->header.ucode_size_bytes), PAGE_SIZE));
    }

    /* Actual upstream GFX12 error/final cleanup, including the zero-size
     * register-list handle and firmware ownership release. */
    gfx_v12_0_free_microcode(adev);
    assert(!adev->gfx.rlc_fw);
    free(adev);
    assert(kmemcheck_verify_all() == 0);
}

int main(int argc, char **argv)
{
    int expect_failure = argc == 2;
    if (expect_failure) {
        legacy_zero = !strcmp(argv[1], "--legacy-zero");
        assert(legacy_zero || !strcmp(argv[1], "--expect-zero-failure"));
    } else {
        assert(argc == 1);
    }
    /* The images come from linux-firmware via scripts/fetch-firmware.sh;
     * the wrapper script skips the test when they are absent. */
    const char *dir = getenv("FIRMWARE_DIR");
    char path[4096];
    if (!dir || !*dir)
        dir = "build/firmware/amdgpu";
    snprintf(path, sizeof(path), "%s/gc_12_0_0_rlc.bin", dir);
    check_firmware(path, expect_failure);
    snprintf(path, sizeof(path), "%s/gc_12_0_1_rlc.bin", dir);
    check_firmware(path, expect_failure);
    puts(expect_failure ? "upstream RLC zero-allocation failure reproduced" :
                          "upstream RLC firmware initialization and cleanup passed");
    return 0;
}
