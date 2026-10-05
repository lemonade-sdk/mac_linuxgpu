/* host/owner_call.h's wait, against a driver that answers Ping but never
 * completes the call (a pre-243 driver runs an async call synchronously
 * and sends no completion): unbounded it would wait forever; with a
 * timeout it returns kIOReturnTimeout on time, and a driver that stops
 * answering Ping is kIOReturnNotAttached. The IOKit calls the header makes
 * on the connection are replaced; the notification port is real. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

static int async_calls, pings;
static int ping_answers = 1;

static kern_return_t mock_async(mach_port_t c, uint32_t sel, mach_port_t wake, uint64_t *ref,
				uint32_t nref, const uint64_t *in, uint32_t nin, const void *is,
				size_t isz, uint64_t *out, uint32_t *nout, void *os, size_t *osz)
{
	(void)c; (void)sel; (void)wake; (void)ref; (void)nref; (void)in; (void)nin;
	(void)is; (void)isz; (void)out; (void)os; (void)osz;
	++async_calls;
	if (nout)
		*nout = 0;
	return kIOReturnSuccess; /* ran "synchronously"; no completion will come */
}

static kern_return_t mock_scalar(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t nin,
				 uint64_t *out, uint32_t *nout)
{
	(void)c; (void)in; (void)nin;
	if (sel != 0)
		return kIOReturnUnsupported;
	++pings;
	if (!ping_answers)
		return kIOReturnNotAttached;
	out[0] = 0xA117AB1Eu;
	*nout = 1;
	return kIOReturnSuccess;
}

#define MLG_OWNER_CALL_BOUND_MS 1500u
#define MLG_OWNER_CALL_LONG_BOUND_MS 3000u
#define IOConnectCallAsyncMethod mock_async
#define IOConnectCallScalarMethod mock_scalar
#include "owner_call.h"

static uint64_t now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull; }

int main(void)
{
	uint64_t in[3] = {0, 0, 0x52455452}, out[3];
	uint32_t n = 3;
	uint64_t start = now_ms();
	kern_return_t kr;

	alarm(25); /* a wait that ignores its timeout fails here, not hangs */
	kr = mlg_owner_call_timed(1, 2500, 85, in, 3, NULL, 0, out, &n, NULL, NULL);
	uint64_t took = now_ms() - start;

	if (kr != kIOReturnTimeout || took < 2500 || took > 3500 || async_calls != 1 || pings < 2) {
		fprintf(stderr, "FAIL timed owner call: kr=%#x after %llu ms, %d calls, %d pings\n", kr,
			(unsigned long long)took, async_calls, pings);
		return 1;
	}
	/* The plain call has a bound too: a completion that never comes
	 * (a selector the driver serves synchronously, sent async) fails
	 * loudly within it, the longer bound for bringing the GPU up. */
	start = now_ms();
	kr = mlg_owner_call(1, 102, in, 2, NULL, 0, out, &n, NULL, NULL);
	took = now_ms() - start;
	if (kr != kIOReturnTimeout || took < 1500 || took > 2500) {
		fprintf(stderr, "FAIL bounded owner call: kr=%#x after %llu ms\n", kr, (unsigned long long)took);
		return 1;
	}
	start = now_ms();
	kr = mlg_owner_call(1, 9, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
	took = now_ms() - start;
	if (kr != kIOReturnTimeout || took < 3000 || took > 4000) {
		fprintf(stderr, "FAIL bounded InitDevice call: kr=%#x after %llu ms\n", kr, (unsigned long long)took);
		return 1;
	}
	ping_answers = 0;
	start = now_ms();
	kr = mlg_owner_call_timed(1, 0, 85, in, 3, NULL, 0, out, &n, NULL, NULL);
	took = now_ms() - start;
	if (kr != kIOReturnNotAttached || took > 1500) {
		fprintf(stderr, "FAIL owner call to a driver gone: kr=%#x after %llu ms\n", kr,
			(unsigned long long)took);
		return 1;
	}
	printf("PASS owner call (host): a completion that never comes times out on time "
	       "(kIOReturnTimeout after %d pings), the plain call within its per-selector bound, loudly; "
	       "a driver that stops answering Ping ends the wait\n", pings);
	return 0;
}
