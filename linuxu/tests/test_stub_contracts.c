/* Runtime contracts for services that used to be generated stubs:
 * upstream userq entry points with no user-queue IP, seq_file, I2C
 * transfers, ndelay, refcounted pids, dma_fence_dedup_array, DP link
 * arithmetic and the generated stub categories. Links build/libmacamgdu.a;
 * no device is opened. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "amdgpu.h"
#include "amdgpu_userq.h"
#include <linux/seq_file.h>
#include <linux/i2c.h>
#include <linux/rtmutex.h>
#include <linux/delay.h>
#include <linux/pid.h>
#include <linux/vgaarb.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/hrtimer.h>
#include <drm/display/drm_dp.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_utils.h>
#include <drm/drm_probe_helper.h>
#include <rt/task.h>
#include <linux/component.h>
#include "amdgpu_ras_mgr.h"

static int failures;

#define CHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++;						\
	}								\
} while (0)

/* ---- userq: upstream amdgpu_userq.c with no supported IP mask ---- */
static void test_userq(void)
{
	struct amdgpu_device *adev = calloc(1, sizeof(*adev));
	struct amdgpu_userq_mgr *mgr = calloc(1, sizeof(*mgr));

	CHECK(adev && mgr);
	if (!adev || !mgr)
		return;
	xa_init_flags(&adev->userq_doorbell_xa, XA_FLAGS_LOCK_IRQ);
	CHECK(amdgpu_userq_get_supported_ip_mask(adev) == 0);
	CHECK(amdgpu_userq_mgr_init(mgr, NULL, adev) == 0);
	CHECK(mgr->adev == adev);
	CHECK(amdgpu_userq_suspend(adev) == 0);
	CHECK(amdgpu_userq_resume(adev) == 0);
	CHECK(amdgpu_userq_stop_sched_for_enforce_isolation(adev, 0) == 0);
	CHECK(amdgpu_userq_start_sched_for_enforce_isolation(adev, 0) == 0);
	amdgpu_userq_pre_reset(adev);
	CHECK(amdgpu_userq_post_reset(adev, false) == 0);
	CHECK(amdgpu_userq_post_reset(adev, true) == 0);
	amdgpu_userq_mgr_cancel_resume(mgr);
	amdgpu_userq_mgr_fini(mgr);
	xa_destroy(&adev->userq_doorbell_xa);
	free(mgr);
	free(adev);
}

/* ---- seq_file ---- */
static int show_small(struct seq_file *m, void *v)
{
	CHECK(v == SEQ_START_TOKEN);
	seq_printf(m, "hello %d\n", 42);
	seq_puts(m, "x");
	seq_putc(m, 'y');
	CHECK(seq_write(m, "z\n", 2) == 0);
	return 0;
}

#define BIG_LINES 1000
static int show_big(struct seq_file *m, void *v)
{
	(void)v;
	for (int i = 0; i < BIG_LINES; i++)
		seq_printf(m, "line %04d\n", i);	/* 10 bytes per line */
	return 0;
}

static int show_hex(struct seq_file *m, void *v)
{
	u8 data[20];

	(void)v;
	for (int i = 0; i < 20; i++)
		data[i] = (u8)i;
	seq_hex_dump(m, "", DUMP_PREFIX_OFFSET, 16, 1, data, sizeof(data), false);
	return 0;
}

