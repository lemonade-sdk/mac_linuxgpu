#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include "dext_heap_backend.h"

static unsigned released, attached, detached, pinned, unmapped, vmaps, vunmaps, invalidated;
static int fail_attach, fail_pin, fail_map, fail_mmap;
static unsigned mmaps;
static unsigned cpu_begins, cpu_ends;
static int fail_cpu_begin;
static char backing[16384];
static int attach_cb(struct dma_buf *b, struct dma_buf_attachment *a)
{ assert(a->dmabuf==b); attached++; return fail_attach ? -EIO : 0; }
static void detach_cb(struct dma_buf *b, struct dma_buf_attachment *a)
{ assert(a->dmabuf==b); detached++; }
static int pin_cb(struct dma_buf_attachment *a)
{ (void)a; if(fail_pin)return -ENOMEM; pinned++; return 0; }
static void unpin_cb(struct dma_buf_attachment *a) { (void)a; assert(pinned); pinned--; }
static struct sg_table *map_cb(struct dma_buf_attachment *a, enum dma_data_direction d)
{
	(void)a;(void)d;
	if(fail_map)return ERR_PTR(-EIO);
	struct sg_table *t=kzalloc(sizeof(*t),0);
	if(!t)return ERR_PTR(-ENOMEM);
	t->sgl=kzalloc(sizeof(*t->sgl),0);
	if(!t->sgl){kfree(t);return ERR_PTR(-ENOMEM);}
	t->nents=t->orig_nents=1;return t;
}
static void unmap_cb(struct dma_buf_attachment *a,struct sg_table *t,enum dma_data_direction d)
{(void)a;(void)d;unmapped++;kfree(t->sgl);kfree(t);}
static void release_cb(struct dma_buf *b){(void)b;assert(!pinned);released++;}
static int vmap_cb(struct dma_buf *b, struct iosys_map *m)
{(void)b;vmaps++;iosys_map_set_vaddr(m,backing);return 0;}
static void vunmap_cb(struct dma_buf *b,struct iosys_map *m)
{(void)b;assert(m->vaddr==backing);vunmaps++;}
static void invalidate_cb(struct dma_buf_attachment *a){(void)a;invalidated++;}
static int mmap_cb(struct dma_buf *b, struct vm_area_struct *vma)
{ assert(vma->vm_file==b->file); mmaps++; return fail_mmap ? -ENOMEM : 0; }
static int begin_cb(struct dma_buf *b, enum dma_data_direction d)
{ (void)b; (void)d; cpu_begins++; return fail_cpu_begin ? -EIO : 0; }
static int end_cb(struct dma_buf *b, enum dma_data_direction d)
{ (void)b; (void)d; cpu_ends++; return 0; }
static void *signal_later(void *arg)
{ usleep(15000); dma_fence_signal(arg); return NULL; }
static const struct dma_buf_ops ops={.attach=attach_cb,.detach=detach_cb,
 .pin=pin_cb,.unpin=unpin_cb,.map_dma_buf=map_cb,.unmap_dma_buf=unmap_cb,
 .release=release_cb,.vmap=vmap_cb,.vunmap=vunmap_cb,.mmap=mmap_cb,
 .begin_cpu_access=begin_cb,.end_cpu_access=end_cb};
