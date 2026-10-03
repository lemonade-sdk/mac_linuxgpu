/* In-process Linux DMA-buf ownership. Exporter callbacks provide real backing
 * and device addresses; descriptors are private to this driver's file table. */
#include <limits.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/mm.h>

struct buffer_mapping {
	struct list_head node;
	struct sg_table *table;
	enum dma_data_direction direction;
};
static int buffer_release(struct inode *inode, struct file *file)
{
	(void)inode;
	struct dma_buf *buf = file->private_data;
	bool owns = buf->linuxu_owns_resv;
	struct dma_resv *resv = buf->resv;
	buf->ops->release(buf);
	if (owns) dma_resv_put(resv);
	kfree(buf);
	return 0;
}
static const struct file_operations buffer_file_ops = { .release = buffer_release };

struct dma_buf *dma_buf_export(const struct dma_buf_export_info *info)
{
	if (!info || !info->size || !info->ops || !info->ops->release ||
	    !info->ops->map_dma_buf || !info->ops->unmap_dma_buf ||
	    (!!info->ops->pin != !!info->ops->unpin)) return ERR_PTR(-EINVAL);
	struct dma_buf *buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) return ERR_PTR(-ENOMEM);
	buf->size = info->size; buf->ops = info->ops; buf->priv = info->priv;
	buf->owner = info->owner; buf->exp_name = info->exp_name;
	buf->resv = info->resv ? info->resv : dma_resv_alloc();
	buf->linuxu_owns_resv = !info->resv;
	if (!buf->resv) { kfree(buf); return ERR_PTR(-ENOMEM); }
	INIT_LIST_HEAD(&buf->attachments); INIT_LIST_HEAD(&buf->list_node);
	spin_lock_init(&buf->name_lock); init_waitqueue_head(&buf->poll);
	buf->file = anon_inode_getfile("dmabuf", &buffer_file_ops, buf, info->flags);
	if (IS_ERR(buf->file)) {
		long error = PTR_ERR(buf->file);
		if (buf->linuxu_owns_resv) dma_resv_put(buf->resv);
		kfree(buf); return ERR_PTR(error);
	}
	return buf;
}
int dma_buf_fd(struct dma_buf *buf, int flags)
{
	if (IS_ERR_OR_NULL(buf)) return -EINVAL;
	int fd = get_unused_fd_flags(flags);
	if (fd >= 0) fd_install(fd, buf->file); /* Transfer the caller's reference. */
	return fd;
}
struct dma_buf *dma_buf_get(int fd)
{
	struct file *file = fget(fd);
	if (!file) return ERR_PTR(-EBADF);
	if (file->f_op != &buffer_file_ops) { fput(file); return ERR_PTR(-EINVAL); }
	return file->private_data;
}
void dma_buf_put(struct dma_buf *buf)
{ if (!IS_ERR_OR_NULL(buf)) fput(buf->file); }

