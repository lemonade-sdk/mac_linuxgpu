/* Linux-file clients (linuxu/src/amdgpu-rt/lx_files.c): one Linux process
 * per macOS client, whose descriptor table holds the character devices the
 * client opened, driven through their file_operations as Linux drives them
 * for a process making system calls. This is the dext side of the RPC in
 * rt/lx_abi.h, kept free of DriverKit so it runs in host tests.
 *
 * Process. rt_lx_client_create makes a linuxu process (task, mm, files)
 * with the client's pid and command name. Every call enters the process on
 * the calling thread (linuxu_process_enter), so upstream sees `current`,
 * `current->mm` and `current->files` as it would on Linux.
 *
 * Descriptors. rt_lx_open opens the GPU's render node, its primary node
 * (never as DRM master) or /dev/kfd as chrdev_open does and installs the
 * file in the process's table; the descriptor is what the client names in
 * later calls. Descriptors an ioctl creates (syncobj and dma-buf fds) live
 * in the same table.
 *
 * ioctl. The request frame's segments become user memory of the process
 * for the call: each page a segment touches is mapped at the client's
 * address with the segment bytes in it. Pages are shared and reference
 * counted, so concurrent calls whose arguments share a page see one page,
 * as threads of one Linux process do. Only commands a Linux client of that
 * device issues are admitted (mlg_lx_cmd_known).
 *
 * Async. rt_lx_ioctl_async runs one call on a worker thread of the process
 * and reports it through a callback; the result waits for rt_lx_result
 * unless the callback consumed it.
 *
 * mmap. rt_lx_mmap calls the file's ->mmap on a new VMA, then describes
 * the memory behind it for the dext to map into the client: the host pages
 * of a GTT buffer, or BAR ranges (CPU-visible VRAM, doorbells). macOS
 * cannot revoke or fault a client mapping, so a buffer stays pinned where
 * it is while mapped (a VRAM buffer first moves to CPU-visible VRAM, as the
 * Linux fault path does). rt_lx_mmap_commit places the VMA at the client's
 * address in the process's address space; rt_lx_munmap undoes both.
 *
 * Teardown. rt_lx_client_destroy kills the process (waits return), waits
 * for every worker, unmaps everything and exits the process: its files
 * close, so the render file's postclose and KFD's process release run.
 *
 * Shared with DriverKit and C++ callers: no kernel types. */
#ifndef LINUXU_RT_LX_FILES_H
#define LINUXU_RT_LX_FILES_H

#include <stddef.h>
#include <stdint.h>
#include <rt/lx_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;
struct rt_lx_client;

/* A client of the GPU bound to @pdev (PCI drvdata: its drm_device).
 * @pid <= 0 takes a counter value. */
int rt_lx_client_create(struct pci_dev *pdev, int pid, const char *comm,
			struct rt_lx_client **out);
void rt_lx_client_destroy(struct rt_lx_client *c);
/* rt_lx_client_destroy on a thread of its own, then @then(@arg) there: for
 * a caller that must not wait (the driver's incoming-call thread, which a
 * call that never returns would otherwise hold). Takes @c in every case:
 * 0, or -EAGAIN when no thread could start, logged; then the client is
 * destroyed by rt_lx_retire_drain and @then is not called. */
int rt_lx_client_retire(struct rt_lx_client *c, void (*then)(void *arg), void *arg);
/* Wait until every retiring client is destroyed (a session close, before
 * the driver goes). */
void rt_lx_retire_drain(void);
/* Clients still being destroyed by rt_lx_client_retire. */
unsigned int rt_lx_retiring(void);
int rt_lx_client_pid(const struct rt_lx_client *c);

/* The display hooks for the client's primary-node files and LX_SCANOUT
 * (lx_internal.h; the driver's are rt_display_lx_hooks). Without them the
 * primary node does not open and LX_SCANOUT is -ENODEV. Set once, before
 * the client's first call. */
struct rt_lx_display_hooks;
void rt_lx_client_set_display(struct rt_lx_client *c, const struct rt_lx_display_hooks *hooks);
/* LX_SCANOUT (rt/lx_abi.h): 0 or -errno, with *state filled. */
int rt_lx_scanout(struct rt_lx_client *c, const struct mlg_lx_scanout *req,
		  struct mlg_lx_scanout_state *state);

/* open(2): the descriptor or -errno. */
int rt_lx_open(struct rt_lx_client *c, uint32_t dev, uint32_t flags);
int rt_lx_close(struct rt_lx_client *c, int fd);
/* Descriptors open in the process (for tests and diagnostics). */
unsigned int rt_lx_open_files(struct rt_lx_client *c);

/* One ioctl with its request frame. Returns 0 when the call ran, with the
 * ioctl's return value in *result and the reply frame in @rbuf (@rbuf_cap
 * must hold mlg_lx_reply_bytes of the request's OUT bytes), or -errno when
 * it did not run (malformed frame, closed client, no memory). */
