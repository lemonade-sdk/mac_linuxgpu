#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Exercise the DriverKit exports without replacing the test process's libc. */
#define LINUXU_DEXT_DK 1
#define fprintf linuxu_test_fprintf
#define vfprintf linuxu_test_vfprintf
#define fflush linuxu_test_fflush
#define fputc linuxu_test_fputc
#define __stderrp linuxu_test_stderr
#define __sprintf_chk linuxu_test_sprintf_chk
#define __strcpy_chk linuxu_test_strcpy_chk
#include "../src/shims/dext_stdio.c"

static char log_text[128];
int IOLogv(const char *format, va_list arguments)
{
	return vsnprintf(log_text, sizeof(log_text), format, arguments);
}

int main(void)
{
	char text[64];
	assert(linuxu_test_fprintf(NULL, "PCI %04x:%04x", 0x1002, 0x744c) == 13);
	assert(strcmp(log_text, "PCI 1002:744c") == 0);
	assert(linuxu_test_fputc('X', NULL) == 'X');
	assert(strcmp(log_text, "X") == 0);
	assert(linuxu_test_fflush(NULL) == 0);
	assert(linuxu_test_sprintf_chk(text, 0, sizeof(text), "%s%d", "renderD", 128) == 10);
	assert(strcmp(text, "renderD128") == 0);
	assert(linuxu_test_sprintf_chk(text, 0, 4, "%s", "abc") == 3);
	assert(strcmp(text, "abc") == 0);
	assert(linuxu_test_strcpy_chk(text, "", 1) == text && text[0] == '\0');
	assert(linuxu_test_strcpy_chk(text, "card0", 6) == text);
	assert(strcmp(text, "card0") == 0);
	return 0;
}