struct dma_buf_attachment *dma_buf_dynamic_attach(struct dma_buf *buf,
	struct device *dev, const struct dma_buf_attach_ops *ops, void *priv)
{
	if (IS_ERR_OR_NULL(buf) || !dev)
		return ERR_PTR(-EINVAL);
	struct dma_buf_attachment *attach = kzalloc(sizeof(*attach), GFP_KERNEL);
	if (!attach) return ERR_PTR(-ENOMEM);
	attach->dmabuf = buf; attach->dev = dev; attach->importer_ops = ops;
	attach->importer_priv = priv; attach->peer2peer = ops && ops->allow_peer2peer;
	INIT_LIST_HEAD(&attach->node); INIT_LIST_HEAD(&attach->linuxu_mappings);
	/* Exporters such as AMDGPU take the reservation lock in attach itself. */
	int error = buf->ops->attach ? buf->ops->attach(buf, attach) : 0;
	if (error) { kfree(attach); return ERR_PTR(error); }
	error = dma_resv_lock(buf->resv, NULL);
	if (error) goto detach;
	list_add_tail(&attach->node, &buf->attachments);
	if ((!ops || !ops->invalidate_mappings) && buf->ops->pin) {
		error = buf->ops->pin(attach);
		if (!error) attach->linuxu_static_pin = true;
	}
	if (error) list_del_init(&attach->node);
	else get_dma_buf(buf); /* Keep the exporter alive until detached. */
	dma_resv_unlock(buf->resv);
	if (!error) return attach;
detach:
	if (buf->ops->detach) buf->ops->detach(buf, attach);
	kfree(attach); return ERR_PTR(error);
}
struct dma_buf_attachment *dma_buf_attach(struct dma_buf *buf, struct device *dev)
{ return dma_buf_dynamic_attach(buf, dev, NULL, NULL); }
void dma_buf_detach(struct dma_buf *buf, struct dma_buf_attachment *attach)
{
	if (IS_ERR_OR_NULL(buf) || IS_ERR_OR_NULL(attach) || attach->dmabuf != buf) return;
	if (dma_resv_lock(buf->resv, NULL)) return;
	/* Outstanding DMA mappings must be returned before detachment. Preserve
	 * backing on a caller lifetime violation rather than revoke live DMA. */
	if (!list_empty(&attach->linuxu_mappings) || attach->linuxu_pins) {
		dma_resv_unlock(buf->resv); return;
	}
	list_del_init(&attach->node);
	if (attach->linuxu_static_pin) buf->ops->unpin(attach);
	dma_resv_unlock(buf->resv);
	if (buf->ops->detach) buf->ops->detach(buf, attach);
	kfree(attach); dma_buf_put(buf);
}
int dma_buf_pin(struct dma_buf_attachment *attach)
{
	if (IS_ERR_OR_NULL(attach) || !attach->importer_ops) return -EINVAL;
	if (attach->linuxu_pins == UINT_MAX) return -EOVERFLOW;
	int r = attach->dmabuf->ops->pin ? attach->dmabuf->ops->pin(attach) : 0;
	if (!r) ++attach->linuxu_pins;
	return r;
}
void dma_buf_unpin(struct dma_buf_attachment *attach)
{
	if (IS_ERR_OR_NULL(attach) || !attach->linuxu_pins) return;
	--attach->linuxu_pins;
	if (attach->dmabuf->ops->unpin) attach->dmabuf->ops->unpin(attach);
}
struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *attach,
	enum dma_data_direction direction)
{
	if (IS_ERR_OR_NULL(attach) || !valid_dma_direction(direction)) return ERR_PTR(-EINVAL);
	struct buffer_mapping *map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) return ERR_PTR(-ENOMEM);
	struct sg_table *table = attach->dmabuf->ops->map_dma_buf(attach, direction);
	if (IS_ERR_OR_NULL(table)) {
		kfree(map); return table ? table : ERR_PTR(-ENOMEM);
	}
	if (!table->sgl || !table->nents) {
		attach->dmabuf->ops->unmap_dma_buf(attach, table, direction);
		kfree(map); return ERR_PTR(-EIO);
	}
	map->table = table; map->direction = direction;
	list_add_tail(&map->node, &attach->linuxu_mappings);
	return table;
}
void dma_buf_unmap_attachment(struct dma_buf_attachment *attach,
	struct sg_table *table, enum dma_data_direction direction)
{
	if (IS_ERR_OR_NULL(attach) || IS_ERR_OR_NULL(table)) return;
	struct buffer_mapping *map;
	list_for_each_entry(map, &attach->linuxu_mappings, node) {
		if (map->table != table || map->direction != direction) continue;
		list_del(&map->node);
		attach->dmabuf->ops->unmap_dma_buf(attach, table, direction);
		kfree(map); return;
	}
}
struct sg_table *dma_buf_map_attachment_unlocked(struct dma_buf_attachment *attach,
	enum dma_data_direction direction)
{
	if (IS_ERR_OR_NULL(attach)) return ERR_PTR(-EINVAL);
	int r = dma_resv_lock(attach->dmabuf->resv, NULL);
	if (r) return ERR_PTR(r);
	struct sg_table *table = dma_buf_map_attachment(attach, direction);
	dma_resv_unlock(attach->dmabuf->resv); return table;
}
void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *attach,
	struct sg_table *table, enum dma_data_direction direction)
{
	if (IS_ERR_OR_NULL(attach)) return;
	if (dma_resv_lock(attach->dmabuf->resv, NULL)) return;
	dma_buf_unmap_attachment(attach, table, direction);
	dma_resv_unlock(attach->dmabuf->resv);
}
void dma_buf_invalidate_mappings(struct dma_buf *buf)
{
	struct dma_buf_attachment *attach;
	list_for_each_entry(attach, &buf->attachments, node)
		if (attach->importer_ops && attach->importer_ops->invalidate_mappings)
			attach->importer_ops->invalidate_mappings(attach);
}
bool dma_buf_attach_revocable(struct dma_buf_attachment *attach)
{ return attach && attach->importer_ops &&
	attach->importer_ops->invalidate_mappings && !attach->linuxu_pins; }
