/* linuxu shim: printk — printk/pr_ family to fprintf(stderr) with
 * kernel-style prefix; global debug-level gate
 * (MAPPING → os_log ring in the dext; stderr
 * for the host build). */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/printk.h>
#include <linux/kern_levels.h>
#include <linux/kernel.h>
#include <rt/klog.h>
#include <rt/fatal.h>

#ifdef LINUXU_DEXT_DK
/* Public DriverKit logging entry point; no stdio stream exists in a dext. */
extern int IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif

/* ---- debug level gate (0=emerg .. 7=debug) ---- */
static pthread_mutex_t klog_lock = PTHREAD_MUTEX_INITIALIZER;
static int klog_level = 6; /* show info and above by default */
static int klog_debug = 0; /* dynamic_debug gate: 0 = drop dev_dbg */

/* Formatting and the platform sink run outside this allocation-free lock.
 * A reader holds it only while copying at most the fixed ring capacity. */
static unsigned char klog_ring_lock;
static char klog_ring[LINUXU_KLOG_CAPACITY];
static uint64_t klog_ring_end;

static void klog_ring_acquire(void)
{
	while (__atomic_test_and_set(&klog_ring_lock, __ATOMIC_ACQUIRE))
		while (__atomic_load_n(&klog_ring_lock, __ATOMIC_RELAXED))
			;
}

static void klog_ring_release(void)
{
	__atomic_clear(&klog_ring_lock, __ATOMIC_RELEASE);
}

static void klog_append(const char *text, size_t length)
{
	klog_ring_acquire();
	/* Keep cursors monotonic even at the theoretical 64-bit sequence limit. */
	if (length > UINT64_MAX - klog_ring_end)
		length = (size_t)(UINT64_MAX - klog_ring_end);
	size_t offset = klog_ring_end % LINUXU_KLOG_CAPACITY;
	size_t first = LINUXU_KLOG_CAPACITY - offset;
	if (first > length) first = length;
	memcpy(klog_ring + offset, text, first);
	memcpy(klog_ring, text + first, length - first);
	klog_ring_end += length;
	klog_ring_release();
}

void klog_write(const char *text, size_t length)
{
	if (!text || !length) return;
	if (length > LINUXU_KLOG_CAPACITY) {
		text += length - LINUXU_KLOG_CAPACITY;
		length = LINUXU_KLOG_CAPACITY;
	}
	klog_append(text, length);
}

size_t klog_read(uint64_t *cursor, char *out, size_t capacity, uint64_t *end)
{
	if (!cursor || (!out && capacity)) return 0;
	klog_ring_acquire();
	uint64_t oldest = klog_ring_end > LINUXU_KLOG_CAPACITY ?
		klog_ring_end - LINUXU_KLOG_CAPACITY : 0;
	uint64_t position = *cursor;
	if (position < oldest) position = oldest;
	if (position > klog_ring_end) position = klog_ring_end;
	size_t length = (size_t)(klog_ring_end - position);
	if (length > capacity) length = capacity;
	if (length) {
		size_t offset = position % LINUXU_KLOG_CAPACITY;
		size_t first = LINUXU_KLOG_CAPACITY - offset;
		if (first > length) first = length;
		memcpy(out, klog_ring + offset, first);
		memcpy(out + first, klog_ring, length - first);
	}
	*cursor = position + length;
	if (end) *end = klog_ring_end;
	klog_ring_release();
	return length;
}

void klog_set_level(int level)
{
	__atomic_store_n(&klog_level, level, __ATOMIC_RELAXED);
}

int klog_get_level(void)
{
	return __atomic_load_n(&klog_level, __ATOMIC_RELAXED);
}

void klog_set_debug(int on)
{
	__atomic_store_n(&klog_debug, on, __ATOMIC_RELAXED);
}

int klog_get_debug(void)
{
	return __atomic_load_n(&klog_debug, __ATOMIC_RELAXED);
}

