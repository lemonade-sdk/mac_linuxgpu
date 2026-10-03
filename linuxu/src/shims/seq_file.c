/* linuxu shim: seq_file - synthetic record files for debugfs.
 *
 * Logic follows the pinned fs/seq_file.c. seq_read() runs the
 * seq_read_iter() algorithm against one flat destination buffer and copies
 * with copy_to_user(), so it fails with -EFAULT while linux/uaccess.h
 * rejects user copies; linuxu_seq_read_kernel() runs the same algorithm
 * into a kernel buffer for in-process readers. Buffers come from
 * kvmalloc(); there is no seq_file kmem_cache. hex_dump_to_buffer() follows lib/hexdump.c. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include <asm/unaligned.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#endif

/* fs/read_write.c: largest single read/write the VFS passes down. */
#define SEQ_MAX_RW_COUNT ((size_t)(INT_MAX & ~(PAGE_SIZE - 1)))

static const char seq_hex_asc[] = "0123456789abcdef";

static void seq_set_overflow(struct seq_file *m)
{
	m->count = m->size;
}

static void *seq_buf_alloc(unsigned long size)
{
	if (unlikely(size > SEQ_MAX_RW_COUNT))
		return NULL;

	return kvmalloc(size, GFP_KERNEL);
}

int seq_open(struct file *file, const struct seq_operations *op)
{
	struct seq_file *p;

	WARN_ON(file->private_data);

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	file->private_data = p;

	mutex_init(&p->lock);
	p->op = op;

	/* The lifetime of p is constrained to the lifetime of the file. */
	p->file = file;

	file->f_mode &= ~FMODE_PWRITE;
	return 0;
}

static int traverse(struct seq_file *m, loff_t offset)
{
	loff_t pos = 0;
	int error = 0;
	void *p;

	m->index = 0;
	m->count = m->from = 0;
	if (!offset)
		return 0;

	if (!m->buf) {
		m->buf = seq_buf_alloc(m->size = PAGE_SIZE);
		if (!m->buf)
			return -ENOMEM;
	}
	p = m->op->start(m, &m->index);
	while (p) {
		error = PTR_ERR(p);
		if (IS_ERR(p))
			break;
		error = m->op->show(m, p);
		if (error < 0)
			break;
		if (unlikely(error)) {
			error = 0;
			m->count = 0;
		}
		if (seq_has_overflowed(m))
			goto Eoverflow;
		p = m->op->next(m, p, &m->index);
		if (pos + m->count > offset) {
			m->from = offset - pos;
			m->count -= m->from;
			break;
		}
		pos += m->count;
		m->count = 0;
		if (pos == offset)
			break;
	}
	m->op->stop(m, p);
	return error;

Eoverflow:
	m->op->stop(m, p);
	kvfree(m->buf);
	m->count = 0;
	m->buf = seq_buf_alloc(m->size <<= 1);
	return !m->buf ? -ENOMEM : -EAGAIN;
}

/* Destination of one read: a Linux user buffer (copy_to_user) or, for
 * in-process consumers, a kernel buffer. */
struct seq_dest {
	char *base;
	bool kernel;
};

/* Copy up to @len buffered bytes to the destination at offset @at; returns
 * bytes copied (copy_to_iter() semantics). */
static size_t seq_copy_out(const struct seq_dest *dst, size_t at, size_t *room,
			   const char *src, size_t len)
{
	size_t n = len < *room ? len : *room;
	size_t left;

	if (!n)
		return 0;
	if (dst->kernel) {
		memcpy(dst->base + at, src, n);
		left = 0;
	} else {
		left = copy_to_user((char __user *)dst->base + at, src, n);
	}
	n -= left;
	*room -= n;
	return n;
}

static ssize_t seq_read_dest(struct file *file, const struct seq_dest *buf,
			     size_t size, loff_t *ppos);

ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
	struct seq_dest dst = { .base = (char *)buf, .kernel = false };

	return seq_read_dest(file, &dst, size, ppos);
}

ssize_t linuxu_seq_read_kernel(struct file *file, char *buf, size_t size,
			       loff_t *ppos)
{
	struct seq_dest dst = { .base = buf, .kernel = true };

	return seq_read_dest(file, &dst, size, ppos);
}

