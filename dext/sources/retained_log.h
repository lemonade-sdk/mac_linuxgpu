#ifndef MACLINUXGPU_RETAINED_LOG_H
#define MACLINUXGPU_RETAINED_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <rt/klog.h>

namespace maclinuxgpu {

/* Use printf formatting once, then give identical bounded text to the cached
 * ring and platform sink. The sink treats the result as data, so percent signs
 * in messages cannot become os_log format directives. No allocation or driver
 * operation is involved, including while a session is quarantined. */
static inline void RetainedLog(void (*sink)(const char *), const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static inline void RetainedLog(void (*sink)(const char *), const char *format, ...)
{
    char record[LINUXU_KLOG_MESSAGE_CAPACITY];
    static const char prefix[] = "mac.linuxgpu: ";
    constexpr size_t prefixLength = sizeof(prefix) - 1;
    memcpy(record, prefix, prefixLength);
    va_list arguments;
    va_start(arguments, format);
    const int formatted = vsnprintf(record + prefixLength,
        sizeof(record) - prefixLength, format, arguments);
    va_end(arguments);
    size_t length;
    if (formatted < 0) {
        static const char error[] = "[format error]\n";
        memcpy(record + prefixLength, error, sizeof(error));
        length = prefixLength + sizeof(error) - 1;
    } else if ((size_t)formatted >= sizeof(record) - prefixLength - 1) {
        static const char truncated[] = "... [truncated]\n";
        length = sizeof(record) - 1;
        memcpy(record + sizeof(record) - sizeof(truncated), truncated, sizeof(truncated));
    } else {
        length = prefixLength + (size_t)formatted;
        if (length == prefixLength || record[length - 1] != '\n')
            record[length++] = '\n';
        record[length] = '\0';
    }
    klog_write(record, length);
    if (sink) sink(record);
}

} // namespace maclinuxgpu

#endif
