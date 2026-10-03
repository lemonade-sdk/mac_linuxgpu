/* Exercise string helpers with exact-size inputs and GPU ring word counts. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/types.h>
#include <linux/string.h>
extern int scnprintf(char *, size_t, const char *, ...);

int main(void)
{
	char *source = malloc(4), output[8];
	assert(source); memset(source, 'x', 4); memset(output, 'z', sizeof(output));
	assert(sized_strscpy(output, source, 4) == -E2BIG);
	assert(!memcmp(output, "xxx\0", 4) && output[4] == 'z');
	assert(sized_strscpy(NULL, NULL, 0) == -E2BIG);
	assert(sized_strscpy_pad(output, source, 4) == -E2BIG);
	free(source);
	memset(output, 'z', sizeof(output));
	assert(strscpy_pad(output, "a", sizeof(output)) == 1);
	assert(output[0] == 'a');
	for (unsigned int i = 1; i < sizeof(output); i++) assert(output[i] == 0);
	assert(scnprintf(output, 4, "%s", "abcdef") == 3);
	assert(!strcmp(output, "abc"));
	assert(scnprintf(NULL, 0, "%s", "abcdef") == 0);
	/* BIOS/debug formatting loops advance by bytes actually stored. */
	size_t offset = scnprintf(output, sizeof(output), "%s", "abcdef");
	offset += scnprintf(output + offset, sizeof(output) - offset, "%s", "ghijkl");
	assert(offset == sizeof(output) - 1);
	assert(scnprintf(output + offset, sizeof(output) - offset, "%s", "more") == 0);
	char chars[] = "abc";
	assert(strnchr(chars, sizeof(chars), 0) == chars + 3);
	assert(!strnchr(chars, 2, 'c'));
	strtomem_pad(output, "ab", 0);
	assert(output[0] == 'a' && output[1] == 'b');
	for (unsigned int i = 2; i < sizeof(output); i++) assert(!output[i]);
	struct { uint32_t before, ring[17], after; } words = {.before = 0x1234, .after = 0x5678};
	assert(memset32(words.ring, 0xffff1000, 17) == words.ring);
	for (unsigned int i = 0; i < 17; i++) assert(words.ring[i] == 0xffff1000);
	assert(words.before == 0x1234 && words.after == 0x5678);
	memset32(words.ring + 13, 0x80000000, 4);
	for (unsigned int i = 13; i < 17; i++) assert(words.ring[i] == 0x80000000);
	uint64_t wide[4] = {1, 0, 0, 2};
	assert(memset64(wide + 1, 0xabcdef12345678UL, 2) == wide + 1);
	assert(wide[0] == 1 && wide[1] == 0xabcdef12345678UL && wide[2] == wide[1] && wide[3] == 2);
	puts("string contracts: bounded reads, NUL padding, snprintf lengths and full GPU ring word initialization passed");
}
