/* Fuzzing the Linux-file RPC framing (linuxu/src/amdgpu-rt/lx_frame.c):
 * the dext's request checker against an independent reference, on valid
 * frames from the encoder and on random mutations of them; the encoder's
 * merge and timeout rules; and the client's reply decoder, which must write
 * only the OUT segments its own frame names. Runs under ASan and UBSan; a
 * fixed seed keeps failures reproducible (LX_FUZZ_SEED, LX_FUZZ_ROUNDS
 * override). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rt/lx_abi.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s (round %lu)\n", \
	__FILE__, __LINE__, #c, round_no); abort(); } } while (0)

static unsigned long round_no;
static uint64_t rng_state;

static uint64_t rng(void)
{
	/* xorshift64* */
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 0x2545f4914f6cdd1dULL;
}

static uint64_t below(uint64_t n)
{
	return n ? rng() % n : 0;
}

/* The client memory the frames describe. */
#define ARENA_BYTES (1u << 20)
static uint8_t *arena;

/* An independent statement of what mlg_lx_frame_check accepts. */
static int reference_check(const uint8_t *f, size_t bytes, uint32_t cmd, uint64_t *out_bytes)
{
	struct mlg_lx_frame h;
	uint64_t cursor, end = 0, out = 0;
	int timeout_ok = 0;

	if (bytes < sizeof(h))
		return -MLG_LX_EINVAL;
	if (bytes > MLG_LX_MAX_FRAME_BYTES)
		return -MLG_LX_E2BIG;
	memcpy(&h, f, sizeof(h));
	if (h.magic != MLG_LX_FRAME_MAGIC || h.version != MLG_LX_VERSION || h.header_bytes != 64 ||
	    h.total_bytes != bytes || h.cmd != cmd || (h.flags & ~1u) || h.reserved[0] ||
	    h.reserved[1])
		return -MLG_LX_EINVAL;
	if (h.nsegs > MLG_LX_MAX_SEGMENTS)
		return -MLG_LX_E2BIG;
	cursor = 64 + 24ull * h.nsegs;
	if (cursor > bytes)
		return -MLG_LX_EINVAL;
	if (!(h.flags & 1) && (h.timeout_va || h.timeout_ns))
		return -MLG_LX_EINVAL;
	for (uint32_t i = 0; i < h.nsegs; ++i) {
		struct mlg_lx_segment s;

		memcpy(&s, f + 64 + 24ull * i, sizeof(s));
		if (s.reserved || s.dir < 1 || s.dir > 3 || !s.size)
			return -MLG_LX_EINVAL;
		if (s.size > MLG_LX_MAX_SEGMENT_BYTES)
			return -MLG_LX_E2BIG;
		if (s.va < MLG_LX_VA_MIN || s.va >= MLG_LX_VA_LIMIT || s.va + s.size > MLG_LX_VA_LIMIT)
			return -MLG_LX_EFAULT;
		if (s.va < end)
			return -MLG_LX_EINVAL;
		if (s.dir & MLG_LX_SEG_IN) {
			if (s.data_offset != cursor || cursor + s.size > bytes)
				return -MLG_LX_EINVAL;
			cursor += s.size;
		} else if (s.data_offset) {
			return -MLG_LX_EINVAL;
		}
		end = s.va + s.size;
		if (s.dir & MLG_LX_SEG_OUT)
			out += s.size;
		if ((h.flags & 1) && (s.dir & MLG_LX_SEG_IN) && s.size >= 8 &&
		    h.timeout_va >= s.va && h.timeout_va + 8 <= s.va + s.size)
			timeout_ok = 1;
	}
	if (cursor != bytes)
		return -MLG_LX_EINVAL;
	if ((h.flags & 1) && !timeout_ok)
		return -MLG_LX_EINVAL;
	if (out > MLG_LX_MAX_FRAME_BYTES - sizeof(struct mlg_lx_reply))
		return -MLG_LX_E2BIG;
	*out_bytes = out;
	return 0;
}

static uint32_t random_spans(struct mlg_lx_span *spans, uint32_t cap)
{
	uint32_t n = 1 + (uint32_t)below(cap);

	for (uint32_t i = 0; i < n; ++i) {
		uint64_t size = 1 + below(below(4) ? 64 : 9000);
		uint64_t off = below(ARENA_BYTES - size);

		spans[i] = (struct mlg_lx_span){
			.va = (uint64_t)(uintptr_t)(arena + off), .size = size,
			.dir = 1 + (uint32_t)below(3),
		};
	}
	return n;
}

static void fuzz_check(uint8_t *frame, size_t len, uint32_t cmd)
{
	uint64_t a = 0, b = 0;
	int r1 = mlg_lx_frame_check(frame, len, cmd, &a);
	int r2 = reference_check(frame, len, cmd, &b);

	CHECK(r1 == r2);
	CHECK(r1 || a == b);
}