static ssize_t seq_read_dest(struct file *file, const struct seq_dest *buf,
			     size_t size, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	size_t room = size;
	size_t copied = 0;
	size_t n;
	ssize_t ret;
	void *p;
	int err = 0;

	if (!size)
		return 0;

	mutex_lock(&m->lock);

	/* A read from offset zero restarts the iterator at the first record. */
	if (*ppos == 0) {
		m->index = 0;
		m->count = 0;
	}

	/* Do not assume *ppos is where the previous read left it. */
	if (unlikely(*ppos != m->read_pos)) {
		while ((err = traverse(m, *ppos)) == -EAGAIN)
			;
		if (err) {
			m->read_pos = 0;
			m->index = 0;
			m->count = 0;
			goto Done;
		} else {
			m->read_pos = *ppos;
		}
	}

	if (!m->buf) {
		m->buf = seq_buf_alloc(m->size = PAGE_SIZE);
		if (!m->buf)
			goto Enomem;
	}
	/* Something left in the buffer: copy it out first. */
	if (m->count) {
		n = seq_copy_out(buf, copied, &room, m->buf + m->from, m->count);
		m->count -= n;
		m->from += n;
		copied += n;
		if (m->count)
			goto Done;
	}
	/* Get a non-empty record in the buffer. */
	m->from = 0;
	p = m->op->start(m, &m->index);
	while (1) {
		err = PTR_ERR(p);
		if (!p || IS_ERR(p))
			break;
		err = m->op->show(m, p);
		if (err < 0)
			break;
		if (unlikely(err))
			m->count = 0;
		if (unlikely(!m->count)) {
			p = m->op->next(m, p, &m->index);
			continue;
		}
		if (!seq_has_overflowed(m))
			goto Fill;
		/* Need a bigger buffer. */
		m->op->stop(m, p);
		kvfree(m->buf);
		m->count = 0;
		m->buf = seq_buf_alloc(m->size <<= 1);
		if (!m->buf)
			goto Enomem;
		p = m->op->start(m, &m->index);
	}
	/* EOF or an error. */
	m->op->stop(m, p);
	m->count = 0;
	goto Done;
Fill:
	/* One non-empty record is buffered; fit more if the reader wants
	 * more, advancing the iterator once for every record shown. */
	while (1) {
		size_t offs = m->count;
		loff_t pos = m->index;

		p = m->op->next(m, p, &m->index);
		if (pos == m->index) {
			pr_info("seq_file: buggy .next function did not update position index\n");
			m->index++;
		}
		if (!p || IS_ERR(p))
			break;
		if (m->count >= room)
			break;
		err = m->op->show(m, p);
		if (err > 0) {
			m->count = offs;
		} else if (err || seq_has_overflowed(m)) {
			m->count = offs;
			break;
		}
	}
	m->op->stop(m, p);
	n = seq_copy_out(buf, copied, &room, m->buf, m->count);
	copied += n;
	m->count -= n;
	m->from = n;
Done:
	if (unlikely(!copied)) {
		ret = m->count ? -EFAULT : err;
	} else {
		*ppos += copied;
		m->read_pos += copied;
		ret = copied;
	}
	mutex_unlock(&m->lock);
	return ret;
Enomem:
	err = -ENOMEM;
	goto Done;
}

loff_t seq_lseek(struct file *file, loff_t offset, int whence)
{
	struct seq_file *m = file->private_data;
	loff_t retval = -EINVAL;

	mutex_lock(&m->lock);
	switch (whence) {
	case SEEK_CUR:
		offset += file->f_pos;
		/* fallthrough */
	case SEEK_SET:
		if (offset < 0)
			break;
		retval = offset;
		if (offset != m->read_pos) {
			while ((retval = traverse(m, offset)) == -EAGAIN)
				;
			if (retval) {
				file->f_pos = 0;
				m->read_pos = 0;
				m->index = 0;
				m->count = 0;
			} else {
				m->read_pos = offset;
				retval = file->f_pos = offset;
			}
		} else {
			file->f_pos = offset;
		}
	}
	mutex_unlock(&m->lock);
	return retval;
}