static const char *level_tag(int level)
{
	switch (level) {
	case 0: return "<0>";
	case 1: return "<1>";
	case 2: return "<2>";
	case 3: return "<3>";
	case 4: return "<4>";
	case 5: return "<5>";
	case 6: return "<6>";
	default: return "<7>";
	}
}

static int klog_vemit(int level, const char *f, va_list ap)
{
	if (level > klog_get_level())
		return 0;
	char record[LINUXU_KLOG_MESSAGE_CAPACITY];
	memcpy(record, level_tag(level), 3);
	record[3] = ' ';
	va_list captured;
	va_copy(captured, ap);
	int formatted = vsnprintf(record + 4, sizeof(record) - 4, f, captured);
	va_end(captured);
	size_t length;
	if (formatted < 0) {
		static const char error[] = "[format error]\n";
		memcpy(record + 4, error, sizeof(error) - 1);
		length = 4 + sizeof(error) - 1;
	} else if ((size_t)formatted >= sizeof(record) - 4) {
		static const char truncated[] = "... [truncated]\n";
		length = sizeof(record);
		memcpy(record + length - (sizeof(truncated) - 1),
			truncated, sizeof(truncated) - 1);
	} else {
		length = 4 + (size_t)formatted;
		if (length == 4 || record[length - 1] != '\n') record[length++] = '\n';
	}
	klog_append(record, length);
#ifdef LINUXU_DEXT_DK
	/* Errors and worse are events (a ring timeout, a failed reset, a
	 * device dump): they reach the unified log, prefixed as the driver's
	 * own events are. Everything else stays in the retained ring: a dext
	 * that logs every line there gets its logging quarantined by the
	 * system for high volume. */
	(void)ap;
	if (level > 3)
		return 0;
	if (length >= sizeof(record))
		length = sizeof(record) - 1;
	record[length] = '\0';
	return IOLog("mac.linuxgpu: EVENT %s", record);
#else
	pthread_mutex_lock(&klog_lock);
	fprintf(stderr, "%s ", level_tag(level));
	vfprintf(stderr, f, ap);
	fputc('\n', stderr);
	fflush(stderr);
	pthread_mutex_unlock(&klog_lock);
	return 0;
#endif
}

static int klog_emit(const char *fmt, va_list ap)
{
	const char *f = fmt;
	int level = 6;

	/* parse an embedded level: "<N>" */
	if (f[0] == '<' && f[1] >= '0' && f[1] <= '7' && f[2] == '>') {
		level = f[1] - '0';
		f += 3;
	} else if (f[0] == '\001' && f[1] >= '0' && f[1] <= '7') {
		level = f[1] - '0';
		f += 2;
	}

	return klog_vemit(level, f, ap);
}

static void klog_at_level(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	(void)klog_vemit(level, fmt, ap);
	va_end(ap);
}

void dev_printk_impl(const char *prefix, const char *name, const char *fmt, ...)
{
	int level = 6;
	char message[1024];
	va_list ap;
	if (prefix && prefix[0] == '\001' && prefix[1] >= '0' && prefix[1] <= '7')
		level = prefix[1] - '0';
	else if (prefix && prefix[0] == '<' && prefix[1] >= '0' && prefix[1] <= '7')
		level = prefix[1] - '0';
	if (level > klog_get_level())
		return;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	klog_at_level(level, "%s: %s", name ? name : "device", message);
}

/* ---- vprintk family (printk.h) ---- */
int vprintk(const char *fmt, va_list args)
{
	return klog_emit(fmt, args);
}



int vprintk_emit(int facility, int level, const struct dev_printk_info *info,
		 const char *fmt, va_list args)
{
	(void)facility; (void)info;
	return klog_vemit(level >= 0 && level <= 7 ? level : 6, fmt, args);
}

int printk(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = klog_emit(fmt, ap);
	va_end(ap);
	return n;
}

int printk_ratelimited(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = klog_emit(fmt, ap);
	va_end(ap);
	return n;
}

int (printk_once)(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = klog_emit(fmt, ap);
	va_end(ap);
	return n;
}

int printk_deferred(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = klog_emit(fmt, ap);
	va_end(ap);
	return n;
}

