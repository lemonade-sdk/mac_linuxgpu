/* linuxu: SHIM (third_party/linux/include/linux/dma-buf.h)
 *
 * Aligned to the pinned vendor 2026 text (struct layout + API). The
 * unmodified third_party/linux/drivers/gpu/drm/amd/amdgpu/*.c is the ABI test: amdgpu_dma_buf.c uses
 * attach->dev / importer_ops / importer_priv and the 2-arg
 * map_dma_buf; amdgpu_cs.c walks dmabuf->attachments.
 */
#ifndef _LINUX_DMA_BUF_H
#define _LINUX_DMA_BUF_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/err.h>
#include <linux/kref.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/pci-p2pdma.h>
#include <linux/dma-mapping.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/iosys-map.h>
#include <uapi/linux/dma-heap.h>

struct device;
struct module;
struct dma_buf;
struct dma_buf_attachment;
struct dma_buf_ops;
struct dma_buf_attach_ops;

struct dma_buf_map {
	unsigned long addr;
};
#define DMA_BUF_MAP_INIT_ZERO	{ 0 }

struct dma_buf_poll_cb_t {
	struct dma_fence_cb cb;
	wait_queue_head_t *poll;
	__poll_t active;
};

struct dma_buf_ops {
	int (*attach)(struct dma_buf *, struct dma_buf_attachment *);
	void (*detach)(struct dma_buf *, struct dma_buf_attachment *);
	int (*pin)(struct dma_buf_attachment *attach);
	void (*unpin)(struct dma_buf_attachment *attach);
	struct sg_table *(*map_dma_buf)(struct dma_buf_attachment *,
					enum dma_data_direction);
	void (*unmap_dma_buf)(struct dma_buf_attachment *,
			      struct sg_table *,
			      enum dma_data_direction);
	void (*release)(struct dma_buf *);
	int (*begin_cpu_access)(struct dma_buf *, enum dma_data_direction);
	int (*end_cpu_access)(struct dma_buf *, enum dma_data_direction);
	int (*mmap)(struct dma_buf *, struct vm_area_struct *vma);
	int (*vmap)(struct dma_buf *dmabuf, struct iosys_map *map);
	void (*vunmap)(struct dma_buf *dmabuf, struct iosys_map *map);
};

struct dma_buf {
	size_t size;
	struct file *file;
	struct list_head attachments;
	const struct dma_buf_ops *ops;
	unsigned vmapping_counter;
	struct iosys_map vmap_ptr;
	const char *exp_name;
	const char *name;
	spinlock_t name_lock;
	struct module *owner;
	struct list_head list_node;
	void *priv;
	struct dma_resv *resv;
	wait_queue_head_t poll;
	struct dma_buf_poll_cb_t cb_in, cb_out;
	bool linuxu_owns_resv;
};

struct dma_buf_attach_ops {
	bool allow_peer2peer;
	void (*invalidate_mappings)(struct dma_buf_attachment *attach);
};

struct dma_buf_attachment {
	struct dma_buf *dmabuf;
	struct device *dev;
	struct list_head node;
	bool peer2peer;
	const struct dma_buf_attach_ops *importer_ops;
	void *importer_priv;
	void *priv;
	struct list_head linuxu_mappings;
	unsigned int linuxu_pins;
	bool linuxu_static_pin;
};

struct dma_buf_export_info {
	const char *exp_name;
	struct module *owner;
	const struct dma_buf_ops *ops;
	size_t size;
	int flags;
	struct dma_resv *resv;
	void *priv;
};

#define DEFINE_DMA_BUF_EXPORT_INFO(name)	\
	struct dma_buf_export_info name = { .exp_name = KBUILD_MODNAME, \
					 .owner = THIS_MODULE }

struct dma_buf_move_info {
	int direction;
	void *iommu;
};

static inline void get_dma_buf(struct dma_buf *dmabuf)
{
	get_file(dmabuf->file);
}

static inline bool dma_buf_is_dynamic(struct dma_buf *dmabuf)
{
	return !!dmabuf->ops->pin;
}

struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
					  struct device *dev);
struct dma_buf_attachment *
dma_buf_dynamic_attach(struct dma_buf *dmabuf, struct device *dev,
		       const struct dma_buf_attach_ops *importer_ops,
		       void *importer_priv);
void dma_buf_detach(struct dma_buf *dmabuf,
		    struct dma_buf_attachment *attach);
int dma_buf_pin(struct dma_buf_attachment *attach);
void dma_buf_unpin(struct dma_buf_attachment *attach);

struct dma_buf *dma_buf_export(const struct dma_buf_export_info *exp_info);

int dma_buf_fd(struct dma_buf *dmabuf, int flags);
struct dma_buf *dma_buf_get(int fd);
void dma_buf_put(struct dma_buf *dmabuf);

struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *,
					enum dma_data_direction);
void dma_buf_unmap_attachment(struct dma_buf_attachment *, struct sg_table *,
				enum dma_data_direction);
void dma_buf_invalidate_mappings(struct dma_buf *dma_buf);
bool dma_buf_attach_revocable(struct dma_buf_attachment *attach);
int dma_buf_begin_cpu_access(struct dma_buf *dma_buf,
			     enum dma_data_direction dir);
int dma_buf_end_cpu_access(struct dma_buf *dma_buf,
			   enum dma_data_direction dir);
struct sg_table *
dma_buf_map_attachment_unlocked(struct dma_buf_attachment *attach,
				enum dma_data_direction direction);
void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *attach,
				       struct sg_table *sg_table,
				       enum dma_data_direction direction);

int dma_buf_mmap(struct dma_buf *, struct vm_area_struct *,
		 unsigned long);
int dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map);
void dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map);
int dma_buf_vmap_unlocked(struct dma_buf *dmabuf, struct iosys_map *map);
void dma_buf_vunmap_unlocked(struct dma_buf *dmabuf, struct iosys_map *map);
struct dma_buf *dma_buf_iter_begin(void);
struct dma_buf *dma_buf_iter_next(struct dma_buf *dmbuf);

#endif /* _LINUX_DMA_BUF_H */