int seq_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;

	(void)inode;
	kvfree(m->buf);
	mutex_destroy(&m->lock);
	kfree(m);
	file->private_data = NULL;
	return 0;
}

void seq_vprintf(struct seq_file *m, const char *f, va_list args)
{
	int len;

	if (m->count < m->size) {
		len = vsnprintf(m->buf + m->count, m->size - m->count, f, args);
		if (len >= 0 && m->count + len < m->size) {
			m->count += len;
			return;
		}
	}
	seq_set_overflow(m);
}

void seq_printf(struct seq_file *m, const char *f, ...)
{
	va_list args;

	va_start(args, f);
	seq_vprintf(m, f, args);
	va_end(args);
}

void *single_start(struct seq_file *p, loff_t *pos)
{
	(void)p;
	return *pos ? NULL : SEQ_START_TOKEN;
}

static void *single_next(struct seq_file *p, void *v, loff_t *pos)
{
	(void)p; (void)v;
	++*pos;
	return NULL;
}

static void single_stop(struct seq_file *p, void *v)
{
	(void)p; (void)v;
}

int single_open(struct file *file, int (*show)(struct seq_file *, void *),
		void *data)
{
	struct seq_operations *op = kmalloc(sizeof(*op), GFP_KERNEL);
	int res = -ENOMEM;

	if (op) {
		op->start = single_start;
		op->next = single_next;
		op->stop = single_stop;
		op->show = show;
		res = seq_open(file, op);
		if (!res)
			((struct seq_file *)file->private_data)->private = data;
		else
			kfree(op);
	}
	return res;
}

int single_open_size(struct file *file, int (*show)(struct seq_file *, void *),
		     void *data, size_t size)
{
	char *buf = seq_buf_alloc(size);
	int ret;

	if (!buf)
		return -ENOMEM;
	ret = single_open(file, show, data);
	if (ret) {
		kvfree(buf);
		return ret;
	}
	((struct seq_file *)file->private_data)->buf = buf;
	((struct seq_file *)file->private_data)->size = size;
	return 0;
}

int single_release(struct inode *inode, struct file *file)
{
	const struct seq_operations *op =
		((struct seq_file *)file->private_data)->op;
	int res = seq_release(inode, file);

	kfree((void *)op);
	return res;
}

int seq_release_private(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	kfree(seq->private);
	seq->private = NULL;
	return seq_release(inode, file);
}

void *__seq_open_private(struct file *f, const struct seq_operations *ops,
			 int psize)
{
	int rc;
	void *private;
	struct seq_file *seq;

	private = kzalloc(psize, GFP_KERNEL);
	if (private == NULL)
		goto out;

	rc = seq_open(f, ops);
	if (rc < 0)
		goto out_free;

	seq = f->private_data;
	seq->private = private;
	return private;

out_free:
	kfree(private);
out:
	return NULL;
}

int seq_open_private(struct file *filp, const struct seq_operations *ops,
		     int psize)
{
	return __seq_open_private(filp, ops, psize) ? 0 : -ENOMEM;
}

void seq_putc(struct seq_file *m, char c)
{
	if (m->count >= m->size)
		return;

	m->buf[m->count++] = c;
}

void __seq_puts(struct seq_file *m, const char *s)
{
	seq_write(m, s, strlen(s));
}

void seq_puts(struct seq_file *m, const char *s)
{
	if (s[0] && !s[1])
		seq_putc(m, s[0]);
	else
		__seq_puts(m, s);
}

/* lib/vsprintf.c num_to_str(): decimal digits right-aligned to @width with
 * leading spaces; 0 when @size is too small. */
static int seq_num_to_str(char *buf, int size, unsigned long long num,
			  unsigned int width)
{
	char tmp[24];
	int idx = 0, len, i;

	do {
		tmp[idx++] = (char)('0' + num % 10);
		num /= 10;
	} while (num);
	len = idx > (int)width ? idx : (int)width;
	if (len > size)
		return 0;
	for (i = 0; i < len - idx; i++)
		buf[i] = ' ';
	for (; i < len; i++)
		buf[i] = tmp[--idx];
	return len;
}