int vprintk_deferred(const char *fmt, va_list args)
{
	return vprintk(fmt, args);
}

int vprintk_sprint(const char *fmt, va_list args, char *buf,
		   size_t size)
{
	va_list ap;
	va_copy(ap, args);
	int n;

	if (buf && size)
		n = vsnprintf(buf, size, fmt, ap);
	else
		n = 0;
	va_end(ap);
	return n;
}

int vscnprintf(char *buf, size_t size, const char *fmt,
	       va_list args)
{
	va_list ap;
	va_copy(ap, args);

	int written = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (written < 0 || !size) return 0;
	return (size_t)written < size ? written : (int)(size - 1);
}

/* ---- panic / die / dump_stack ---- */
/* Linux panic() never returns. The formatted reason goes to the retained
 * ring first; linuxu_fatal() then quarantines the device and parks the
 * thread in the dext (no deliberate process exit), or aborts on the host. */
void panic(const char *fmt, ...)
{
	char reason[256];
	va_list ap;
	int length;

	va_start(ap, fmt);
	length = vsnprintf(reason, sizeof(reason), fmt ? fmt : "?", ap);
	va_end(ap);
	if (length < 0)
		reason[0] = '\0';
	klog_at_level(0, "Kernel panic - not syncing: %s", reason);
#if LINUXU_FATAL_PARK
	linuxu_fatal("panic", __FILE__, __LINE__);
#else
	abort();
#endif
}

void die(const char *str, struct pt_regs *regs, int err)
{
	(void)regs;
	fprintf(stderr, "Kernel die: %s (err %d)\n", str ? str : "?", err);
}

void dump_stack(void)
{
	fprintf(stderr, "---- linuxu dump_stack ----\n");
}

/* ---- dev_* helpers (device.h routes to dev_printk → vprintk) ---- */
int dev_printk(int level, struct device *dev, const char *fmt, ...)
{
	va_list ap;
	int n;

	(void)dev;
	va_start(ap, fmt);
	n = klog_emit(fmt, ap);
	va_end(ap);
	return n;
}

/* vdev_printk for the _ratelimited/_once variants */
int vdev_printk(int level, struct device *dev, const char *fmt,
		va_list args)
{
	(void)level; (void)dev;
	return vprintk(fmt, args);
}

int vprintk_store(int level, const char *fmt, va_list args,
		  const char *file, int line, const char *func)
{
	(void)level; (void)file; (void)line; (void)func;
	return vprintk(fmt, args);
}

/* ratelimit runtime (vendor 2026 ___ratelimit; in-process shim keeps the
 * same token-bucket semantics with jiffies replaced by a monotonic
 * millisecond clock). */
#include <time.h>
#include <linux/ratelimit.h>

DEFINE_RATELIMIT_STATE(printk_ratelimit_state,
                      DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);