static void test_seq_file(void)
{
	struct inode inode = { 0 };
	struct file file = { 0 };
	struct seq_file *m;
	char buf[64];
	loff_t pos = 0;
	ssize_t n;

	CHECK(single_open(&file, show_small, (void *)0x1234) == 0);
	m = file.private_data;
	CHECK(m && m->private == (void *)0x1234 && m->file == &file);

	n = linuxu_seq_read_kernel(&file, buf, 4, &pos);
	CHECK(n == 4 && !memcmp(buf, "hell", 4) && pos == 4);
	n = linuxu_seq_read_kernel(&file, buf, sizeof(buf), &pos);
	CHECK(n == 9 && !memcmp(buf, "o 42\nxyz\n", 9) && pos == 13);
	n = linuxu_seq_read_kernel(&file, buf, sizeof(buf), &pos);
	CHECK(n == 0);

	CHECK(seq_lseek(&file, 6, SEEK_SET) == 6 && file.f_pos == 6);
	n = linuxu_seq_read_kernel(&file, buf, sizeof(buf), &file.f_pos);
	CHECK(n == 7 && !memcmp(buf, "42\nxyz\n", 7) && file.f_pos == 13);
	CHECK(seq_lseek(&file, -2, SEEK_CUR) == 11);
	CHECK(seq_lseek(&file, -1, SEEK_SET) == -EINVAL);
	CHECK(seq_lseek(&file, 0, 2 /* SEEK_END */) == -EINVAL);

	/* Linux user copies are rejected in this process: -EFAULT, not 0. */
	pos = 0;
	CHECK(seq_read(&file, (char __user *)buf, sizeof(buf), &pos) == -EFAULT);
	CHECK(single_release(&inode, &file) == 0);

	/* Output larger than one page grows the buffer and stays complete. */
	memset(&file, 0, sizeof(file));
	CHECK(single_open(&file, show_big, NULL) == 0);
	{
		char *all = calloc(1, BIG_LINES * 10 + 1);
		size_t total = 0;

		pos = 0;
		while (all && total < BIG_LINES * 10) {
			n = linuxu_seq_read_kernel(&file, all + total, 333, &pos);
			if (n <= 0)
				break;
			total += n;
		}
		CHECK(total == BIG_LINES * 10);
		CHECK(all && !memcmp(all + 9990, "line 0999\n", 10));
		free(all);
	}
	CHECK(single_release(&inode, &file) == 0);

	memset(&file, 0, sizeof(file));
	CHECK(single_open(&file, show_hex, NULL) == 0);
	pos = 0;
	{
		char out[256] = { 0 };

		n = linuxu_seq_read_kernel(&file, out, sizeof(out) - 1, &pos);
		CHECK(n > 0);
		CHECK(!strncmp(out, "00000000: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f\n"
			       "00000010: 10 11 12 13\n", n));
	}
	CHECK(single_release(&inode, &file) == 0);
}

/* ---- I2C ---- */
static int xfer_calls, xfer_locked, xfer_result;

static int fake_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	xfer_calls++;
	/* Linux holds the adapter segment lock across master_xfer. */
	xfer_locked = !rt_mutex_trylock(&adap->bus_lock);
	if (!xfer_locked)
		rt_mutex_unlock(&adap->bus_lock);
	if (xfer_result < 0)
		return xfer_result;
	msgs[0].buf[0] = 0x5a;
	return num;
}

static const struct i2c_algorithm fake_algo = { .master_xfer = fake_xfer };

static void test_i2c(void)
{
	struct i2c_adapter *adap = calloc(1, sizeof(*adap));
	struct i2c_adapter *bare = calloc(1, sizeof(*bare));
	struct i2c_client client = { 0 };
	u8 byte = 0;
	struct i2c_msg msg = { .addr = 0x50, .flags = I2C_M_RD, .len = 1, .buf = &byte };

	if (!adap || !bare)
		return;
	adap->algo = &fake_algo;
	strcpy(adap->name, "stub-contract");
	CHECK(i2c_add_adapter(adap) == 0);
	CHECK(adap->timeout == HZ && adap->lock_ops);

	CHECK(i2c_transfer(adap, &msg, 1) == 1);
	CHECK(byte == 0x5a && xfer_calls == 1 && xfer_locked);

	/* Arbitration loss is retried adap->retries times. */
	xfer_calls = 0;
	xfer_result = -EAGAIN;
	adap->retries = 2;
	CHECK(i2c_transfer(adap, &msg, 1) == -EAGAIN && xfer_calls == 3);
	xfer_result = -EIO;
	xfer_calls = 0;
	CHECK(i2c_transfer(adap, &msg, 1) == -EIO && xfer_calls == 1);
	xfer_result = 0;

	client.adapter = adap;
	client.addr = 0x50;
	byte = 0;
	CHECK(i2c_transfer_buffer_flags(&client, (char *)&byte, 1, I2C_M_RD) == 1);
	CHECK(byte == 0x5a);

	CHECK(i2c_transfer(adap, NULL, 0) == -EINVAL);
	i2c_mark_adapter_suspended(adap);
	CHECK(i2c_transfer(adap, &msg, 1) == -ESHUTDOWN);
	i2c_mark_adapter_resumed(adap);
	CHECK(i2c_transfer(adap, &msg, 1) == 1);

	/* No algorithm: Linux reports unsupported I2C-level transfers. */
	strcpy(bare->name, "no-algo");
	CHECK(i2c_add_adapter(bare) == 0);
	CHECK(i2c_transfer(bare, &msg, 1) == -EOPNOTSUPP);
	i2c_del_adapter(bare);
	i2c_del_adapter(adap);
	free(bare);
	free(adap);
}