int rt_lx_ioctl(struct rt_lx_client *c, int fd, uint32_t cmd,
		const void *frame, size_t frame_bytes, void *rbuf,
		size_t rbuf_cap, size_t *reply_bytes, int64_t *result);
/* rt_lx_ioctl for the driver's incoming-call thread (LX_IOCTL): a request
 * that can sleep (mlg_lx_cmd_sleeps) does not run; -EDEADLK, logged. */
int rt_lx_ioctl_nosleep(struct rt_lx_client *c, int fd, uint32_t cmd,
			const void *frame, size_t frame_bytes, void *rbuf,
			size_t rbuf_cap, size_t *reply_bytes, int64_t *result);

/* Called on the worker once the call has finished, to send its
 * completion. A reply of at most MLG_LX_ASYNC_INLINE_BYTES is in @rbuf
 * until it returns and is then forgotten: the completion carries it. A
 * longer one comes with @rbuf NULL and is kept for rt_lx_result, which
 * finds it from the moment this is called. */
typedef void (*rt_lx_done_fn)(void *ctx, uint64_t token, int64_t result,
			      const void *rbuf, size_t reply_bytes);
/* Start the call on a worker. 0 with *token, or -errno (not started;
 * -EAGAIN when MLG_LX_MAX_ASYNC calls are outstanding). */
int rt_lx_ioctl_async(struct rt_lx_client *c, int fd, uint32_t cmd,
		      const void *frame, size_t frame_bytes, rt_lx_done_fn done,
		      void *ctx, uint64_t *token);
/* LX_CALL_ASYNC: operation @in[0] (MLG_LX_OP_*) with its scalars, @nin of
 * them in all, on a worker, as rt_lx_open, rt_lx_close, rt_lx_mmap and
 * rt_lx_munmap run them. Its result is the function's; an MMAP's reply is
 * MLG_LX_OP_MMAP_WORDS words (type, length, cache). 0 with *token, or
 * -errno (not started: -EINVAL for a malformed call, -EAGAIN as above). */
int rt_lx_op_async(struct rt_lx_client *c, const uint64_t *in, uint32_t nin,
		   rt_lx_done_fn done, void *ctx, uint64_t *token);
/* The kept result of a finished async call; frees it. -ENOENT for an
 * unknown token, -EBUSY while it still runs, -ENOSPC when @cap is short
 * (the result is kept). */
int rt_lx_result(struct rt_lx_client *c, uint64_t token, void *rbuf,
		 size_t cap, size_t *reply_bytes, int64_t *result);
/* Async calls running or kept. */
unsigned int rt_lx_async_outstanding(struct rt_lx_client *c);
/* The client's async workers (they stay, at most MLG_LX_MAX_ASYNC). */
unsigned int rt_lx_async_workers(struct rt_lx_client *c);

/* What backs a mapping. */
#define RT_LX_RANGE_CPU	1u	/* addr: dext virtual address */
#define RT_LX_RANGE_BAR	2u	/* bar: BAR index, addr: offset in the BAR */
#define RT_LX_CACHE_DEFAULT	0u
#define RT_LX_CACHE_WRITE_COMBINE 1u
#define RT_LX_CACHE_UNCACHED	2u
struct rt_lx_map_info {
	uint64_t type;		/* memory type for CopyClientMemoryForType */
	uint64_t length;
	uint32_t backing;	/* RT_LX_RANGE_* of every range */
	uint32_t cache;		/* RT_LX_CACHE_* */
	uint32_t ranges;	/* number of ranges */
	uint32_t committed;	/* placed in the process address space */
	uint64_t va;		/* where, once committed */
};
int rt_lx_mmap(struct rt_lx_client *c, int fd, uint64_t offset, uint64_t length,
	       uint32_t prot, uint32_t flags, struct rt_lx_map_info *out);
int rt_lx_map_info(struct rt_lx_client *c, uint64_t type, struct rt_lx_map_info *out);
/* Each range of the mapping, in order; @fn returning nonzero stops the
 * walk with that value. */
int rt_lx_map_ranges(struct rt_lx_client *c, uint64_t type,
		     int (*fn)(void *arg, uint32_t backing, uint32_t bar,
			       uint64_t addr, uint64_t bytes),
		     void *arg);
int rt_lx_mmap_commit(struct rt_lx_client *c, uint64_t type, uint64_t va);
int rt_lx_munmap(struct rt_lx_client *c, uint64_t type);
unsigned int rt_lx_mappings(struct rt_lx_client *c);
/* Mappings of every client that reach the GPU through a BAR (rt_lx_mmap
 * to rt_lx_munmap): while any exists, a client can store into a BAR. */
unsigned int rt_lx_bar_mappings(void);

/* In-process access for callers that are the client themselves (the CS
 * self-test): the dext address of byte @offset of a CPU-backed mapping and
 * how many bytes are contiguous from there. NULL for BAR mappings. */
void *rt_lx_map_cpu(struct rt_lx_client *c, uint64_t type, uint64_t offset,
		    uint64_t *contiguous);

#ifdef __cplusplus
}
#endif
#endif
