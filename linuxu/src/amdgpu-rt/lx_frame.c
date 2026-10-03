/* Request frames of the Linux-file RPC (rt/lx_abi.h): the dext's checker
 * and the client side's encoder and reply decoder. Pure C without kernel
 * types: the dext, the in-dext self-test and the client library (libmlg_drm)
 * build this one file. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <rt/lx_abi.h>

static int segment_check(const struct mlg_lx_segment *seg, uint64_t *cursor,
			 uint64_t bytes, uint64_t prev_end)
{
	if (seg->reserved || !seg->dir || (seg->dir & ~(uint32_t)MLG_LX_SEG_INOUT))
		return -MLG_LX_EINVAL;
	if (!seg->size)
		return -MLG_LX_EINVAL;
	if (seg->size > MLG_LX_MAX_SEGMENT_BYTES)
		return -MLG_LX_E2BIG;
	if (seg->va < MLG_LX_VA_MIN || seg->va >= MLG_LX_VA_LIMIT ||
	    seg->size > MLG_LX_VA_LIMIT - seg->va)
		return -MLG_LX_EFAULT;
	/* Sorted and disjoint: the client merges what overlaps. */
	if (seg->va < prev_end)
		return -MLG_LX_EINVAL;
	if (seg->dir & MLG_LX_SEG_IN) {
		/* IN bytes follow the table in segment order, back to back. */
		if (seg->data_offset != *cursor || seg->size > bytes - *cursor)
			return -MLG_LX_EINVAL;
		*cursor += seg->size;
	} else if (seg->data_offset) {
		return -MLG_LX_EINVAL;
	}
	return 0;
}

int mlg_lx_frame_check(const void *frame, size_t bytes, uint32_t cmd,
		       uint64_t *out_bytes)
{
	struct mlg_lx_frame head;
	uint64_t cursor, prev_end = 0, out = 0;
	int timeout_found = 0;

	if (!frame || bytes < sizeof(head))
		return -MLG_LX_EINVAL;
	if (bytes > MLG_LX_MAX_FRAME_BYTES)
		return -MLG_LX_E2BIG;
	memcpy(&head, frame, sizeof(head));
	if (head.magic != MLG_LX_FRAME_MAGIC || head.version != MLG_LX_VERSION ||
	    head.header_bytes != sizeof(head) || head.total_bytes != bytes ||
	    head.cmd != cmd || (head.flags & ~(uint32_t)MLG_LX_FRAME_FLAGS) ||
	    head.reserved[0] || head.reserved[1])
		return -MLG_LX_EINVAL;
	if (head.nsegs > MLG_LX_MAX_SEGMENTS)
		return -MLG_LX_E2BIG;
	cursor = sizeof(head) + (uint64_t)head.nsegs * sizeof(struct mlg_lx_segment);
	if (cursor > bytes)
		return -MLG_LX_EINVAL;
	if (!(head.flags & MLG_LX_FRAME_TIMEOUT) && (head.timeout_va || head.timeout_ns))
		return -MLG_LX_EINVAL;
	for (uint32_t i = 0; i < head.nsegs; ++i) {
		struct mlg_lx_segment seg;
		int r;

		memcpy(&seg, (const uint8_t *)frame + sizeof(head) + (size_t)i * sizeof(seg),
		       sizeof(seg));
		r = segment_check(&seg, &cursor, bytes, prev_end);
		if (r)
			return r;
		prev_end = seg.va + seg.size;
		if (seg.dir & MLG_LX_SEG_OUT)
			out += seg.size;
		if ((head.flags & MLG_LX_FRAME_TIMEOUT) && (seg.dir & MLG_LX_SEG_IN) &&
		    seg.size >= 8 && head.timeout_va >= seg.va &&
		    head.timeout_va - seg.va <= seg.size - 8)
			timeout_found = 1;
	}
	if (cursor != bytes)
		return -MLG_LX_EINVAL;
	if ((head.flags & MLG_LX_FRAME_TIMEOUT) && !timeout_found)
		return -MLG_LX_EINVAL;
	if (out > MLG_LX_MAX_FRAME_BYTES - sizeof(struct mlg_lx_reply))
		return -MLG_LX_E2BIG;
	if (out_bytes)
		*out_bytes = out;
	return 0;
}

/* ---- the client side ---- */

static int span_order(const void *a, const void *b)
{
	const struct mlg_lx_span *x = a, *y = b;

	return x->va < y->va ? -1 : x->va > y->va ? 1 : 0;
}