static const struct dma_buf_attach_ops importer={.invalidate_mappings=invalidate_cb};
int main(void)
{
	struct device device={0};
	struct dma_buf_export_info info={.size=sizeof(backing),.ops=&ops};
	assert(IS_ERR(dma_buf_export(NULL)));
	const size_t base = dext_heap_test_live_allocations();
	for (int budget=0;budget<5;++budget) {
		dext_heap_test_fail_after(budget);
		struct dma_buf *failed=dma_buf_export(&info);
		dext_heap_test_fail_after(-1);
		if (!IS_ERR(failed)) dma_buf_put(failed);
		assert(dext_heap_test_live_allocations()==base);
	}
	released=0;
	struct dma_buf *b=dma_buf_export(&info);assert(!IS_ERR(b));
	static const struct dma_fence_ops fence_ops={0};
	struct dma_fence *fence=kmalloc(sizeof(*fence),GFP_KERNEL); assert(fence);
	dma_fence_init64(fence,&fence_ops,NULL,dma_fence_context_alloc(1),1);
	assert(!dma_resv_lock(b->resv,NULL));
	assert(!dma_resv_reserve_fences(b->resv,1));
	dma_resv_add_fence(b->resv,fence,DMA_RESV_USAGE_WRITE);
	dma_resv_unlock(b->resv);
	fail_cpu_begin=1; assert(dma_buf_begin_cpu_access(b,DMA_FROM_DEVICE)==-EIO);
	fail_cpu_begin=0;
	pthread_t signaler; assert(!pthread_create(&signaler,NULL,signal_later,fence));
	assert(!dma_buf_begin_cpu_access(b,DMA_FROM_DEVICE));
	assert(dma_fence_is_signaled(fence)); pthread_join(signaler,NULL);
	assert(!dma_buf_end_cpu_access(b,DMA_FROM_DEVICE)&&cpu_begins==2&&cpu_ends==1);
	dma_fence_put(fence);
	int fd=dma_buf_fd(b,O_CLOEXEC);assert(fd>=0);
	b=dma_buf_get(fd);assert(!IS_ERR(b));
	assert(close_fd(fd)==0 && !released);
	assert(IS_ERR(dma_buf_get(fd)));
	fail_attach=1;assert(IS_ERR(dma_buf_attach(b,&device)));fail_attach=0;
	fail_pin=1;assert(IS_ERR(dma_buf_attach(b,&device)));fail_pin=0;
	struct dma_buf_attachment *a=dma_buf_attach(b,&device);assert(!IS_ERR(a)&&pinned==1);
	fail_map=1;assert(IS_ERR(dma_buf_map_attachment_unlocked(a,DMA_TO_DEVICE)));fail_map=0;
	struct sg_table *t=dma_buf_map_attachment_unlocked(a,DMA_TO_DEVICE);assert(!IS_ERR(t));
	unsigned old=detached;dma_buf_detach(b,a);assert(detached==old);
	dma_buf_unmap_attachment_unlocked(a,t,DMA_FROM_DEVICE);assert(!unmapped);
	dma_buf_unmap_attachment_unlocked(a,t,DMA_TO_DEVICE);assert(unmapped==1);
	dma_buf_detach(b,a);assert(!pinned && detached==old+1);
	const struct dma_buf_attach_ops peer_only={.allow_peer2peer=true};
	a=dma_buf_dynamic_attach(b,&device,&peer_only,NULL);assert(!IS_ERR(a));
	assert(a->peer2peer&&pinned==1&&!dma_buf_attach_revocable(a));
	assert(!dma_resv_lock(b->resv,NULL));dma_buf_invalidate_mappings(b);
	dma_resv_unlock(b->resv);assert(!invalidated);
	dma_buf_detach(b,a);assert(!pinned);
	a=dma_buf_dynamic_attach(b,&device,&importer,NULL);assert(!IS_ERR(a));
	assert(dma_buf_attach_revocable(a));
	assert(!dma_resv_lock(b->resv,NULL));
	assert(!dma_buf_pin(a) && !dma_buf_attach_revocable(a));
	dma_buf_unpin(a);dma_buf_invalidate_mappings(b);assert(invalidated==1);
	dma_resv_unlock(b->resv);
	struct iosys_map m1,m2;
	assert(!dma_buf_vmap_unlocked(b,&m1));assert(!dma_buf_vmap_unlocked(b,&m2));
	assert(vmaps==1&&m1.vaddr==m2.vaddr);
	dma_buf_vunmap_unlocked(b,&m1);assert(!vunmaps);
	dma_buf_vunmap_unlocked(b,&m2);assert(vunmaps==1);
	dma_buf_put(b);assert(!released); /* Attachment retains exporter. */
	dma_buf_detach(a->dmabuf,a);assert(released==1);
	b=dma_buf_export(&info); assert(!IS_ERR(b));
	struct vm_area_struct vma={.vm_start=PAGE_SIZE,.vm_end=2*PAGE_SIZE};
	assert(dma_buf_mmap(b,&vma,ULONG_MAX)==-EINVAL && !mmaps);
	assert(!dma_buf_mmap(b,&vma,0) && mmaps==1 && b->file->f_count==2);
	fail_mmap=1;
	assert(dma_buf_mmap(b,&vma,0)==-ENOMEM && b->file->f_count==2);
	dma_buf_put(b); assert(released==1);
	fput(vma.vm_file); assert(released==2);
	struct dma_buf_ops direct_ops=ops; direct_ops.vunmap=NULL;
	info.ops=&direct_ops;
	b=dma_buf_export(&info); assert(!IS_ERR(b));
	assert(!dma_buf_vmap_unlocked(b,&m1));
	dma_buf_vunmap_unlocked(b,&m1); assert(!b->vmapping_counter);
	dma_buf_put(b); assert(released==3);
	rcu_barrier();
	assert(dext_heap_test_live_allocations()==base);
	puts("DMA-buf export/import, callbacks, pinning, SG maps, vmap sharing and final release passed");
}