static unsigned long ratelimit_ms(void)
{
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int ___ratelimit(struct ratelimit_state *rs, const char *func)
{
	unsigned long now = ratelimit_ms(), flags;
	int allowed = 0;
	(void)func;
	if (!rs) return 0;
	raw_spin_lock_irqsave(&rs->lock, flags);
	if (!rs->interval) {
		allowed = 1;
		goto out;
	}
	if (rs->interval < 0 || rs->burst <= 0) goto missed;
	unsigned long interval_ms = (unsigned long)rs->interval * (1000 / HZ);
	if (!(rs->flags & RATELIMIT_INITIALIZED) ||
	    now - rs->begin >= interval_ms) {
		rs->begin = now;
		rs->flags |= RATELIMIT_INITIALIZED;
		atomic_set(&rs->rs_n_left, rs->burst);
		atomic_set(&rs->missed, 0);
	}
	if (atomic_read(&rs->rs_n_left) > 0) {
		atomic_dec(&rs->rs_n_left);
		allowed = 1;
		goto out;
	}
missed:
	atomic_inc(&rs->missed);
out:
	raw_spin_unlock_irqrestore(&rs->lock, flags);
	return allowed;
}

/*
 * kstrtox runtime (vendor 2026 kstrtox.h API). kstrtoull/kstrtoll are
 * builtins; _kstrtol/_kstrtoul are thin wrappers.
 */
#include <linux/kstrtox.h>

/* Parse Linux sysfs integers without libc errno/TLS dependencies. Only one
 * trailing newline is accepted, and callers retain their output on failure. */
static int linuxu_parse_unsigned(const char *s, unsigned int base,
                                unsigned long long limit,
                                unsigned long long *result)
{
	if (!s || !result || (base && (base < 2 || base > 36))) return -EINVAL;
	if (!base) base = s[0] == '0' ? 8 : 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
	    (base == 8 || base == 16)) {
		/* A hexadecimal prefix is automatic only with base zero. */
		if (base == 8) return -EINVAL;
		s += 2;
	}
	unsigned long long value = 0;
	unsigned int digits = 0;
	for (;; ++s) {
		unsigned int digit;
		if (*s >= '0' && *s <= '9') digit = (unsigned)(*s - '0');
		else if (*s >= 'a' && *s <= 'z') digit = (unsigned)(*s - 'a') + 10;
		else if (*s >= 'A' && *s <= 'Z') digit = (unsigned)(*s - 'A') + 10;
		else break;
		if (digit >= base) break;
		if (value > (limit - digit) / base) return -ERANGE;
		value = value * base + digit;
		++digits;
	}
	if (*s == '\n') ++s;
	if (!digits || *s) return -EINVAL;
	*result = value;
	return 0;
}

static unsigned int linuxu_integer_base(const char *s, unsigned int base)
{
	if (base) return base;
	if (s[0] == '0') return s[1] == 'x' || s[1] == 'X' ? 16 : 8;
	return 10;
}

int kstrtoull(const char *s, unsigned int base, unsigned long long *res)
{
	if (!s || !res || *s == '-') return -EINVAL;
	if (*s == '+') ++s;
	return linuxu_parse_unsigned(s, linuxu_integer_base(s, base), ULLONG_MAX, res);
}

int kstrtoll(const char *s, unsigned int base, long long *res)
{
	if (!s || !res) return -EINVAL;
	bool negative = *s == '-';
	if (*s == '-' || *s == '+') ++s;
	unsigned long long value;
	int error = linuxu_parse_unsigned(s, linuxu_integer_base(s, base),
		negative ? (unsigned long long)LLONG_MAX + 1 : LLONG_MAX, &value);
	if (error) return error;
	*res = negative ? (value == (unsigned long long)LLONG_MAX + 1 ?
		LLONG_MIN : -(long long)value) : (long long)value;
	return 0;
}

int _kstrtol(const char *s, unsigned int base, long *res)
{
	return kstrtoll(s, base, (long long *)res);
}

int _kstrtoul(const char *s, unsigned int base, unsigned long *res)
{
	return kstrtoull(s, base, (unsigned long long *)res);
}

/* backlight registration (in-process: never used, but must link) */
#include <linux/backlight.h>
#include <linux/slab.h>

struct backlight_device *backlight_device_register(
	const char *name, struct device *dev, void *devdata,
	const struct backlight_ops *ops,
	const struct backlight_properties *props)
{
	struct backlight_device *bd;

	bd = (struct backlight_device *)kzalloc(sizeof(*bd), GFP_KERNEL);
	if (!bd)
		return ERR_PTR(-ENOMEM);
	bd->ops = ops;
	if (props)
		bd->props = *props;
	if (dev)
		dev_set_drvdata(dev, devdata);
	return bd;
}

void backlight_device_unregister(struct backlight_device *bd)
{
	kfree(bd);
}

/* rt/park.h: a wait whose condition came true without a wake, in the
 * kernel log (overrides task.c's stderr default). */
void linuxu_wait_report(const char *message)
{
	printk(KERN_WARNING "%s", message);
}

/* rt/device_string.h: the VRAM aperture gated, in the kernel log. */
void linuxu_aperture_report(const char *why)
{
	printk(KERN_WARNING "linuxu: VRAM aperture closed to CPU access: %s\n", why);
}
