#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
from pathlib import Path
import re, sys
out = Path(sys.argv[1])
source = Path('third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c').read_text()
start = source.index('\tadev->pm_nb.notifier_call = amdgpu_device_pm_notifier;',
                     source.index('int amdgpu_device_init('))
end = source.index('\n\treturn 0;', start) + len('\n\treturn 0;')
tail = source[start:end]
assert 'r = register_pm_notifier(&adev->pm_nb);' in tail
assert 'if (r)\n\t\tgoto failed;' in tail
out.joinpath('amdgpu_pm_init_tail.inc').write_text(tail + '\n')
generated = Path('linuxu/src/shims/kernel_api_stubs.c').read_text()
assert not re.search(r'\b(?:un)?register_pm_notifier\s*\(', generated), \
    'generated stubs must not override the CONFIG_PM_SLEEP contract'
# Compare the public event numbers against the pinned Linux header.
for name in ('PM_HIBERNATION_PREPARE', 'PM_POST_HIBERNATION',
             'PM_SUSPEND_PREPARE', 'PM_POST_SUSPEND',
             'PM_RESTORE_PREPARE', 'PM_POST_RESTORE'):
    pattern = rf'^#define\s+{name}\s+(\w+)'
    values = [int(re.search(pattern, Path(p).read_text(), re.M)[1], 0)
              for p in ('third_party/linux/include/linux/suspend.h',
                        'linuxu/headers/linux/suspend.h')]
    assert values[0] == values[1], (name, values)
PY
common=(-std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
        -Wno-macro-redefined
        -Werror=implicit-function-declaration -Werror=incompatible-pointer-types
        -include linux/autoconf.h -Ilinuxu/headers -I"$work")
clang "${common[@]}" linuxu/tests/test_pm_notifier.c -o "$work/fixed"
"$work/fixed"
clang "${common[@]}" -DREPRODUCE_OLD_PM_NOTIFIER_STUB \
    linuxu/tests/test_pm_notifier.c -o "$work/legacy"
"$work/legacy"
# Enabling Linux sleep must require a real notifier service, not inline success.
cat > "$work/enabled.c" <<'EOF'
#include <linux/suspend.h>
int register_enabled(struct notifier_block *nb) { return register_pm_notifier(nb); }
int unregister_enabled(struct notifier_block *nb) { return unregister_pm_notifier(nb); }
EOF
clang "${common[@]}" -DCONFIG_PM_SLEEP=1 -c "$work/enabled.c" -o "$work/enabled.o"
nm -u "$work/enabled.o" > "$work/undefined.txt"
grep -Eq '(^| )_register_pm_notifier$' "$work/undefined.txt"
grep -Eq '(^| )_unregister_pm_notifier$' "$work/undefined.txt"