int main(void)
{
	const char *seed = getenv("LX_FUZZ_SEED"), *rounds_env = getenv("LX_FUZZ_ROUNDS");
	const unsigned long rounds = rounds_env ? strtoul(rounds_env, NULL, 0) : 40000;
	static struct mlg_lx_span spans[64];
	uint8_t *frame = malloc(1 << 20), *mutated = malloc(1 << 20), *shadow = malloc(ARENA_BYTES);
	uint8_t *rep = malloc(1 << 20);
	unsigned long accepted = 0, rejected = 0;

	rng_state = seed ? strtoull(seed, NULL, 0) : 0x9e3779b97f4a7c15ULL;
	arena = malloc(ARENA_BYTES);
	CHECK(frame && mutated && shadow && arena && rep);
	for (uint32_t i = 0; i < ARENA_BYTES; ++i)
		arena[i] = (uint8_t)rng();

	/* Fixed cases. */
	CHECK(mlg_lx_frame_check(NULL, 0, 0, NULL) == -MLG_LX_EINVAL);
	CHECK(mlg_lx_frame_check(frame, 10, 0, NULL) == -MLG_LX_EINVAL);
	CHECK(mlg_lx_frame_check(frame, MLG_LX_MAX_FRAME_BYTES + 1, 0, NULL) == -MLG_LX_E2BIG);
	{
		/* Overlapping spans merge, IN|OUT combine; touching ones stay
		 * apart; zero-length ones vanish. */
		struct mlg_lx_span s[4] = {
			{ (uint64_t)(uintptr_t)arena + 100, 50, MLG_LX_SEG_IN },
			{ (uint64_t)(uintptr_t)arena + 120, 80, MLG_LX_SEG_OUT },
			{ (uint64_t)(uintptr_t)arena + 200, 8, MLG_LX_SEG_IN },
			{ (uint64_t)(uintptr_t)arena + 300, 0, MLG_LX_SEG_IN },
		};
		uint64_t out = 0;
		long len = mlg_lx_encode(42, 7, s, 4, 0, 0, frame, 1 << 20, &out);
		struct mlg_lx_frame h;
		struct mlg_lx_segment seg[2];

		CHECK(len == 64 + 2 * 24 + 100 + 8);
		memcpy(&h, frame, sizeof(h));
		memcpy(seg, frame + 64, sizeof(seg));
		CHECK(h.nsegs == 2 && h.cmd == 42 && h.arg == 7 && out == 100);
		CHECK(seg[0].va == (uint64_t)(uintptr_t)arena + 100 && seg[0].size == 100 &&
		      seg[0].dir == MLG_LX_SEG_INOUT && seg[0].data_offset == 64 + 48);
		CHECK(seg[1].size == 8 && seg[1].dir == MLG_LX_SEG_IN);
		CHECK(!memcmp(frame + 64 + 48, arena + 100, 100));
		CHECK(!mlg_lx_frame_check(frame, (size_t)len, 42, &out) && out == 100);
		/* Size query, short buffer. */
		CHECK(mlg_lx_encode(42, 7, s, 4, 0, 0, NULL, 0, NULL) == len);
		CHECK(mlg_lx_encode(42, 7, s, 4, 0, 0, frame, (size_t)len - 1, NULL) == -MLG_LX_ENOSPC);
		/* Bad spans. */
		struct mlg_lx_span bad = { 16, 8, MLG_LX_SEG_IN };
		CHECK(mlg_lx_encode(1, 0, &bad, 1, 0, 0, frame, 1 << 20, NULL) == -MLG_LX_EFAULT);
		bad = (struct mlg_lx_span){ MLG_LX_VA_LIMIT - 4, 8, MLG_LX_SEG_IN };
		CHECK(mlg_lx_encode(1, 0, &bad, 1, 0, 0, frame, 1 << 20, NULL) == -MLG_LX_EFAULT);
		bad = (struct mlg_lx_span){ (uint64_t)(uintptr_t)arena, 8, 4 };
		CHECK(mlg_lx_encode(1, 0, &bad, 1, 0, 0, frame, 1 << 20, NULL) == -MLG_LX_EINVAL);
		bad = (struct mlg_lx_span){ (uint64_t)(uintptr_t)arena, MLG_LX_MAX_SEGMENT_BYTES + 1,
					    MLG_LX_SEG_OUT };
		CHECK(mlg_lx_encode(1, 0, &bad, 1, 0, 0, frame, 1 << 20, NULL) == -MLG_LX_E2BIG);
	}
	{
		/* Deadlines: finite ones become time left on the caller's clock;
		 * 0 (poll) and negative (forever) travel as they are; a deadline
		 * outside every IN span is refused. */
		int64_t *deadline = (int64_t *)(void *)(arena + 4096);
		struct mlg_lx_span s = { (uint64_t)(uintptr_t)deadline, 16, MLG_LX_SEG_INOUT };
		struct mlg_lx_frame h;
		long len;

		*deadline = 5000;
		len = mlg_lx_encode(9, 0, &s, 1, s.va, 1000, frame, 1 << 20, NULL);
		memcpy(&h, frame, sizeof(h));
		CHECK(len > 0 && (h.flags & MLG_LX_FRAME_TIMEOUT) && h.timeout_ns == 4000 &&
		      h.timeout_va == s.va);
		CHECK(!mlg_lx_frame_check(frame, (size_t)len, 9, NULL));
		*deadline = 500;	/* already passed */
		len = mlg_lx_encode(9, 0, &s, 1, s.va, 1000, frame, 1 << 20, NULL);
		memcpy(&h, frame, sizeof(h));
		CHECK(len > 0 && h.timeout_ns == 0 && (h.flags & MLG_LX_FRAME_TIMEOUT));
		*deadline = 0;
		len = mlg_lx_encode(9, 0, &s, 1, s.va, 1000, frame, 1 << 20, NULL);
		memcpy(&h, frame, sizeof(h));
		CHECK(len > 0 && !h.flags && !h.timeout_va);
		*deadline = -1;
		len = mlg_lx_encode(9, 0, &s, 1, s.va, 1000, frame, 1 << 20, NULL);
		memcpy(&h, frame, sizeof(h));
		CHECK(len > 0 && !h.flags);
		*deadline = 5000;
		CHECK(mlg_lx_encode(9, 0, &s, 1, s.va + 12, 1000, frame, 1 << 20, NULL) == -MLG_LX_EINVAL);
		s.dir = MLG_LX_SEG_OUT;
		CHECK(mlg_lx_encode(9, 0, &s, 1, s.va, 1000, frame, 1 << 20, NULL) == -MLG_LX_EINVAL);
	}

	for (round_no = 0; round_no < rounds; ++round_no) {
		uint32_t n = random_spans(spans, 1 + (uint32_t)below(64));
		uint32_t cmd = (uint32_t)rng();
		uint64_t out = 0, out2 = 0;
		long len = mlg_lx_encode(cmd, rng(), spans, n, 0, 0, frame, 1 << 20, &out);

		if (len < 0) {
			/* Only the limits refuse random spans inside the arena. */
			CHECK(len == -MLG_LX_E2BIG);
			continue;
		}
		CHECK(!mlg_lx_frame_check(frame, (size_t)len, cmd, &out2) && out2 == out);
		fuzz_check(frame, (size_t)len, cmd);

		/* A well-formed reply writes exactly the OUT segments. */
		{
			struct mlg_lx_reply rh = { MLG_LX_REPLY_MAGIC, MLG_LX_VERSION, sizeof(rh),
						   (uint32_t)mlg_lx_reply_bytes(out), 0, -5 };
			struct mlg_lx_frame h;
			int64_t result = 0;

			memcpy(&h, frame, sizeof(h));
			for (uint32_t i = 0; i < h.nsegs; ++i) {
				struct mlg_lx_segment s;
				memcpy(&s, frame + 64 + 24ull * i, sizeof(s));
				rh.out_segments += !!(s.dir & MLG_LX_SEG_OUT);
			}
			memcpy(rep, &rh, sizeof(rh));
			for (uint64_t i = 0; i < out; ++i)
				rep[sizeof(rh) + i] = (uint8_t)rng();
			memcpy(shadow, arena, ARENA_BYTES);
			CHECK(!mlg_lx_apply_reply(frame, (size_t)len, rep, mlg_lx_reply_bytes(out), &result));
			CHECK(result == -5);
			const uint8_t *src = rep + sizeof(rh);
			for (uint32_t i = 0; i < h.nsegs; ++i) {
				struct mlg_lx_segment s;
				memcpy(&s, frame + 64 + 24ull * i, sizeof(s));
				uint64_t off = s.va - (uint64_t)(uintptr_t)arena;
				if (s.dir & MLG_LX_SEG_OUT) {
					CHECK(!memcmp(arena + off, src, s.size));
					memcpy(shadow + off, src, s.size);
					src += s.size;
				}
			}
			CHECK(!memcmp(shadow, arena, ARENA_BYTES));
			/* Malformed replies: refused, nothing written. */
			rep[0] ^= 1;
			CHECK(mlg_lx_apply_reply(frame, (size_t)len, rep, mlg_lx_reply_bytes(out), NULL));
			rep[0] ^= 1;
			CHECK(mlg_lx_apply_reply(frame, (size_t)len, rep, mlg_lx_reply_bytes(out) + 1, NULL));
			if (out)
				CHECK(mlg_lx_apply_reply(frame, (size_t)len, rep, mlg_lx_reply_bytes(out) - 1,
							 NULL));
			CHECK(!memcmp(shadow, arena, ARENA_BYTES));
		}

		/* Mutations: header and table fields, the payload, truncation. */
		for (int m = 0; m < 8; ++m) {
			size_t mlen = (size_t)len;

			memcpy(mutated, frame, (size_t)len);
			switch (below(6)) {
			case 0: {	/* flip bits anywhere in header + table */
				size_t span = 64 + 24 * (size_t)n;
				if (span > (size_t)len) span = (size_t)len;
				for (int k = 1 + (int)below(3); k; --k)
					mutated[below(span)] ^= (uint8_t)(1u << below(8));
				break;
			}
			case 1: {	/* overwrite one table word with an edge value */
				static const uint64_t edges[] = { 0, 1, 7, 8, 0xffffffff, 0x100000000ull,
					MLG_LX_VA_LIMIT - 1, MLG_LX_VA_LIMIT, ~0ull, MLG_LX_MAX_SEGMENT_BYTES,
					MLG_LX_MAX_SEGMENT_BYTES + 1ull, 64 };
				size_t at = 8 + 4 * below((64 + 24 * (size_t)n - 8) / 4);
				uint32_t v = (uint32_t)edges[below(sizeof(edges) / sizeof(edges[0]))];
				if (at + 4 <= (size_t)len)
					memcpy(mutated + at, &v, 4);
				break;
			}
			case 2:		/* truncate */
				mlen = below((size_t)len);
				break;
			case 3:		/* extend with garbage (claims stay) */
				mlen = (size_t)len + 1 + below(64);
				for (size_t k = (size_t)len; k < mlen; ++k)
					mutated[k] = (uint8_t)rng();
				break;
			case 4: {	/* set the timeout flag at a random place */
				struct mlg_lx_frame h;
				memcpy(&h, mutated, sizeof(h));
				h.flags = MLG_LX_FRAME_TIMEOUT;
				h.timeout_va = below(2) ? spans[below(n)].va + below(16) : rng();
				h.timeout_ns = rng();
				memcpy(mutated, &h, sizeof(h));
				break;
			}
			default: {	/* nsegs */
				struct mlg_lx_frame h;
				memcpy(&h, mutated, sizeof(h));
				h.nsegs = below(2) ? (uint32_t)rng() : h.nsegs + (uint32_t)below(3) - 1;
				memcpy(mutated, &h, sizeof(h));
				break;
			}
			}
			if (!mlg_lx_frame_check(mutated, mlen, cmd, NULL))
				accepted++;
			else
				rejected++;
			fuzz_check(mutated, mlen, cmd);
			/* A checked frame decodes a reply safely too. */
			if (!mlg_lx_frame_check(mutated, mlen, cmd, &out2)) {
				struct mlg_lx_reply rh = { MLG_LX_REPLY_MAGIC, MLG_LX_VERSION, sizeof(rh),
							   (uint32_t)mlg_lx_reply_bytes(out2), 0, 0 };
				struct mlg_lx_frame h;
				memcpy(&h, mutated, sizeof(h));
				for (uint32_t i = 0; i < h.nsegs; ++i) {
					struct mlg_lx_segment s;
					memcpy(&s, mutated + 64 + 24ull * i, sizeof(s));
					rh.out_segments += !!(s.dir & MLG_LX_SEG_OUT);
				}
				/* Only segments still inside the arena may be written. */
				int inside = 1;
				for (uint32_t i = 0; i < h.nsegs; ++i) {
					struct mlg_lx_segment s;
					memcpy(&s, mutated + 64 + 24ull * i, sizeof(s));
					if ((s.dir & MLG_LX_SEG_OUT) &&
					    (s.va < (uint64_t)(uintptr_t)arena ||
					     s.va + s.size > (uint64_t)(uintptr_t)arena + ARENA_BYTES))
						inside = 0;
				}
				if (inside && mlg_lx_reply_bytes(out2) <= (1u << 20)) {
					memset(rep + sizeof(rh), 0x5a, out2);
					memcpy(rep, &rh, sizeof(rh));
					CHECK(!mlg_lx_apply_reply(mutated, mlen, rep, mlg_lx_reply_bytes(out2),
								  NULL));
				}
			}
		}
	}
	CHECK(accepted && rejected);
	printf("PASS lx frame fuzz: %lu rounds, %lu mutated frames accepted, %lu rejected, "
	       "checker == reference, replies write only OUT segments\n",
	       rounds, accepted, rejected);
	free(frame);
	free(mutated);
	free(shadow);
	free(rep);
	free(arena);
	return 0;
}