void seq_put_decimal_ull_width(struct seq_file *m, const char *delimiter,
			       unsigned long long num, unsigned int width)
{
	int len;

	if (m->count + 2 >= m->size) /* we'll write 2 bytes at least */
		goto overflow;

	if (delimiter && delimiter[0]) {
		if (delimiter[1] == 0)
			seq_putc(m, delimiter[0]);
		else
			seq_puts(m, delimiter);
	}

	if (!width)
		width = 1;

	if (m->count + width >= m->size)
		goto overflow;

	len = seq_num_to_str(m->buf + m->count, m->size - m->count, num, width);
	if (!len)
		goto overflow;

	m->count += len;
	return;

overflow:
	seq_set_overflow(m);
}

void seq_put_decimal_ull(struct seq_file *m, const char *delimiter,
			 unsigned long long num)
{
	seq_put_decimal_ull_width(m, delimiter, num, 0);
}

void seq_put_hex_ll(struct seq_file *m, const char *delimiter,
		    unsigned long long v, unsigned int width)
{
	unsigned int len;
	int i;

	if (delimiter && delimiter[0]) {
		if (delimiter[1] == 0)
			seq_putc(m, delimiter[0]);
		else
			seq_puts(m, delimiter);
	}

	if (v == 0)
		len = 1;
	else
		len = (sizeof(v) * 8 - __builtin_clzll(v) + 3) / 4;

	if (len < width)
		len = width;

	if (m->count + len > m->size) {
		seq_set_overflow(m);
		return;
	}

	for (i = len - 1; i >= 0; i--) {
		m->buf[m->count + i] = seq_hex_asc[0xf & v];
		v = v >> 4;
	}
	m->count += len;
}

void seq_put_decimal_ll(struct seq_file *m, const char *delimiter, long long num)
{
	int len;

	if (m->count + 3 >= m->size) /* we'll write 2 bytes at least */
		goto overflow;

	if (delimiter && delimiter[0]) {
		if (delimiter[1] == 0)
			seq_putc(m, delimiter[0]);
		else
			seq_puts(m, delimiter);
	}

	if (m->count + 2 >= m->size)
		goto overflow;

	if (num < 0) {
		m->buf[m->count++] = '-';
		num = -num;
	}

	if (num < 10) {
		m->buf[m->count++] = num + '0';
		return;
	}

	len = seq_num_to_str(m->buf + m->count, m->size - m->count, num, 0);
	if (!len)
		goto overflow;

	m->count += len;
	return;

overflow:
	seq_set_overflow(m);
}

int seq_write(struct seq_file *seq, const void *data, size_t len)
{
	if (seq->count + len < seq->size) {
		memcpy(seq->buf + seq->count, data, len);
		seq->count += len;
		return 0;
	}
	seq_set_overflow(seq);
	return -1;
}

void seq_pad(struct seq_file *m, char c)
{
	int size = m->pad_until - m->count;

	if (size > 0) {
		if (size + m->count > m->size) {
			seq_set_overflow(m);
			return;
		}
		memset(m->buf + m->count, ' ', size);
		m->count += size;
	}
	if (c)
		seq_putc(m, c);
}

