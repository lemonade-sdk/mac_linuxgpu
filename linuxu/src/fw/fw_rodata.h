/* linuxu shim: fw - contract for the optional embedded firmware table.
 *
 * The table source is GENERATED at build time by scripts/fw2rodata.py into
 * the build directory (it is not part of the source tree).  It embeds
 * whatever firmware images the configured directory contains (see
 * EMBED_FIRMWARE / FW_EMBED_DIR in the Makefile), possibly none.  The names
 * are the strings upstream passes to request_firmware() ("amdgpu/<file>"),
 * and the bytes are the linux-firmware files byte for byte; provenance and
 * redistribution terms are in that directory's WHENCE and license files.
 */
#ifndef LINUXU_FW_RODATA_H
#define LINUXU_FW_RODATA_H

#include <stddef.h>
#include <stdint.h>

/* One embedded firmware blob (the generated file's row type). */
struct fw_rodata_entry {
	const char    *name;      /* the request name ("amdgpu/<file>") */
	const uint8_t *blob;      /* rodata bytes */
	size_t        size;       /* blob size in bytes */
	unsigned long sha256_8;   /* first 8 hex chars of the sha256 (integrity) */
};

/*
 * The embedded set. Returns the rodata table (NULL when empty) and, if
 * count != NULL, its length. The table is static rodata; the pointers are
 * stable for the process lifetime.
 */
const struct fw_rodata_entry *fw_rodata_entries(size_t *count);

#endif /* LINUXU_FW_RODATA_H */
