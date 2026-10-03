/* DriverKit has a log sink, not FILE-backed process streams. Host builds
 * use their native C library; never interpose empty libc implementations. */
#if defined(LINUXU_DEXT_DK) || defined(LINUXU_TEST_DEXT_STDIO)
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <rt/fatal.h>

extern int IOLogv(const char *format, va_list arguments);

#ifdef LINUXU_DEXT_DK
FILE *__stderrp;

int vfprintf(FILE *stream, const char *format, va_list arguments)
{
	(void)stream;
	return IOLogv(format, arguments);
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	int result = vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int fflush(FILE *stream)
{
	(void)stream;
	return 0; /* IOLogv has no local stdio buffer to flush. */
}

int fputc(int c, FILE *stream)
{
	(void)fprintf(stream, "%c", (unsigned char)c);
	return (unsigned char)c;
}
#endif

static int linuxu_vsprintf_chk(char *destination, unsigned long capacity,
			      const char *format, va_list arguments)
{
	int length = vsnprintf(destination, capacity, format, arguments);
	if (length >= 0 && (unsigned long)length >= capacity)
		LINUXU_FATAL("fortify: sprintf overflow");
	return length;
}

int linuxu_sprintf_chk(char *destination, int flag, unsigned long capacity,
		       const char *format, ...)
{
	va_list arguments;
	(void)flag;
	va_start(arguments, format);
	int length = linuxu_vsprintf_chk(destination, capacity, format, arguments);
	va_end(arguments);
	return length;
}

char *linuxu_strcpy_chk(char *destination, const char *source, unsigned long capacity)
{
	size_t length = strlen(source) + 1;
	if (length > capacity)
		LINUXU_FATAL("fortify: strcpy overflow");
	/* Avoid generating another fortified call inside its own implementation. */
	__builtin_memcpy(destination, source, length);
	return destination;
}

#ifdef LINUXU_DEXT_DK
int __sprintf_chk(char *destination, int flag, unsigned long capacity,
		  const char *format, ...)
{
	va_list arguments;
	(void)flag;
	va_start(arguments, format);
	int length = linuxu_vsprintf_chk(destination, capacity, format, arguments);
	va_end(arguments);
	return length;
}

char *__strcpy_chk(char *destination, const char *source, unsigned long capacity)
{
	return linuxu_strcpy_chk(destination, source, capacity);
}
#endif
#endif
