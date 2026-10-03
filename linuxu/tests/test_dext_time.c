#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

#define LINUXU_TEST_DEXT_TIME 1
#include "../src/shims/dext_time.c"

static uint64_t fake_ns = 2500000000ULL;
static uint64_t slept_ms;
static uint64_t delayed_us;
static int fake_errno;

uint64_t linuxu_test_clock_ns(clockid_t clock_id)
{
	(void)clock_id;
	return fake_ns;
}

void linuxu_test_sleep_ms(uint64_t milliseconds)
{
	slept_ms += milliseconds;
}

void linuxu_test_delay_us(uint64_t microseconds)
{
	delayed_us += microseconds;
}

int *linuxu_test_error(void)
{
	return &fake_errno;
}

int main(void)
{
	struct timespec value = {0};
	assert(linuxu_test_clock_gettime(CLOCK_MONOTONIC, &value) == 0);
	assert(value.tv_sec == 2 && value.tv_nsec == 500000000);
	assert(linuxu_test_clock_gettime(-1, &value) == -1 && fake_errno == EINVAL);
	assert(linuxu_test_clock_gettime(CLOCK_MONOTONIC, NULL) == -1 &&
	       fake_errno == EINVAL);
	fake_ns = 0;
	assert(linuxu_test_clock_gettime(CLOCK_MONOTONIC, &value) == -1 &&
	       fake_errno == EIO);

	struct timespec request = { .tv_sec = 1, .tv_nsec = 2003004 };
	struct timespec remainder = { .tv_sec = 9, .tv_nsec = 9 };
	assert(linuxu_test_nanosleep(&request, &remainder) == 0);
	assert(slept_ms == 1002 && delayed_us == 4);
	assert(remainder.tv_sec == 0 && remainder.tv_nsec == 0);
	request.tv_nsec = 1000000000;
	assert(linuxu_test_nanosleep(&request, NULL) == -1 && fake_errno == EINVAL);
	request.tv_sec = (time_t)(UINT64_MAX / 1000ULL);
	request.tv_nsec = 999999999;
	assert(linuxu_test_nanosleep(&request, NULL) == -1 && fake_errno == EOVERFLOW);
	assert(slept_ms == 1002 && delayed_us == 4);
	return 0;
}
