#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -D__KERNEL__ -include linux/autoconf.h -include linux/compiler.h -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_primitive_headers.c linuxu/src/shims/printk.c linuxu/src/shims/dev_coredump.c \
  third_party/linux/lib/sort.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_primitive_headers"
"$test_dir/test_primitive_headers"
cat > "$test_dir/assertion.c" <<'SRC'
#include <linux/build_bug.h>
void forbidden(void) {
  BUILD_BUG_ON_MSG(1, "first invalid contract");
  BUILD_BUG_ON_MSG(1, "second invalid contract");
}
SRC
if clang -w -std=gnu11 -O2 -Ilinuxu/headers -c "$test_dir/assertion.c" \
    -o "$test_dir/assertion.o" > "$test_dir/assertion.log" 2>&1; then
  echo 'BUILD_BUG_ON failed to reject an invalid compile-time contract' >&2
  exit 1
fi
grep -q 'first invalid contract' "$test_dir/assertion.log"
grep -q 'second invalid contract' "$test_dir/assertion.log"
cat > "$test_dir/folded.c" <<'SRC'
#include <linux/build_bug.h>
void valid(void) {
  size_t from = sizeof(unsigned long);
  size_t to = sizeof(unsigned long);
  BUILD_BUG_ON(from != to);
  BUILD_BUG_ON_MSG(sizeof(char) != 1, "valid character size");
}
SRC
clang -w -std=gnu11 -O2 -Ilinuxu/headers -c "$test_dir/folded.c" -o "$test_dir/folded.o"
# Include order must not change Linux byte units or enable debug config.
for first in kernel sizes; do
  if [ "$first" = kernel ]; then second=sizes; else second=kernel; fi
  cat > "$test_dir/size-contract.c" <<SRC
#include <linux/$first.h>
#include <linux/$second.h>
#include <linux/lockdep.h>
#ifdef CONFIG_LOCKDEP
#error header must not enable CONFIG_LOCKDEP
#endif
_Static_assert(SZ_1K == 1024 && SZ_4K == 4096, "kilobyte sizes");
_Static_assert(SZ_64K == 65536 && SZ_1M == 1048576, "megabyte sizes");
_Static_assert(SZ_1G == 1073741824 && SZ_4G == 4294967296ULL, "gigabyte sizes");
_Static_assert(SZ_1T == 1099511627776ULL, "terabyte sizes");
void no_debug_map(void) {
  lock_acquire_shared_recursive(&disabled_config_symbol, 0, 1, NULL, NULL);
  lock_release(&disabled_config_symbol, NULL);
}
SRC
  clang -w -std=gnu11 -O2 -Ilinuxu/headers -c "$test_dir/size-contract.c" -o "$test_dir/size-contract.o"
done
# Verify emitted ordering instructions; a host stress loop alone cannot prove
# that an ARM publication barrier exists or that include order preserves it.
for first in compiler list; do
  if [ "$first" = compiler ]; then second=list; else second=compiler; fi
  cat > "$test_dir/barriers.c" <<SRC
#include <linux/$first.h>
#include <linux/$second.h>
#include <asm/barrier.h>
#include <linux/smp.h>
void publish(void **slot, void *value) { smp_store_release(slot, value); }
void *consume(void **slot) { return smp_load_acquire(slot); }
void full_device(void) { mb(); }
void read_device(void) { rmb(); }
void write_device(void) { wmb(); }
void full_cpu(void) { smp_mb(); }
void read_cpu(void) { smp_rmb(); }
void write_cpu(void) { smp_wmb(); }
void before_atomic(void) { smp_mb__before_atomic(); }
void after_atomic(void) { smp_mb__after_atomic(); }
void store_full(int *slot, int value) { smp_store_mb(*slot, value); }
SRC
  clang -w -arch arm64 -std=gnu11 -O2 -Ilinuxu/headers -S \
    "$test_dir/barriers.c" -o "$test_dir/barriers.s"
  python3 - "$test_dir/barriers.s" <<'PY'
import pathlib, re, sys
assembly = pathlib.Path(sys.argv[1]).read_text()
def body(name):
    return assembly.split('_' + name + ':', 1)[1].split('ret', 1)[0]
for name, instruction in {
    'publish': r'\bstlr\b', 'consume': r'\blda(?:p)?r\b',
    'full_device': r'\bdmb\s+osh\b', 'read_device': r'\bdmb\s+oshld\b',
    'write_device': r'\bdmb\s+oshst\b', 'full_cpu': r'\bdmb\s+ish\b',
    'read_cpu': r'\bdmb\s+ishld\b', 'write_cpu': r'\bdmb\s+ish\b',
    'before_atomic': r'\bdmb\s+ish\b', 'after_atomic': r'\bdmb\s+ish\b',
}.items():
    assert re.search(instruction, body(name)), (name, body(name))
assert re.search(r'\bstr\b[\s\S]*\bdmb\s+ish\b', body('store_full'))
print('ARM64 host publication and device barriers: passed')
PY
done
