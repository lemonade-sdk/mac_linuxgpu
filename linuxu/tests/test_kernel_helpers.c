/* Exercise upstream byte FIFOs through the actual KFD interrupt helpers. */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/list_sort.h>
#include <linux/crc16.h>
#include <linux/property.h>

struct kfd_dev { struct { unsigned int ih_ring_entry_size; } device_info; };
struct kfd_node {
	struct kfifo ih_fifo;
	struct kfd_dev *kfd;
	int node_id;
};
#define dev_warn_ratelimited(...) ((void)0)
#include "kfd_interrupt_helpers.inc"

static void interrupt_fifo(void)
{
	struct kfd_dev device = {.device_info.ih_ring_entry_size = 32};
	struct kfd_node node = {.kfd = &device};
	u32 packet[8], *received;
	assert(!kfifo_alloc(&node.ih_fifo, 64, GFP_KERNEL));
	assert(kfifo_esize(&node.ih_fifo) == 1 && kfifo_size(&node.ih_fifo) == 64);
	for (unsigned int round = 0; round < 128; round++) {
		for (unsigned int entry = 0; entry < 2; entry++) {
			for (unsigned int j = 0; j < 8; j++) packet[j] = 1000 * round + entry * 10 + j;
			assert(enqueue_ih_ring_entry(&node, packet));
		}
		assert(kfifo_len(&node.ih_fifo) == 64);
		assert(!enqueue_ih_ring_entry(&node, packet));
		for (unsigned int entry = 0; entry < 2; entry++) {
			assert(dequeue_ih_ring_entry(&node, &received));
			for (unsigned int j = 0; j < 8; j++) assert(received[j] == 1000 * round + entry * 10 + j);
			kfifo_skip_count(&node.ih_fifo, 32);
		}
		assert(!dequeue_ih_ring_entry(&node, &received));
	}
	/* Indices are monotonic unsigned counters, including wrap at UINT_MAX. */
	node.ih_fifo.kfifo.in = node.ih_fifo.kfifo.out = UINT_MAX - 31;
	assert(enqueue_ih_ring_entry(&node, packet));
	assert(dequeue_ih_ring_entry(&node, &received));
	assert(!memcmp(received, packet, sizeof(packet)));
	kfifo_skip_count(&node.ih_fifo, sizeof(packet));
	assert(kfifo_is_empty(&node.ih_fifo));
	kfifo_free(&node.ih_fifo);
	assert(!node.ih_fifo.kfifo.data && !kfifo_initialized(&node.ih_fifo));
	assert(kfifo_alloc(&node.ih_fifo, 1, GFP_KERNEL) == -EINVAL);
}

static void typed_and_record_fifos(void)
{
	DECLARE_KFIFO(words, u32, 4);
	INIT_KFIFO(words);
	u32 input[] = {3, 5, 7, 9, 11}, output[5] = {0};
	assert(kfifo_in(&words, input, 5) == 4);
	assert(kfifo_out(&words, output, 2) == 2);
	assert(kfifo_in(&words, input + 4, 1) == 1);
	assert(kfifo_out(&words, output + 2, 3) == 3);
	assert(!memcmp(input, output, sizeof(input)));
	struct kfifo_rec_ptr_1 records;
	assert(!kfifo_alloc(&records, 32, GFP_KERNEL));
	const char text[] = "interrupt-data";
	char buffer[32];
	for (int i = 0; i < 16; i++) {
		assert(kfifo_in(&records, text, sizeof(text)) == sizeof(text));
		assert(kfifo_peek_len(&records) == sizeof(text));
		assert(kfifo_out(&records, buffer, sizeof(buffer)) == sizeof(text));
		assert(!memcmp(buffer, text, sizeof(text)));
	}
	kfifo_free(&records);
}

struct sortable { int key, order; struct list_head link; };
static int compare(void *priv, const struct list_head *a, const struct list_head *b)
{
	assert(priv == &compare);
	const struct sortable *left = list_entry(a, struct sortable, link);
	const struct sortable *right = list_entry(b, struct sortable, link);
	return left->key - right->key;
}
static void stable_sort(void)
{
	LIST_HEAD(head);
	struct sortable items[257];
	list_sort(&compare, &head, compare);
	for (int i = 0; i < 257; i++) {
		items[i].key = (i * 31) % 17; items[i].order = i;
		list_add_tail(&items[i].link, &head);
	}
	list_sort(&compare, &head, compare);
	int key = -1, order = -1, count = 0;
	struct list_head *it;
	list_for_each(it, &head) {
		struct sortable *item = list_entry(it, struct sortable, link);
		assert(item->key >= key);
		if (item->key == key) assert(item->order > order);
		assert(it->next->prev == it && it->prev->next == it);
		key = item->key; order = item->order; count++;
	}
	assert(count == 257);
}

int main(void)
{
	u32 property = 0x1234;
	const char *text_property = "unchanged";
	assert(device_property_read_u32(NULL, "missing", &property) == -ENXIO);
	assert(property == 0x1234);
	assert(device_property_read_string(NULL, "missing", &text_property) == -ENXIO);
	assert(!strcmp(text_property, "unchanged"));
	assert(device_property_read_u32(NULL, "missing", NULL) == -EINVAL);
	interrupt_fifo();
	typed_and_record_fifos();
	stable_sort();
	assert(crc16(0, (const u8 *)"123456789", 9) == 0xbb3d);
	assert(crc16(crc16(0, (const u8 *)"1234", 4), (const u8 *)"56789", 5) == 0xbb3d);
	assert(crc16(0x1234, NULL, 0) == 0x1234);
	puts("Pinned kernel helpers: KFD byte interrupts, FIFO wrap/records, stable list sort and CRC passed");
}
