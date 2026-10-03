/* linuxu: SHIM (third_party/linux/include/linux/file.h)
 * fd wrappers for the pinned 2026 surface used by the driver
 * (amdgpu_sched.c, drm_file.c). Tagged pointers retain file references. */
#ifndef __LINUX_FILE_H
#define __LINUX_FILE_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/cleanup.h>

struct file;

extern void fput(struct file *file);
/* either a reference to struct file + flags
 * (cloned vs. borrowed, pos locked), with
 * flags stored in lower bits of value,
 * or empty (represented by 0).
 */
struct fd {
	unsigned long word;
};
#define FDPUT_FPUT       1
#define FDPUT_POS_UNLOCK 2

#define fd_file(f) ((struct file *)((f).word & ~(FDPUT_FPUT|FDPUT_POS_UNLOCK)))
static inline bool fd_empty(struct fd f)
{
	return unlikely(!f.word);
}

#define EMPTY_FD (struct fd){0}
static inline struct fd BORROWED_FD(struct file *f)
{
	return (struct fd){(unsigned long)f};
}
static inline struct fd CLONED_FD(struct file *f)
{
	return (struct fd){(unsigned long)f | FDPUT_FPUT};
}

static inline void fdput(struct fd fd)
{
	if (unlikely(fd.word & FDPUT_FPUT))
		fput(fd_file(fd));
}

extern struct file *fget(unsigned int fd);
extern struct file *fget_raw(unsigned int fd);
extern void __f_unlock_pos(struct file *file);

/* Descriptors index current->files (linux/fdtable.h; the kernel table for
 * a task without files). */
extern void __f_lock_pos(struct file *file);
extern int close_fd(unsigned int fd);
static inline struct fd fdget(unsigned int fd)
{
	struct file *file = fget(fd);
	return file ? CLONED_FD(file) : EMPTY_FD;
}
static inline struct fd fdget_raw(unsigned int fd) { return fdget(fd); }
static inline struct fd fdget_pos(unsigned int fd)
{
	struct fd result = fdget(fd);
	if (!fd_empty(result)) {
		__f_lock_pos(fd_file(result));
		result.word |= FDPUT_POS_UNLOCK;
	}
	return result;
}

static inline void fdput_pos(struct fd f)
{
	if (f.word & FDPUT_POS_UNLOCK)
		__f_unlock_pos(fd_file(f));
	fdput(f);
}

/* fd classes are emitted at the end of this header (they need the
 * fdget()/put_unused_fd() declarations to be in scope). */

/* struct fd_prepare (vendor 2026 fd-prepare surface) */
struct fd_prepare {
	s32 err;
	s32 __fd;
	struct file *__file;
};

typedef struct fd_prepare class_fd_prepare_t;

#define fd_prepare_fd(_fdf) ((_fdf).__fd)
#define fd_prepare_file(_fdf) ((_fdf).__file)

/* local decls (also declared in <linux/fs.h>; the class machinery below
 * needs them in scope even when file.h is included before fs.h) */
extern int get_unused_fd_flags(unsigned flags);
extern void put_unused_fd(unsigned int fd);
extern void fd_install(unsigned int fd, struct file *file);
extern struct file *file_clone_open(struct file *file);

static inline void class_fd_prepare_destructor(const struct fd_prepare *fdf)
{
	if (unlikely(fdf->__fd >= 0))
		put_unused_fd(fdf->__fd);
	if (unlikely(!IS_ERR_OR_NULL(fdf->__file)))
		fput(fdf->__file);
}

static inline int class_fd_prepare_lock_err(const struct fd_prepare *fdf)
{
	if (unlikely(fdf->err))
		return fdf->err;
	if (unlikely(fdf->__fd < 0))
		return fdf->__fd;
	if (unlikely(IS_ERR_OR_NULL(fdf->__file)))
		return fdf->__file ? PTR_ERR(fdf->__file) : -ENOMEM;
	return 0;
}

#define ACQUIRE_ERR(_name, _var) class_fd_prepare_lock_err(_var)

#define __FD_PREPARE_INIT(_fd_flags, _file_owned)                 \
	({                                                        \
		struct fd_prepare fdf = {                         \
			.__fd = get_unused_fd_flags((_fd_flags)), \
		};                                                \
		if (likely(fdf.__fd >= 0))                        \
			fdf.__file = (_file_owned);               \
		fdf.err = ACQUIRE_ERR(fd_prepare, &fdf);          \
		fdf;                                              \
	})

#define FD_PREPARE(_fdf, _fd_flags, _file_owned) \
	CLASS_INIT(fd_prepare, _fdf, __FD_PREPARE_INIT(_fd_flags, _file_owned))

#define fd_publish(_fdf)                                       \
	({                                                     \
		struct fd_prepare *fdp = &(_fdf);              \
		fd_install(fdp->__fd, fdp->__file);            \
		(void)retain_and_null_ptr(fdp->__file);        \
		take_fd(fdp->__fd);                      \
	})

#define take_fd(fd) __get_and_null(fd, -EBADF)

#define __FD_ADD(_fdf, _fd_flags, _file_owned)            \
	({                                                \
		FD_PREPARE(_fdf, _fd_flags, _file_owned); \
		s32 ret = _fdf.err;                       \
		if (likely(!ret))                         \
			ret = fd_publish(_fdf);           \
		ret;                                      \
	})

#define FD_ADD(_fd_flags, _file_owned) \
	__FD_ADD(__UNIQUE_ID(fd_prepare), _fd_flags, _file_owned)

/*
 * fd classes (vendor 2026 cleanup.h DEFINE_CLASS shape) — emitted manually
 * instead of via DEFINE_CLASS because the constructor parameter is named
 * `fd`, which clashes with the struct fd type name.
 */
typedef struct fd class_fd_t;
typedef struct fd lock_fd_t;
static __always_inline void class_fd_destructor(struct fd *p)
{
	struct fd _T = *p;
	{ fdput(_T); }
	*p = (struct fd){0};
}
static __always_inline struct fd class_fd_constructor(int arg)
{
	struct fd t = fdget(arg);
	return t;
}
typedef struct fd class_fd_raw_t;
static __always_inline void class_fd_raw_destructor(struct fd *p)
{
	struct fd _T = *p;
	{ fdput(_T); }
	*p = (struct fd){0};
}
static __always_inline struct fd class_fd_raw_constructor(int arg)
{
	struct fd t = fdget_raw(arg);
	return t;
}
typedef struct fd class_fd_pos_t;
static __always_inline void class_fd_pos_destructor(struct fd *p)
{
	struct fd _T = *p;
	{ fdput_pos(_T); }
	*p = (struct fd){0};
}
static __always_inline struct fd class_fd_pos_constructor(int arg)
{
	struct fd t = fdget_pos(arg);
	return t;
}
/* get_unused_fd class is emitted by <linux/fs.h> (it needs
 * put_unused_fd/get_unused_fd_flags in scope). */

#endif /* __LINUX_FILE_H */
