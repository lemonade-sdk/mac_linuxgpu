/* linuxu: SHIM (third_party/linux/include/linux/fdtable.h)
 *
 * Descriptor tables. Each linuxu process owns one files_struct, shared by
 * its thread tasks through task->files; fget/fd_install/close_fd and
 * friends act on current->files. A task without files (a kernel thread or
 * a DriverKit thread outside any process) uses the kernel's own table.
 * Runtime: linuxu/src/shims/fd.c.
 */
#ifndef __LINUX_FDTABLE_H
#define __LINUX_FDTABLE_H

#include <linux/types.h>

struct files_struct;
struct file;

/* Upper bound on one table's size (Linux's default RLIMIT_NOFILE soft limit
 * is 1024; the hard sysctl_nr_open default is 1048576). */
#define LINUXU_NR_OPEN	4096

/* An empty table holding one reference. */
extern struct files_struct *linuxu_files_alloc(void);
extern struct files_struct *linuxu_files_get(struct files_struct *files);
/* Drop a reference; the last one closes every descriptor and frees. */
extern void linuxu_files_put(struct files_struct *files);
/* Close every installed descriptor (fput each file) and release reserved
 * slots: the descriptor half of exit_files. Returns the number closed. */
extern unsigned int linuxu_files_close_all(struct files_struct *files);
/* Number of installed descriptors (diagnostics and tests). */
extern unsigned int linuxu_files_count(struct files_struct *files);
/* The table fget() and friends use for the calling task. */
extern struct files_struct *linuxu_current_files(void);

#endif /* __LINUX_FDTABLE_H */
