/* Port ABI contracts exercised without opening a device. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <asm/ioctl.h>
#include <asm/unaligned.h>
#include <uapi/linux/kfd_ioctl.h>
#include <drm/amdgpu_drm.h>
#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/kstrtox.h>
#include <linux/devcoredump.h>
#include <linux/sort.h>
#include <linux/bitops.h>
#include <linux/nospec.h>

static void ioctl_contracts(void)
{
	for (unsigned direction = 0; direction < 4; ++direction)
		for (unsigned size = 0; size <= _IOC_SIZEMASK; ++size) {
			unsigned command = _IOC(direction, 0xab, 0x53, size);
			assert(_IOC_DIR(command) == direction);
			assert(_IOC_SIZE(command) == size);
			assert(_IOC_TYPE(command) == 0xab);
			assert(_IOC_NR(command) == 0x53);
		}
	assert(AMDKFD_IOC_GET_VERSION == 0x80084b01u);
	assert(_IOC_SIZE(AMDKFD_IOC_GET_VERSION) == sizeof(struct kfd_ioctl_get_version_args));
	assert((AMDKFD_IOC_GET_VERSION & IOC_OUT) && !(AMDKFD_IOC_GET_VERSION & IOC_IN));
	assert(_IOC_SIZE(AMDKFD_IOC_CREATE_QUEUE) == sizeof(struct kfd_ioctl_create_queue_args));
	assert((AMDKFD_IOC_CREATE_QUEUE & IOC_IN) && (AMDKFD_IOC_CREATE_QUEUE & IOC_OUT));
	assert(_IOC_TYPE(DRM_IOCTL_AMDGPU_GEM_CREATE) == 'd');
	assert(_IOC_SIZE(DRM_IOCTL_AMDGPU_GEM_CREATE) == sizeof(union drm_amdgpu_gem_create));
	assert(_IOC_DIR(DRM_IOCTL_AMDGPU_GEM_CREATE) == (_IOC_READ | _IOC_WRITE));
}

static void unaligned_contracts(void)
{
	unsigned char bytes[16];
	const unsigned char network[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
	memset(bytes, 0xa5, sizeof(bytes));
	memcpy(bytes + 1, network, sizeof(network));
	assert(get_unaligned_be16(bytes + 1) == 0x1234);
	assert(get_unaligned_be32(bytes + 1) == 0x12345678);
	assert(get_unaligned_be64(bytes + 1) == 0x123456789abcdef0ULL);
	put_unaligned_be16(0x1234, bytes + 1);
	put_unaligned_be32(0x12345678, bytes + 1);
	put_unaligned_be64(0x123456789abcdef0ULL, bytes + 1);
	assert(!memcmp(bytes + 1, network, sizeof(network)));
	assert(bytes[0] == 0xa5 && bytes[9] == 0xa5);
	put_unaligned(0xfedcba9876543210ULL, (u64 *)(bytes + 1));
	assert(get_unaligned((const u64 *)(bytes + 1)) == 0xfedcba9876543210ULL);
	put_unaligned(0x12345678, (u32 *)(bytes + 1));
	assert(get_unaligned((const u32 *)(bytes + 1)) == 0x12345678);
	u8 *cursor = bytes + 1;
	put_unaligned(0x5a, cursor++);
	assert(cursor == bytes + 2 && bytes[1] == 0x5a);
	cursor = bytes + 1;
	assert(get_unaligned(cursor++) == 0x5a && cursor == bytes + 2);
}


static unsigned dump_frees;
static int dump_payload;
static void release_dump(void *data)
{
	assert(data == &dump_payload);
	++dump_frees;
}
static int descending(const void *left, const void *right)
{
	return cmp_int(*(const unsigned *)right, *(const unsigned *)left);
}
static unsigned custom_swaps;
static void swap_item(void *left, void *right, int size)
{
	assert(size == sizeof(unsigned));
	unsigned temp = *(unsigned *)left;
	*(unsigned *)left = *(unsigned *)right;
	*(unsigned *)right = temp;
	++custom_swaps;
}
static void peripheral_contracts(void)
{
	struct { int value; unsigned guard; } signed_out = {0, 0xaabbccdd};
	struct { unsigned value, guard; } unsigned_out = {0, 0xaabbccdd};
	assert(!kstrtoint("-2147483648", 10, &signed_out.value));
	assert(signed_out.value == INT_MIN && signed_out.guard == 0xaabbccdd);
	assert(kstrtoint("2147483648", 10, &signed_out.value) == -ERANGE);
	assert(signed_out.value == INT_MIN && signed_out.guard == 0xaabbccdd);
	assert(!kstrtou32("4294967295\n", 10, &unsigned_out.value));
	assert(unsigned_out.value == UINT_MAX && unsigned_out.guard == 0xaabbccdd);
	assert(kstrtouint("4294967296", 10, &unsigned_out.value) == -ERANGE);
	assert(unsigned_out.value == UINT_MAX && unsigned_out.guard == 0xaabbccdd);
	unsigned long long u = 7;
	long long v = 7;
	assert(!kstrtoull("18446744073709551615", 10, &u) && u == ULLONG_MAX);
	assert(kstrtoull("18446744073709551616", 10, &u) == -ERANGE && u == ULLONG_MAX);
	assert(!kstrtoll("-9223372036854775808", 10, &v) && v == LLONG_MIN);
	assert(kstrtoll("-9223372036854775809", 10, &v) == -ERANGE && v == LLONG_MIN);
	assert(kstrtoll("9223372036854775808", 10, &v) == -ERANGE);
	assert(!kstrtoull("+0x1f\n", 0, &u) && u == 31);
	assert(!kstrtoull("077", 0, &u) && u == 63);
	assert(!kstrtoull("0x20", 16, &u) && u == 32);
	assert(!kstrtoull("z", 36, &u) && u == 35);
	const char *invalid[] = {"-1", "1x", " 1", "1 ", "1\n\n", "", "+", "0x", "09"};
	for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
		u = 37;
		assert(kstrtoull(invalid[i], 0, &u) == -EINVAL && u == 37);
	}
	assert(kstrtoull("1", 1, &u) == -EINVAL);
	assert(kstrtoull("1", 37, &u) == -EINVAL);
	long signed_user = 19;
	unsigned long unsigned_user = 23;
	assert(kstrtol_from_user((void *)0x12345, 8, 10, &signed_user) == -EFAULT);
	assert(kstrtoul_from_user((void *)0x12345, 8, 10, &unsigned_user) == -EFAULT);
	assert(kstrtouint_from_user((void *)0x12345, 8, 10, &unsigned_out.value) == -EFAULT);
	assert(signed_user == 19 && unsigned_user == 23);

	struct trailing { unsigned prefix; unsigned long long elements[]; } *p = NULL;
	assert(struct_size(p, elements, 3) == sizeof(*p) + 24);
	assert(array_size(SIZE_MAX, 2) == SIZE_MAX);
	assert(flex_array_size(p, elements, SIZE_MAX / 8 + 1) == SIZE_MAX);
	assert(struct_size(p, elements, SIZE_MAX / 8) == SIZE_MAX);
	size_t count = 3;
	assert(struct_size(p, elements, count++) == sizeof(*p) + 24 && count == 4);
	struct offsets { char leading[7]; char middle[5]; char trailing[3]; };
	BUILD_BUG_ON(offsetofend(struct offsets, middle) != 12);
	BUILD_BUG_ON(offsetofend(struct offsets, trailing) != 15);
	assert(min_t(unsigned long, 3U, 100ULL) == 3);
	assert(min_t(unsigned long, 100U, 3ULL) == 3);

	dev_coredumpm(NULL, NULL, &dump_payload, 0, 0, NULL, release_dump);
	assert(dump_frees == 1);
	unsigned items[] = {2, 7, 1, 7, 0, 4};
	sort(items, sizeof(items)/sizeof(items[0]), sizeof(items[0]), descending, NULL);
	for (unsigned i = 1; i < sizeof(items)/sizeof(items[0]); ++i) assert(items[i-1] >= items[i]);
	unsigned more[] = {1, 2, 3, 4};
	sort(more, 4, sizeof(more[0]), descending, swap_item);
	assert(custom_swaps && more[0] == 4 && more[3] == 1);
	sort(NULL, 0, sizeof(unsigned), descending, NULL);
}

int main(void)
{
	peripheral_contracts();
	ioctl_contracts();
	unaligned_contracts();
	/* Upstream FIELD_GET() takes compile-time constant masks only. */
#define FIELD_GET_BIT(shift) assert(FIELD_GET(1ULL << (shift), ~0ULL) == 1)
	FIELD_GET_BIT(0); FIELD_GET_BIT(1); FIELD_GET_BIT(7); FIELD_GET_BIT(15);
	FIELD_GET_BIT(31); FIELD_GET_BIT(32); FIELD_GET_BIT(47); FIELD_GET_BIT(63);
#undef FIELD_GET_BIT
	assert(FIELD_GET(GENMASK(15, 8), 0x12345678ULL) == 0x56);
	assert(FIELD_GET(GENMASK_ULL(63, 32), 0x123456789abcdef0ULL) == 0x12345678);
	puts("Primitive header ABI: ioctl field decoding/directions and typed endian access passed");
}
