/* DriverKit heap adapter. IOMalloc/IOFree require the original allocation
 * address and size; retain both ahead of the aligned C allocation. */
#if defined(LINUXU_DEXT_DK) || defined(LINUXU_TEST_DEXT_ALLOC)
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Public DriverKit IOLib C entry points. Tests provide a checked backend. */
extern void *IOMalloc(size_t length);
extern void IOFree(void *address, size_t length);

struct dext_heap_header {
	void *allocation;
	size_t allocation_size;
	size_t payload_size;
};

/* Match Darwin malloc's minimum alignment, including 16-byte SIMD values.
 * Darwin's max_align_t alone can be only 8-byte aligned on arm64. */
#define DEXT_HEAP_ALIGNMENT (_Alignof(max_align_t) < 16 ? 16 : _Alignof(max_align_t))

static void *heap_allocate(size_t size, size_t alignment)
{
	const size_t overhead = sizeof(struct dext_heap_header);
	size_t payload = size ? size : 1;
	if (!alignment || (alignment & (alignment - 1)) ||
	    alignment < _Alignof(struct dext_heap_header) ||
	    alignment - 1 > SIZE_MAX - overhead ||
	    payload > SIZE_MAX - overhead - (alignment - 1))
		return NULL;
	size_t total = payload + overhead + alignment - 1;
	void *raw = IOMalloc(total);
	if (!raw)
		return NULL;
	uintptr_t address = ((uintptr_t)raw + overhead + alignment - 1) &
		~(uintptr_t)(alignment - 1);
	struct dext_heap_header *header = (struct dext_heap_header *)address - 1;
	header->allocation = raw;
	header->allocation_size = total;
	header->payload_size = size;
	return (void *)address;
}

void *linuxu_dext_malloc(size_t size)
{
	return heap_allocate(size, DEXT_HEAP_ALIGNMENT);
}

void linuxu_dext_free(void *pointer)
{
	if (pointer) {
		struct dext_heap_header *header = (struct dext_heap_header *)pointer - 1;
		IOFree(header->allocation, header->allocation_size);
	}
}

void *linuxu_dext_calloc(size_t count, size_t size)
{
	if (count && size > SIZE_MAX / count)
		return NULL;
	void *pointer = linuxu_dext_malloc(count * size);
	if (pointer)
		memset(pointer, 0, count * size);
	return pointer;
}

void *linuxu_dext_realloc(void *pointer, size_t size)
{
	if (!pointer)
		return linuxu_dext_malloc(size);
	if (!size) {
		linuxu_dext_free(pointer);
		return NULL;
	}
	struct dext_heap_header *header = (struct dext_heap_header *)pointer - 1;
	void *replacement = linuxu_dext_malloc(size);
	if (!replacement)
		return NULL;
	size_t copy_size = header->payload_size < size ? header->payload_size : size;
	memcpy(replacement, pointer, copy_size);
	linuxu_dext_free(pointer);
	return replacement;
}

void *linuxu_dext_aligned_alloc(size_t alignment, size_t size)
{
	if (!alignment || (alignment & (alignment - 1)) || size % alignment)
		return NULL;
	/* Small requested alignments are also satisfied by ordinary alignment. */
	if (alignment < DEXT_HEAP_ALIGNMENT)
		alignment = DEXT_HEAP_ALIGNMENT;
	return heap_allocate(size, alignment);
}

/* DriverKit's libc string allocators do not call this image's malloc.
 * Keep their results in the same allocation family as our free/realloc. */
char *linuxu_dext_strndup(const char *source, size_t limit)
{
	size_t length = 0;
	while (length < limit && source[length])
		length++;
	if (length == SIZE_MAX)
		return NULL;
	char *copy = linuxu_dext_malloc(length + 1);
	if (copy) {
		memcpy(copy, source, length);
		copy[length] = '\0';
	}
	return copy;
}

char *linuxu_dext_strdup(const char *source)
{
	return linuxu_dext_strndup(source, SIZE_MAX);
}

#ifdef LINUXU_DEXT_DK
void *malloc(size_t size) { return linuxu_dext_malloc(size); }
void *calloc(size_t count, size_t size) { return linuxu_dext_calloc(count, size); }
void *realloc(void *pointer, size_t size) { return linuxu_dext_realloc(pointer, size); }
void *aligned_alloc(size_t alignment, size_t size)
{
	return linuxu_dext_aligned_alloc(alignment, size);
}
void free(void *pointer) { linuxu_dext_free(pointer); }
char *strdup(const char *source) { return linuxu_dext_strdup(source); }
char *strndup(const char *source, size_t limit)
{
	return linuxu_dext_strndup(source, limit);
}
#endif
#endif