int dma_buf_begin_cpu_access(struct dma_buf *buf, enum dma_data_direction direction)
{
	if (IS_ERR_OR_NULL(buf) || !valid_dma_direction(direction)) return -EINVAL;
	int r = buf->ops->begin_cpu_access ? buf->ops->begin_cpu_access(buf, direction) : 0;
	if (r) return r;
	return dma_resv_wait(buf->resv, direction == DMA_FROM_DEVICE ?
		DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ, true);
}
int dma_buf_end_cpu_access(struct dma_buf *buf, enum dma_data_direction direction)
{
	if (IS_ERR_OR_NULL(buf) || !valid_dma_direction(direction)) return -EINVAL;
	return buf->ops->end_cpu_access ? buf->ops->end_cpu_access(buf, direction) : 0;
}
int dma_buf_vmap(struct dma_buf *buf, struct iosys_map *map)
{
	if (IS_ERR_OR_NULL(buf) || !map) return -EINVAL;
	iosys_map_clear(map);
	if (!buf->ops->vmap) return -EOPNOTSUPP;
	if (buf->vmapping_counter == UINT_MAX) return -EOVERFLOW;
	if (!buf->vmapping_counter) {
		int r = buf->ops->vmap(buf, &buf->vmap_ptr);
		if (r) { iosys_map_clear(&buf->vmap_ptr); return r; }
		if (iosys_map_is_null(&buf->vmap_ptr)) return -ENOMEM;
	}
	++buf->vmapping_counter; *map = buf->vmap_ptr;
	return 0;
}
void dma_buf_vunmap(struct dma_buf *buf, struct iosys_map *map)
{
	if (IS_ERR_OR_NULL(buf) || !map || !buf->vmapping_counter ||
	    !iosys_map_is_equal(map, &buf->vmap_ptr)) return;
	if (!--buf->vmapping_counter) {
		if (buf->ops->vunmap) buf->ops->vunmap(buf, &buf->vmap_ptr);
		iosys_map_clear(&buf->vmap_ptr);
	}
	iosys_map_clear(map);
}
int dma_buf_vmap_unlocked(struct dma_buf *buf, struct iosys_map *map)
{
	if (IS_ERR_OR_NULL(buf)) return -EINVAL;
	int r = dma_resv_lock(buf->resv, NULL);
	if (r) return r;
	r = dma_buf_vmap(buf, map); dma_resv_unlock(buf->resv); return r;
}
void dma_buf_vunmap_unlocked(struct dma_buf *buf, struct iosys_map *map)
{
	if (IS_ERR_OR_NULL(buf) || dma_resv_lock(buf->resv, NULL)) return;
	dma_buf_vunmap(buf, map); dma_resv_unlock(buf->resv);
}
int dma_buf_mmap(struct dma_buf *buf, struct vm_area_struct *vma, unsigned long offset)
{
	if (IS_ERR_OR_NULL(buf) || !vma || vma->vm_end < vma->vm_start ||
	    offset > (buf->size >> PAGE_SHIFT) ||
	    vma->vm_end - vma->vm_start > buf->size - (offset << PAGE_SHIFT)) return -EINVAL;
	if (!buf->ops->mmap) return -EOPNOTSUPP;
	/* The VMA owns this reference, including callback failure cleanup. */
	struct file *old = vma->vm_file;
	vma->vm_file = get_file(buf->file);
	fput(old);
	vma->vm_pgoff = offset;
	return buf->ops->mmap(buf, vma);
}
