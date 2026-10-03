/* linuxu shim: seq_buf formatting, following the pinned lib/seq_buf.c. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <linux/types.h>
#include <linux/bug.h>
#include <linux/seq_buf.h>

static bool seq_buf_can_fit(struct seq_buf *s, size_t len)
{
	return s->len + len <= s->size;
}

int seq_buf_vprintf(struct seq_buf *s, const char *fmt, va_list args)
{
	int len;

	WARN_ON(s->size == 0);

	if (s->len < s->size) {
		len = vsnprintf(s->buffer + s->len, s->size - s->len, fmt, args);
		if (len >= 0 && s->len + len < s->size) {
			s->len += len;
			return 0;
		}
	}
	seq_buf_set_overflow(s);
	return -1;
}

int seq_buf_printf(struct seq_buf *s, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = seq_buf_vprintf(s, fmt, ap);
	va_end(ap);

	return ret;
}

int seq_buf_puts(struct seq_buf *s, const char *str)
{
	size_t len = strlen(str);

	WARN_ON(s->size == 0);

	/* Add 1 to len for the trailing null byte which must be there */
	len += 1;

	if (seq_buf_can_fit(s, len)) {
		memcpy(s->buffer + s->len, str, len);
		/* Don't count the trailing null byte against the capacity */
		s->len += len - 1;
		return 0;
	}
	seq_buf_set_overflow(s);
	return -1;
}

int seq_buf_putc(struct seq_buf *s, unsigned char c)
{
	WARN_ON(s->size == 0);

	if (seq_buf_can_fit(s, 1)) {
		s->buffer[s->len++] = c;
		return 0;
	}
	seq_buf_set_overflow(s);
	return -1;
}
