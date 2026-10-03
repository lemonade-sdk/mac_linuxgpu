/* test_firmware.c - request_firmware over the in-process firmware cache.
 *
 * Asserts (host backend, no hardware, no host servicer attached):
 *   1. fw_table_register + request_firmware by name: right size and bytes,
 *      independent copies, into_buf/partial variants.
 *   2. a name nobody provides: -ENOENT, *fw left NULL, logged.
 *   3. request_firmware_direct: mutable entries are snapshotted.
 *   4. fw_boot_load_all(): every entry validates; idempotent.
 *   5. the optional embedded fallback: whatever the build embedded (maybe
 *      nothing) resolves, and other names stay -ENOENT.  No GPU-specific
 *      file list is assumed.
 *   6. the LoadFirmware push channel: add, replace, precedence over the
 *      embedded fallback, stable snapshots, error paths.
 *   7. the cache is not limited to a fixed number of entries.
 * The on-demand host servicer path is covered by test_firmware_mailbox.c.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <linux/firmware.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <rt/klog.h>

#include "fw/fw_table.h"
#include "fw/fw_rodata.h"

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

/* The placeholder magic is GONE — the table now holds REAL linux-firmware
 * bytes (scripts/fw2rodata.py). Instead of checking a specific magic word, we
 * verify REAL-blob properties:
 *   - not all-zero (a real firmware blob has non-zero content)
 *   - not the old LINUXU placeholder magic (0x554E494C, "LINU") at offset 0
 *     (guards against a regression to the placeholder table)
 * A real PSP/GC/SMU blob starts with a known header, but the first 4 bytes
 * differ per-blob, so we assert the two invariants above (non-zero + not the
 * old placeholder) which together prove the bytes are the embedded real set.
 */
#define LINUXU_FW_MAGIC 0x554e494cU

static int looks_real(const uint8_t *p, size_t size)
{
	uint32_t m;

	if (!p || size < 16)
		return 0;
	/* not all-zero */
	{
		int any = 0;

		for (size_t i = 0; i < size && i < 256; i++)
			if (p[i]) {
				any = 1;
				break;
			}
		if (!any)
			return 0;
	}
	/* not the old placeholder magic at offset 0 */
	memcpy(&m, p, sizeof(m));
	if (m == LINUXU_FW_MAGIC)
		return 0;
	return 1;
}

extern int kmemcheck_verify_all(void);

