#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/anon_inodes.h>
#include <linux/fdtable.h>
#include <linux/dma-buf.h>
#include <linux/shmem_fs.h>
#include <linux/hwmon.h>

static int fail_alloc, releases;
void *kzalloc(size_t n, gfp_t flags) { (void)flags; return fail_alloc ? NULL : calloc(1,n); }
void kfree(const void *p) { free((void *)p); }
static int release(struct inode *inode, struct file *file)
{
	assert(inode && file->private_data == &releases);
	__atomic_add_fetch(&releases, 1, __ATOMIC_RELAXED);
	/* Release must run outside the table lock. */
	int fd = get_unused_fd_flags(0);
	assert(fd >= 0); put_unused_fd(fd);
	return 0;
}
static const struct file_operations ops = { .release = release };
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static int ready, go;
static void *reserve(void *arg)
{
	int *fd = arg;
	pthread_mutex_lock(&start_lock);
	++ready; pthread_cond_broadcast(&start_cond);
	while (!go) pthread_cond_wait(&start_cond,&start_lock);
	pthread_mutex_unlock(&start_lock);
	*fd = get_unused_fd_flags(0);
	return NULL;
}
static void *retain(void *arg)
{
	unsigned fd = *(unsigned *)arg;
	for (int i=0;i<5000;++i) {
		struct fd f = fdget(fd);
		assert(!fd_empty(f));
		assert(fd_file(f)->private_data == &releases);
		fdput(f);
	}
	return NULL;
}
int main(void)
{
	int fds[64]; pthread_t threads[64];
	for (int i=0;i<64;++i) assert(!pthread_create(&threads[i],NULL,reserve,&fds[i]));
	pthread_mutex_lock(&start_lock);
	while (ready<64) pthread_cond_wait(&start_cond,&start_lock);
	go=1; pthread_cond_broadcast(&start_cond); pthread_mutex_unlock(&start_lock);
	for (int i=0;i<64;++i) pthread_join(threads[i],NULL);
	for (int i=0;i<64;++i) { assert(fds[i]>=0); for(int j=0;j<i;++j)assert(fds[i]!=fds[j]); }
	/* Tables grow past their initial 64 slots (lowest free first) up to
	 * LINUXU_NR_OPEN, then report -EMFILE. */
	int extra = get_unused_fd_flags(0);
	assert(extra == 64);
	int last = extra;
	for (int fd; (fd = get_unused_fd_flags(0)) >= 0; last = fd) assert(fd == last + 1);
	assert(last == LINUXU_NR_OPEN - 1 && get_unused_fd_flags(0) == -EMFILE);
	for (int fd = 64; fd <= last; ++fd) put_unused_fd(fd);
	for (int i=0;i<64;++i) put_unused_fd(fds[i]);
	fail_alloc=1;
	assert(PTR_ERR(anon_inode_getfile("test",&ops,&releases,0)) == -ENOMEM);
	assert(anon_inode_getfd("test",&ops,&releases,0) == -ENOMEM);
	fail_alloc=0;
	int added = FD_ADD(0, anon_inode_getfile("test", &ops, &releases, 0));
	assert(added >= 0 && !close_fd(added) && releases == 1);
	releases = 0;
	fail_alloc=1;
	assert(FD_ADD(0, anon_inode_getfile("test", &ops, &releases, 0)) == -ENOMEM);
	fail_alloc=0;
	assert(FD_ADD(0, (struct file *)NULL) == -ENOMEM);
	unsigned fd = anon_inode_getfd("test",&ops,&releases,O_RDWR);
	assert(fd<64);
	put_unused_fd(fd); /* Must not release a published slot. */
	int next=get_unused_fd_flags(0);assert(next>=0 && (unsigned)next!=fd);put_unused_fd(next);
	for(int i=0;i<16;++i)assert(!pthread_create(&threads[i],NULL,retain,&fd));
	for(int i=0;i<16;++i)pthread_join(threads[i],NULL);
	struct file *file=fget(fd);assert(file && file->f_count==2);
	assert(IS_ERR(file_clone_open(file)));
	assert(!close_fd(fd) && releases==0 && fd_empty(fdget(fd)));
	assert(close_fd(fd)==-EBADF);
	fput(file);assert(releases==1);
	puts("file reservation, concurrent references, final release and clone error handling passed");
}