/* ---- ndelay ---- */
static unsigned long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void test_ndelay(void)
{
	unsigned long long t0 = now_ns();

	ndelay(3000000);	/* 3 ms */
	CHECK(now_ns() - t0 >= 3000000ULL);
	t0 = now_ns();
	ndelay(1500);
	CHECK(now_ns() - t0 >= 1500ULL);
	ndelay(0);
}

/* ---- pids ---- */
static int released;
static void release_test_task(struct task_struct *t)
{
	released++;
	free(t);
}

static void test_pids(void)
{
	struct task_struct *self = current;
	struct task_struct *task, *got;
	struct pid *pid, *again;
	int nr;

	pid = get_task_pid(self, PIDTYPE_TGID);
	CHECK(pid != NULL);
	CHECK(pid_nr(pid) == self->tgid && pid_vnr(pid) == self->tgid);
	CHECK(task_tgid(self) == pid);
	CHECK(get_task_pid(self, PIDTYPE_PID) == pid);	/* each task leads its group */
	put_pid(pid);
	got = get_pid_task(pid, PIDTYPE_PID);
	CHECK(got == self);
	put_task_struct(got);
	CHECK(pid_task(pid, PIDTYPE_TGID) == self);
	again = find_get_pid(self->tgid);
	CHECK(again == pid);
	put_pid(again);
	put_pid(pid);
	CHECK(task_pid_vnr(self) == self->pid);
	CHECK(pid_nr(NULL) == 0 && get_pid_task(NULL, PIDTYPE_PID) == NULL);

	/* A pid outlives its task: the number stays, the task link goes. */
	task = calloc(1, sizeof(*task));
	if (!task)
		return;
	linuxu_task_init(task, "pid-test", 0);
	task->linuxu_release = release_test_task;
	pid = get_task_pid(task, PIDTYPE_TGID);
	nr = task->tgid;
	CHECK(pid && pid_nr(pid) == nr);
	got = get_pid_task(pid, PIDTYPE_PID);
	CHECK(got == task);
	put_task_struct(got);
	put_task_struct(task);		/* final task reference */
	CHECK(released == 1);
	CHECK(get_pid_task(pid, PIDTYPE_PID) == NULL);
	CHECK(pid_task(pid, PIDTYPE_PID) == NULL);
	CHECK(pid_nr(pid) == nr);
	CHECK(find_get_pid(nr) == NULL);
	put_pid(pid);
}

/* ---- dma_fence_dedup_array ---- */
static const char *fence_name(struct dma_fence *f) { (void)f; return "stub-contract"; }
static const struct dma_fence_ops test_fence_ops = {
	.get_driver_name = fence_name,
	.get_timeline_name = fence_name,
};
static spinlock_t fence_lock;

static void test_fence_dedup(void)
{
	struct dma_fence *f[4];
	u64 ctx_a = dma_fence_context_alloc(1), ctx_b = dma_fence_context_alloc(1);
	struct dma_fence *older, *newer;
	int n;

	spin_lock_init(&fence_lock);
	for (int i = 0; i < 4; i++) {
		f[i] = calloc(1, sizeof(struct dma_fence));
		if (!f[i])
			return;
	}
	dma_fence_init(f[0], &test_fence_ops, &fence_lock, ctx_a, 1);
	dma_fence_init(f[1], &test_fence_ops, &fence_lock, ctx_b, 7);
	dma_fence_init(f[2], &test_fence_ops, &fence_lock, ctx_a, 3);
	dma_fence_init(f[3], &test_fence_ops, &fence_lock, ctx_a, 2);
	older = dma_fence_get(f[0]);	/* observe the dropped references */
	newer = dma_fence_get(f[2]);

	n = dma_fence_dedup_array(f, 4);
	CHECK(n == 2);
	CHECK(f[0]->context == ctx_a && f[0] == newer && f[0]->seqno == 3);
	CHECK(f[1]->context == ctx_b && f[1]->seqno == 7);
	CHECK(kref_read(&older->refcount) == 1);	/* array reference dropped */
	CHECK(kref_read(&newer->refcount) == 2);
	CHECK(dma_fence_dedup_array(f, 0) == 0);
	dma_fence_put(f[0]);
	dma_fence_put(f[1]);
	dma_fence_put(older);
	dma_fence_put(newer);
}