int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize,
		       char *linebuf, size_t linebuflen, bool ascii)
{
	const u8 *ptr = buf;
	int ngroups;
	u8 ch;
	int j, lx = 0;
	int ascii_column;
	int ret;

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;

	if (len > (size_t)rowsize)	/* limit to one line at a time */
		len = rowsize;
	if (groupsize <= 0 || (groupsize & (groupsize - 1)) || groupsize > 8)
		groupsize = 1;
	if ((len % groupsize) != 0)	/* no mixed size output */
		groupsize = 1;

	ngroups = len / groupsize;
	ascii_column = rowsize * 2 + rowsize / groupsize + 1;

	if (!linebuflen)
		goto overflow1;

	if (!len)
		goto nil;

	if (groupsize == 8) {
		const u64 *ptr8 = buf;

		for (j = 0; j < ngroups; j++) {
			ret = snprintf(linebuf + lx, linebuflen - lx,
				       "%s%16.16llx", j ? " " : "",
				       (unsigned long long)get_unaligned(ptr8 + j));
			if (ret >= (int)(linebuflen - lx))
				goto overflow1;
			lx += ret;
		}
	} else if (groupsize == 4) {
		const u32 *ptr4 = buf;

		for (j = 0; j < ngroups; j++) {
			ret = snprintf(linebuf + lx, linebuflen - lx,
				       "%s%8.8x", j ? " " : "",
				       (unsigned int)get_unaligned(ptr4 + j));
			if (ret >= (int)(linebuflen - lx))
				goto overflow1;
			lx += ret;
		}
	} else if (groupsize == 2) {
		const u16 *ptr2 = buf;

		for (j = 0; j < ngroups; j++) {
			ret = snprintf(linebuf + lx, linebuflen - lx,
				       "%s%4.4x", j ? " " : "",
				       (unsigned int)get_unaligned(ptr2 + j));
			if (ret >= (int)(linebuflen - lx))
				goto overflow1;
			lx += ret;
		}
	} else {
		for (j = 0; j < (int)len; j++) {
			if (linebuflen < (size_t)lx + 2)
				goto overflow2;
			ch = ptr[j];
			linebuf[lx++] = seq_hex_asc[(ch & 0xf0) >> 4];
			if (linebuflen < (size_t)lx + 2)
				goto overflow2;
			linebuf[lx++] = seq_hex_asc[ch & 0x0f];
			if (linebuflen < (size_t)lx + 2)
				goto overflow2;
			linebuf[lx++] = ' ';
		}
		if (j)
			lx--;
	}
	if (!ascii)
		goto nil;

	while (lx < ascii_column) {
		if (linebuflen < (size_t)lx + 2)
			goto overflow2;
		linebuf[lx++] = ' ';
	}
	for (j = 0; j < (int)len; j++) {
		if (linebuflen < (size_t)lx + 2)
			goto overflow2;
		ch = ptr[j];
		linebuf[lx++] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
	}
nil:
	linebuf[lx] = '\0';
	return lx;
overflow2:
	linebuf[lx++] = '\0';
overflow1:
	return ascii ? ascii_column + len : (groupsize * 2 + 1) * ngroups - 1;
}

/* A complete analogue of print_hex_dump(). */
void seq_hex_dump(struct seq_file *m, const char *prefix_str, int prefix_type,
		  int rowsize, int groupsize, const void *buf, size_t len,
		  bool ascii)
{
	const u8 *ptr = buf;
	int i, linelen, remaining = len;
	char *buffer;
	size_t size;
	int ret;

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;

	for (i = 0; (size_t)i < len && !seq_has_overflowed(m); i += rowsize) {
		linelen = remaining < rowsize ? remaining : rowsize;
		remaining -= rowsize;

		switch (prefix_type) {
		case DUMP_PREFIX_ADDRESS:
			seq_printf(m, "%s%p: ", prefix_str, ptr + i);
			break;
		case DUMP_PREFIX_OFFSET:
			seq_printf(m, "%s%.8x: ", prefix_str, i);
			break;
		default:
			seq_printf(m, "%s", prefix_str);
			break;
		}

		size = seq_get_buf(m, &buffer);
		ret = hex_dump_to_buffer(ptr + i, linelen, rowsize, groupsize,
					 buffer, size, ascii);
		seq_commit(m, (size_t)ret < size ? ret : -1);

		seq_putc(m, '\n');
	}
}

struct list_head *seq_list_start(struct list_head *head, loff_t pos)
{
	struct list_head *lh;

	list_for_each(lh, head)
		if (pos-- == 0)
			return lh;

	return NULL;
}

struct list_head *seq_list_start_head(struct list_head *head, loff_t pos)
{
	if (!pos)
		return head;

	return seq_list_start(head, pos - 1);
}

struct list_head *seq_list_next(void *v, struct list_head *head, loff_t *ppos)
{
	struct list_head *lh;

	lh = ((struct list_head *)v)->next;
	++*ppos;
	return lh == head ? NULL : lh;
}
