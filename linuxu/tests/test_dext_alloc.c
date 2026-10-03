#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *linuxu_dext_malloc(size_t);
void *linuxu_dext_calloc(size_t, size_t);
void *linuxu_dext_realloc(void *, size_t);
void *linuxu_dext_aligned_alloc(size_t, size_t);
char *linuxu_dext_strdup(const char *);
char *linuxu_dext_strndup(const char *, size_t);
void linuxu_dext_free(void *);

static size_t live_bytes;
static size_t allocations;
static int fail_allocation;

/* Deliberately return a misaligned raw address: IOMalloc does not promise
 * C malloc alignment. Also check the exact IOFree address and length. */
void *IOMalloc(size_t size)
{
	if (fail_allocation)
		return NULL;
	unsigned char *raw = malloc(size + sizeof(size_t) + 1);
	assert(raw);
	memcpy(raw, &size, sizeof(size));
	live_bytes += size;
	allocations++;
	return raw + sizeof(size_t) + 1;
}

void IOFree(void *pointer, size_t size)
{
	unsigned char *raw = (unsigned char *)pointer - sizeof(size_t) - 1;
	size_t expected;
	memcpy(&expected, raw, sizeof(expected));
	assert(expected == size);
	assert(live_bytes >= size && allocations);
	live_bytes -= size;
	allocations--;
	free(raw);
}

int main(void)
{
	unsigned char *p = linuxu_dext_malloc(7);
	assert(p && (uintptr_t)p % _Alignof(max_align_t) == 0);
	assert((uintptr_t)p % 16 == 0);
	memset(p, 0x5a, 7);
	p = linuxu_dext_realloc(p, 4096);
	assert(p);
	for (int i = 0; i < 7; i++) assert(p[i] == 0x5a);
	memset(p, 0xa5, 4096);
	p = linuxu_dext_realloc(p, 3);
	assert(p && p[0] == 0xa5 && p[2] == 0xa5);
	fail_allocation = 1;
	assert(linuxu_dext_realloc(p, 8192) == NULL);
	assert(p[0] == 0xa5 && p[2] == 0xa5);
	fail_allocation = 0;
	assert(linuxu_dext_realloc(p, 0) == NULL);

	p = linuxu_dext_calloc(17, 33);
	assert(p);
	for (int i = 0; i < 17 * 33; i++) assert(p[i] == 0);
	linuxu_dext_free(p);
	assert(linuxu_dext_calloc(SIZE_MAX, 2) == NULL);
	assert(linuxu_dext_malloc(SIZE_MAX) == NULL);
	assert(linuxu_dext_aligned_alloc(3, 9) == NULL);
	assert(linuxu_dext_aligned_alloc(4096, 17) == NULL);
	assert(linuxu_dext_aligned_alloc(0, 0) == NULL);
	for (size_t alignment = 1; alignment <= 65536; alignment *= 2) {
		p = linuxu_dext_aligned_alloc(alignment, alignment * 2);
		assert(p && (uintptr_t)p % alignment == 0);
		memset(p, 0xf1, alignment * 2);
		linuxu_dext_free(p);
	}
	/* More than the old 8 MiB permanent bump arena, with real reclamation. */
	for (int i = 0; i < 128; i++) {
		p = linuxu_dext_malloc(128 * 1024);
		assert(p);
		memset(p, i, 128 * 1024);
		linuxu_dext_free(p);
	}
	linuxu_dext_free(linuxu_dext_malloc(0));
	linuxu_dext_free(NULL);
	char *copy = linuxu_dext_strdup("owned name");
	assert(copy && strcmp(copy, "owned name") == 0);
	copy = linuxu_dext_realloc(copy, 64);
	assert(copy && strcmp(copy, "owned name") == 0);
	linuxu_dext_free(copy);
	const char bounded[] = {'a', 'b', 'c'};
	copy = linuxu_dext_strndup(bounded, sizeof(bounded));
	assert(copy && strcmp(copy, "abc") == 0);
	linuxu_dext_free(copy);
	copy = linuxu_dext_strndup(bounded, 0);
	assert(copy && copy[0] == '\0');
	linuxu_dext_free(copy);
	copy = linuxu_dext_strndup("abc", SIZE_MAX);
	assert(copy && strcmp(copy, "abc") == 0);
	linuxu_dext_free(copy);
	fail_allocation = 1;
	assert(linuxu_dext_strdup("name") == NULL);
	assert(linuxu_dext_strndup("name", 2) == NULL);
	fail_allocation = 0;
	assert(allocations == 0 && live_bytes == 0);
	puts("DriverKit heap: alignment, overflow, realloc, string ownership and reclamation passed");
}