/* ---- DP link arithmetic and panel quirks ---- */
static void test_dp(void)
{
	u8 status[DP_LINK_STATUS_SIZE] = { 0 };

	CHECK(drm_dp_link_rate_to_bw_code(162000) == DP_LINK_BW_1_62);
	CHECK(drm_dp_link_rate_to_bw_code(270000) == DP_LINK_BW_2_7);
	CHECK(drm_dp_link_rate_to_bw_code(540000) == DP_LINK_BW_5_4);
	CHECK(drm_dp_link_rate_to_bw_code(1000000) == DP_LINK_BW_10);
	CHECK(drm_dp_link_rate_to_bw_code(1350000) == DP_LINK_BW_13_5);
	CHECK(drm_dp_link_rate_to_bw_code(2000000) == DP_LINK_BW_20);

	/* lane0: swing 2, pre-emphasis 1; lane1: swing 3, pre-emphasis 0 */
	status[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS] = 0x36;
	CHECK(drm_dp_get_adjust_request_voltage(status, 0) == DP_TRAIN_VOLTAGE_SWING_LEVEL_2);
	CHECK(drm_dp_get_adjust_request_pre_emphasis(status, 0) == DP_TRAIN_PRE_EMPH_LEVEL_1);
	CHECK(drm_dp_get_adjust_request_voltage(status, 1) == DP_TRAIN_VOLTAGE_SWING_LEVEL_3);
	CHECK(drm_dp_get_adjust_request_pre_emphasis(status, 1) == DP_TRAIN_PRE_EMPH_LEVEL_0);

	status[0] = DP_LANE_CR_DONE | (DP_LANE_CR_DONE << 4);
	CHECK(drm_dp_clock_recovery_ok(status, 2));
	CHECK(!drm_dp_clock_recovery_ok(status, 4));
	CHECK(!drm_dp_channel_eq_ok(status, 2));
	status[0] = DP_CHANNEL_EQ_BITS | (DP_CHANNEL_EQ_BITS << 4);
	status[DP_LANE_ALIGN_STATUS_UPDATED - DP_LANE0_1_STATUS] = DP_INTERLANE_ALIGN_DONE;
	CHECK(drm_dp_channel_eq_ok(status, 2));

	CHECK(drm_get_panel_orientation_quirk(1920, 1080) ==
	      DRM_MODE_PANEL_ORIENTATION_UNKNOWN);
}

/* ---- generated stub categories ---- */
static void test_generated(void)
{
	CHECK(vga_client_register(NULL, NULL) == 0);	/* disabled-inline */
	vga_client_unregister(NULL);
	CHECK(!amdgpu_ras_mgr_is_rma(NULL));		/* unreachable: benign */
	CHECK(!amdgpu_ras_mgr_is_rma(NULL));		/* warns once only */
	CHECK(component_add(NULL, NULL) == 0);		/* ok-zero */
	/* Upstream drm_probe_helper.c now: not called from the poll worker. */
	CHECK(!drm_kms_helper_is_poll_worker());
}

int main(void)
{
	test_userq();
	test_seq_file();
	test_i2c();
	test_ndelay();
	test_pids();
	test_fence_dedup();
	test_dp();
	test_generated();
	if (failures) {
		fprintf(stderr, "stub contracts: %d failure(s)\n", failures);
		return 1;
	}
	printf("stub contracts: userq, seq_file, i2c, ndelay, pid, fence dedup, DP and generated categories passed\n");
	return 0;
}