long mlg_lx_encode(uint32_t cmd, uint64_t arg, const struct mlg_lx_span *spans,
		   uint32_t count, uint64_t timeout_va, uint64_t now_ns,
		   void *buf, size_t cap, uint64_t *out_bytes)
{
	struct mlg_lx_frame head;
	struct mlg_lx_span *m;
	uint64_t in = 0, out = 0, total, cursor;
	uint32_t n = 0;
	long r = 0;

	if (count > MLG_LX_DESCRIBE_MAX || (count && !spans))
		return count ? -MLG_LX_E2BIG : -MLG_LX_EINVAL;
	m = malloc((count ? count : 1) * sizeof(*m));
	if (!m)
		return -MLG_LX_ENOMEM;
	for (uint32_t i = 0; i < count; ++i) {
		if (!spans[i].size)
			continue;
		if (!spans[i].dir || (spans[i].dir & ~(uint32_t)MLG_LX_SEG_INOUT)) {
			free(m);
			return -MLG_LX_EINVAL;
		}
		if (spans[i].va < MLG_LX_VA_MIN || spans[i].va >= MLG_LX_VA_LIMIT ||
		    spans[i].size > MLG_LX_VA_LIMIT - spans[i].va) {
			free(m);
			return -MLG_LX_EFAULT;
		}
		m[n++] = spans[i];
	}
	qsort(m, n, sizeof(*m), span_order);
	/* Merge what overlaps; ranges that only touch stay apart. */
	count = n;
	n = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (n && m[i].va < m[n - 1].va + m[n - 1].size) {
			uint64_t end = m[i].va + m[i].size;

			if (end > m[n - 1].va + m[n - 1].size)
				m[n - 1].size = end - m[n - 1].va;
			m[n - 1].dir |= m[i].dir;
			continue;
		}
		m[n++] = m[i];
	}
	if (n > MLG_LX_MAX_SEGMENTS)
		r = -MLG_LX_E2BIG;
	for (uint32_t i = 0; !r && i < n; ++i) {
		if (m[i].size > MLG_LX_MAX_SEGMENT_BYTES)
			r = -MLG_LX_E2BIG;
		if (m[i].dir & MLG_LX_SEG_IN)
			in += m[i].size;
		if (m[i].dir & MLG_LX_SEG_OUT)
			out += m[i].size;
	}
	total = sizeof(head) + (uint64_t)n * sizeof(struct mlg_lx_segment) + in;
	if (!r && (total > MLG_LX_MAX_FRAME_BYTES ||
		   out > MLG_LX_MAX_FRAME_BYTES - sizeof(struct mlg_lx_reply)))
		r = -MLG_LX_E2BIG;
	memset(&head, 0, sizeof(head));
	if (!r && timeout_va) {
		int64_t deadline;
		int covered = 0;

		for (uint32_t i = 0; i < n; ++i)
			if ((m[i].dir & MLG_LX_SEG_IN) && m[i].size >= 8 && timeout_va >= m[i].va &&
			    timeout_va - m[i].va <= m[i].size - 8)
				covered = 1;
		if (!covered) {
			r = -MLG_LX_EINVAL;
		} else {
			memcpy(&deadline, (const void *)(uintptr_t)timeout_va, sizeof(deadline));
			/* 0 polls and a negative or huge deadline never expires on
			 * either clock: those travel unchanged. */
			if (deadline > 0 && deadline < INT64_MAX / 2) {
				head.flags |= MLG_LX_FRAME_TIMEOUT;
				head.timeout_va = timeout_va;
				head.timeout_ns = (uint64_t)deadline > now_ns ?
					(uint64_t)deadline - now_ns : 0;
			}
		}
	}
	if (!r && out_bytes)
		*out_bytes = out;
	if (!r && buf && cap < total)
		r = -MLG_LX_ENOSPC;
	if (r || !buf) {
		free(m);
		return r ? r : (long)total;
	}
	head.magic = MLG_LX_FRAME_MAGIC;
	head.version = MLG_LX_VERSION;
	head.header_bytes = sizeof(head);
	head.total_bytes = (uint32_t)total;
	head.cmd = cmd;
	head.arg = arg;
	head.nsegs = n;
	memcpy(buf, &head, sizeof(head));
	cursor = sizeof(head) + (uint64_t)n * sizeof(struct mlg_lx_segment);
	for (uint32_t i = 0; i < n; ++i) {
		struct mlg_lx_segment seg = {
			.va = m[i].va, .size = (uint32_t)m[i].size, .dir = m[i].dir,
		};

		if (m[i].dir & MLG_LX_SEG_IN) {
			seg.data_offset = (uint32_t)cursor;
			memcpy((uint8_t *)buf + cursor, (const void *)(uintptr_t)m[i].va, m[i].size);
			cursor += m[i].size;
		}
		memcpy((uint8_t *)buf + sizeof(head) + (size_t)i * sizeof(seg), &seg, sizeof(seg));
	}
	free(m);
	return (long)total;
}

int mlg_lx_apply_reply(const void *frame, size_t frame_bytes, const void *reply,
		       size_t reply_bytes, int64_t *result)
{
	struct mlg_lx_frame head;
	struct mlg_lx_reply rh;
	uint64_t out = 0;
	uint32_t outs = 0;
	const uint8_t *src;

	if (!frame || !reply || frame_bytes < sizeof(head) || reply_bytes < sizeof(rh))
		return -MLG_LX_EINVAL;
	memcpy(&head, frame, sizeof(head));
	memcpy(&rh, reply, sizeof(rh));
	if (mlg_lx_frame_check(frame, frame_bytes, head.cmd, &out))
		return -MLG_LX_EINVAL;
	if (rh.magic != MLG_LX_REPLY_MAGIC || rh.version != MLG_LX_VERSION ||
	    rh.header_bytes != sizeof(rh) || rh.total_bytes != reply_bytes ||
	    reply_bytes != mlg_lx_reply_bytes(out))
		return -MLG_LX_EINVAL;
	for (uint32_t i = 0; i < head.nsegs; ++i) {
		struct mlg_lx_segment seg;

		memcpy(&seg, (const uint8_t *)frame + sizeof(head) + (size_t)i * sizeof(seg),
		       sizeof(seg));
		outs += !!(seg.dir & MLG_LX_SEG_OUT);
	}
	if (rh.out_segments != outs)
		return -MLG_LX_EINVAL;
	src = (const uint8_t *)reply + sizeof(rh);
	for (uint32_t i = 0; i < head.nsegs; ++i) {
		struct mlg_lx_segment seg;

		memcpy(&seg, (const uint8_t *)frame + sizeof(head) + (size_t)i * sizeof(seg),
		       sizeof(seg));
		if (!(seg.dir & MLG_LX_SEG_OUT))
			continue;
		memcpy((void *)(uintptr_t)seg.va, src, seg.size);
		src += seg.size;
	}
	if (result)
		*result = rh.result;
	return 0;
}