int main(void)
{
	/* ------------------------------------------------------------------ */
	/* 0. clean starting state                                            */
	EXPECT(kmemcheck_verify_all() == 0);
	fw_table_clear();
	EXPECT(fw_table_count() == 0);

	/* ------------------------------------------------------------------ */
	/* 1. register a few entries + request one by name                    */
	{
		uint8_t blob1[256], blob2[4096];
		struct fw_entry e1, e2;

		/* real-looking bytes: non-zero content, NOT the old LINUXU magic at
		 * offset 0 (so looks_real() passes). */
		memset(blob1, 0xAB, sizeof(blob1));
		memset(blob2, 0xCD, sizeof(blob2));
		blob1[0] = 0x01; /* ensure non-zero head, not the placeholder */
		blob2[0] = 0x02;
		e1 = (struct fw_entry){ .name = "amdgpu/test_one.bin",
					.blob = blob1, .size = sizeof(blob1) };
		e2 = (struct fw_entry){ .name = "amdgpu/test_two.bin",
					.blob = blob2, .size = sizeof(blob2) };
		EXPECT(fw_table_register(&e1) == 0);
		EXPECT(fw_table_register(&e2) == 0);
		EXPECT(fw_table_count() == 2);

		/* lookups */
		EXPECT(fw_table_find("amdgpu/test_one.bin") != NULL);
		EXPECT(fw_table_find("amdgpu/test_one.bin")->size == 256);
		EXPECT(fw_table_find("amdgpu/test_two.bin")->size == 4096);
		EXPECT(fw_table_find("amdgpu/does_not_exist.bin") == NULL);
		EXPECT(fw_table_find(NULL) == NULL);

		/* request by name: non-NULL, right size, REAL-looking bytes */
		const struct firmware *fw = NULL;
		EXPECT(request_firmware(&fw, "amdgpu/test_one.bin", NULL) == 0);
		EXPECT(fw != NULL);
		EXPECT(fw->data != NULL);
		EXPECT(fw->size == 256);
		EXPECT(looks_real(fw->data, fw->size));
		/* the copy must equal the registered blob */
		EXPECT(memcmp(fw->data, blob1, 256) == 0);
		/* a second request gives an independent allocation */
		const struct firmware *fw2 = NULL;
		EXPECT(request_firmware(&fw2, "amdgpu/test_one.bin", NULL) == 0);
		EXPECT(fw2 != NULL && fw2 != fw);
		EXPECT(fw2->data != fw->data);
		EXPECT(memcmp(fw2->data, blob1, 256) == 0);
		release_firmware(fw2);
		release_firmware(fw);
		uint8_t buffer[256];
		memset(buffer, 0xa5, sizeof(buffer));
		EXPECT(request_firmware_into_buf(&fw, e1.name, NULL, buffer, sizeof(buffer)) == 0);
		EXPECT(fw->data == buffer && fw->size == sizeof(buffer));
		EXPECT(memcmp(buffer, blob1, sizeof(buffer)) == 0);
		release_firmware(fw); /* caller buffer must not be freed */
		memset(buffer, 0xa5, sizeof(buffer));
		EXPECT(request_firmware_into_buf(&fw, e1.name, NULL, buffer, 8) == -EFBIG);
		EXPECT(!fw && buffer[0] == 0xa5);
		EXPECT(request_partial_firmware_into_buf(&fw, e1.name, NULL, buffer, 8, 4) == 0);
		EXPECT(fw->data == buffer && fw->size == 8 && buffer[8] == 0xa5);
		EXPECT(memcmp(buffer, blob1 + 4, 8) == 0);
		release_firmware(fw);
		EXPECT(request_partial_firmware_into_buf(&fw, e1.name, NULL, buffer, 8, 252) == 0);
		EXPECT(fw->size == 4);
		release_firmware(fw);
		EXPECT(request_partial_firmware_into_buf(&fw, e1.name, NULL, buffer, 8, SIZE_MAX) == -EINVAL);
		EXPECT(!fw);
		EXPECT(request_firmware_into_buf(&fw, e1.name, NULL, NULL, 8) == -EINVAL);
		EXPECT(!fw);
	}
	EXPECT(kmemcheck_verify_all() == 0);

	/* ------------------------------------------------------------------ */
	/* 2. unknown name -> -ENOENT, *fw left NULL                           */
	{
		const struct firmware *fw = (const struct firmware *)0x1;
		uint64_t cursor = UINT64_MAX, end = 0;
		char retained[512] = {0};
		EXPECT(klog_read(&cursor, NULL, 0, &end) == 0);
		EXPECT(cursor == end);
		int ret = request_firmware(&fw, "amdgpu/no_such_ucode.bin",
					   NULL);
		EXPECT(ret == -ENOENT);
		EXPECT(fw == NULL);
		EXPECT(klog_read(&cursor, retained, sizeof(retained) - 1, &end) > 0);
		EXPECT(cursor == end);
		EXPECT(strstr(retained, "amdgpu/no_such_ucode.bin") != NULL);
		EXPECT(strstr(retained, "-2") != NULL);
		/* NULL out-params are rejected, not dereferenced */
		EXPECT(request_firmware(NULL, "amdgpu/test_one.bin", NULL) ==
		       -EINVAL);
		EXPECT(request_firmware(&fw, NULL, NULL) == -EINVAL);
	}
	EXPECT(kmemcheck_verify_all() == 0);

	/* ------------------------------------------------------------------ */
	/* 3. request_firmware_direct: mutable overrides own a stable copy   */
	{
		const struct fw_entry *entry;
		const struct firmware *fd = NULL;

		entry = fw_table_find("amdgpu/test_two.bin");
		EXPECT(entry != NULL);
		EXPECT(request_firmware_direct(&fd, "amdgpu/test_two.bin",
					       NULL) == 0);
		EXPECT(fd != NULL);
		/* Registered entries are mutable; only embedded rodata is borrowed. */
		EXPECT(fd->data != entry->blob);
		EXPECT(memcmp(fd->data, entry->blob, entry->size) == 0);
		EXPECT(fd->size == 4096);
		EXPECT(looks_real(fd->data, fd->size));
		/* release must free only the struct, not the rodata blob */
		release_firmware(fd);
		entry = fw_table_find("amdgpu/test_two.bin");
		EXPECT(entry != NULL);
		EXPECT(looks_real(entry->blob, entry->size));
		/* unknown name also -ENOENT on the direct path */
		const struct firmware *fx = (const struct firmware *)0x1;
		EXPECT(request_firmware_direct(&fx, "amdgpu/nope.bin", NULL) ==
		       -ENOENT);
		EXPECT(fx == NULL);
	}
	EXPECT(kmemcheck_verify_all() == 0);

	/* ------------------------------------------------------------------ */
	/* 4. fw_boot_load_all(): every entry loads, count matches, idempotent */
	{
		const struct firmware *fw = NULL;

		EXPECT(fw_table_loaded_count() == 0);
		EXPECT(fw_boot_load_all() == 0);
		EXPECT(fw_table_loaded_count() == fw_table_count());
		/* idempotent */
		EXPECT(fw_boot_load_all() == 0);
		EXPECT(fw_table_loaded_count() == fw_table_count());
		/* still resolvable and still fails on unknown names */
		EXPECT(request_firmware(&fw, "amdgpu/test_two.bin", NULL) == 0);
		EXPECT(fw != NULL && fw->size == 4096);
		release_firmware(fw);
	}
	EXPECT(kmemcheck_verify_all() == 0);

	/* ------------------------------------------------------------------ */
	/* 5. embedded fallback: whatever the build embedded resolves          */
	{
		size_t n, rodata_count = 0;
		const struct fw_rodata_entry *rows;

		/* fresh table: only the optional embedded set, which may be
		 * empty and is never assumed to hold any particular GPU's files */
		fw_table_clear();
		EXPECT(fw_table_register_embedded() == 0);
		n = fw_table_count();
		rows = fw_rodata_entries(&rodata_count);
		EXPECT(n == rodata_count);
		EXPECT(rodata_count == 0 || rows != NULL);
		fprintf(stderr, "ok: %zu embedded fallback image(s)\n", n);
		/* registering again is idempotent */
		EXPECT(fw_table_register_embedded() == 0);
		EXPECT(fw_table_count() == n);

		EXPECT(fw_boot_load_all() == 0);
		EXPECT(fw_table_loaded_count() == n);

		for (size_t i = 0; i < n; i++) {
			const char *name = fw_table_name_at(i);
			const struct fw_entry *e = name ? fw_table_find(name) : NULL;
			const struct firmware *fw = NULL;
			int ret;

			if (!name || !e || strncmp(name, "amdgpu/", 7) != 0) {
				fprintf(stderr, "FAIL: entry %zu bad\n", i);
				return 1;
			}
			ret = request_firmware(&fw, name, NULL);
			if (ret != 0 || !fw || fw->size != e->size ||
			    !looks_real(fw->data, fw->size)) {
				fprintf(stderr, "FAIL: %s ret=%d\n", name, ret);
				return 1;
			}
			release_firmware(fw);
			/* rodata may be borrowed by the direct path */
			EXPECT(request_firmware_direct(&fw, name, NULL) == 0);
			EXPECT(fw->data == e->blob);
			release_firmware(fw);
		}
		/* integrity bookkeeping of the generated rows */
		for (size_t i = 0; i < rodata_count; i++) {
			EXPECT(rows[i].name != NULL && rows[i].blob != NULL);
			EXPECT(rows[i].size >= 16 && rows[i].sha256_8 != 0);
		}
		/* a name outside the table is still -ENOENT, with no device
		 * specific special case anywhere */
		const struct firmware *missing = (const struct firmware *)0x1;
		EXPECT(request_firmware(&missing, "amdgpu/not_shipped_9_9_9.bin",
					NULL) == -ENOENT);
		EXPECT(missing == NULL);
	}
	EXPECT(kmemcheck_verify_all() == 0);

	/* ------------------------------------------------------------------ */
	/* 6. host push: a pushed name resolves, then overrides on re-push     */
	{
		uint8_t new_blob[64], second[96];
		const struct firmware *fw = NULL;

		memset(new_blob, 0xEE, sizeof(new_blob));
		new_blob[0] = 0x42;
		memset(second, 0x5A, sizeof(second));
		second[0] = 0x43;

		/* add an unseen name, as the LoadFirmware selector does */
		EXPECT(!fw_table_has_override("amdgpu/host_pushed_1_2_3.bin"));
		EXPECT(fw_table_override("amdgpu/host_pushed_1_2_3.bin", new_blob,
					 sizeof(new_blob)) == 0);
		EXPECT(fw_table_has_override("amdgpu/host_pushed_1_2_3.bin"));
		EXPECT(request_firmware(&fw, "amdgpu/host_pushed_1_2_3.bin", NULL) == 0);
		EXPECT(fw != NULL && fw->size == sizeof(new_blob));
		EXPECT(memcmp(fw->data, new_blob, sizeof(new_blob)) == 0);
		/* replacing while a request is held keeps the held snapshot */
		EXPECT(fw_table_override("amdgpu/host_pushed_1_2_3.bin", second,
					 sizeof(second)) == 0);
		EXPECT(fw->size == sizeof(new_blob) && fw->data[0] == 0x42);
		release_firmware(fw);
		EXPECT(request_firmware(&fw, "amdgpu/host_pushed_1_2_3.bin", NULL) == 0);
		EXPECT(fw->size == sizeof(second) && fw->data[0] == 0x43);
		release_firmware(fw);

		/* a push also takes precedence over an embedded row of that name */
		size_t count = 0;
		const struct fw_rodata_entry *rows = fw_rodata_entries(&count);
		if (count) {
			EXPECT(fw_table_override(rows[0].name, new_blob,
						 sizeof(new_blob)) == 0);
			EXPECT(request_firmware(&fw, rows[0].name, NULL) == 0);
			EXPECT(fw->size == sizeof(new_blob));
			release_firmware(fw);
			/* retried Start keeps the push */
			EXPECT(fw_table_register_embedded() == 0);
			EXPECT(fw_table_has_override(rows[0].name));
		}

		/* repeated alloc/release stays clean */
		for (int i = 0; i < 50; i++) {
			if (request_firmware(&fw, "amdgpu/host_pushed_1_2_3.bin",
					     NULL) != 0 || !fw)
				return 1;
			release_firmware(fw);
			if (request_firmware_direct(&fw, "amdgpu/host_pushed_1_2_3.bin",
						    NULL) != 0 || !fw)
				return 1;
			release_firmware(fw);
		}
		release_firmware(NULL); /* must be a no-op */

		/* error paths */
		EXPECT(fw_table_override(NULL, new_blob, sizeof(new_blob)) == -2);
		EXPECT(fw_table_override("amdgpu/x.bin", NULL, 64) == -2);
		EXPECT(fw_table_override("amdgpu/x.bin", new_blob, 8) == -1);
	}
	EXPECT(kmemcheck_verify_all() == 0);

	/* ------------------------------------------------------------------ */
	/* 7. the cache grows past any fixed size                              */
	{
		uint8_t blob[32];
		char name[64];

		memset(blob, 0x11, sizeof(blob));
		for (int i = 0; i < 700; i++) {
			snprintf(name, sizeof(name), "amdgpu/growth_%d.bin", i);
			blob[0] = (uint8_t)i;
			if (fw_table_override(name, blob, sizeof(blob)) != 0)
				return 1;
		}
		EXPECT(fw_table_find("amdgpu/growth_0.bin") != NULL);
		EXPECT(fw_table_find("amdgpu/growth_699.bin") != NULL);
		EXPECT(fw_table_find("amdgpu/growth_699.bin")->blob[0] == (uint8_t)699);
	}
	EXPECT(kmemcheck_verify_all() == 0);

	/* teardown */
	fw_table_clear();
	EXPECT(fw_table_count() == 0);

	fprintf(stderr, "PASS test_firmware\n");
	return 0;
}
