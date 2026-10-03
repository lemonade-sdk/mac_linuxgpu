#!/usr/bin/env bash
# Fail-safe stub policy regression:
#  1. scripts/stub_policy.py classifies every symbol and the checked-in
#     kernel_api_stubs.c matches what gen_stubs.py generates from it;
#  2. gen_stubs.py refuses unclassified symbols, `implement` entries, stale
#     entries and negative errnos through unsigned/bool/pointer returns;
#  3. no generated stub that upstream code names from amdgpu_device_init,
#     amdgpu_driver_open_kms or kfd_create_process returns a negative value,
#     and no generated stub anywhere returns -ENOSYS through an unsigned,
#     enum, bool or pointer type;
#  4. the services that replaced former stubs keep their Linux contracts
#     (linuxu/tests/test_stub_contracts.c, linked against build/libmacamgdu.a).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

python3 scripts/gen_stubs.py --check

expect_fail() {
	local label=$1 pattern=$2
	shift 2
	if python3 scripts/gen_stubs.py "$@" --output "$work/out.c" \
			>"$work/log" 2>&1; then
		echo "FAIL: gen_stubs.py accepted $label" >&2
		cat "$work/log" >&2
		exit 1
	fi
	if ! grep -q -- "$pattern" "$work/log"; then
		echo "FAIL: gen_stubs.py rejected $label without naming it" >&2
		cat "$work/log" >&2
		exit 1
	fi
}

# Unclassified symbol.
cp linuxu/UNDEFINED_SYMBOLS.txt "$work/syms.txt"
echo linuxu_unclassified_probe_symbol >> "$work/syms.txt"
expect_fail "an unclassified symbol" "unclassified symbols.*linuxu_unclassified_probe_symbol" \
	--symbols "$work/syms.txt"

# Derived policies: implement, stale entry, enosys/negative through unsigned.
write_policy() {
	cat > "$work/policy.py" <<EOF
import importlib.util
_spec = importlib.util.spec_from_file_location("base", "scripts/stub_policy.py")
_base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_base)
STUB_INCLUDES = _base.STUB_INCLUDES
POLICY = dict(_base.POLICY)
$1
EOF
}
write_policy 'POLICY["hrtimer_setup"] = ("implement", "probe")'
expect_fail "an implement entry" "hrtimer_setup: classified .implement." --policy "$work/policy.py"
write_policy 'POLICY["linuxu_stale_probe_symbol"] = ("noop", "probe")'
expect_fail "a stale policy entry" "not in UNDEFINED_SYMBOLS.txt: linuxu_stale_probe_symbol" \
	--policy "$work/policy.py"
write_policy 'POLICY["hrtimer_forward"] = ("enosys", "probe")'
expect_fail "-ENOSYS through u64" "hrtimer_forward: .enosys. needs a signed" --policy "$work/policy.py"
write_policy 'POLICY["hrtimer_active"] = {"cat": "unreachable", "ret": "-ENODEV"}'
expect_fail "a negative bool" "hrtimer_active: negative value" --policy "$work/policy.py"
write_policy 'POLICY["drm_gem_fb_get_obj"] = {"cat": "unreachable", "ret": "ERR_PTR(-ENOSYS)"}'
expect_fail "an ERR_PTR through a pointer stub" "drm_gem_fb_get_obj: negative value" \
	--policy "$work/policy.py"
write_policy 'POLICY["ndelay"] = ("noop", "probe")'
expect_fail "a stub shadowing a real definition" "ndelay: classified noop but already defined" \
	--policy "$work/policy.py"
write_policy 'POLICY["hrtimer_cancel"] = ("noop", "probe")'
expect_fail "noop on an int function" "hrtimer_cancel: .noop. requires a void" --policy "$work/policy.py"

# Reachability: no generated stub reached from the probe/open/KFD roots
# returns a negative value.
python3 scripts/stub_reach.py > "$work/reach.txt"
python3 - "$work/reach.txt" <<'PY'
import re, sys
sys.path.insert(0, "scripts")
import stub_reach as r
import gen_stubs as g

text = open(r.GENERATED).read()
code, _ = r.split_preprocessor(r.strip_comments_and_strings(text))
bodies = dict(r.top_level_definitions(code))

def negative(body):
    return re.search(r"\breturn\s*\(?\s*-", body) or \
        re.search(r"ERR_PTR\s*\(\s*-", body)

reached = {}
root = None
for line in open(sys.argv[1]):
    if line.startswith("root "):
        root = line.split()[1].rstrip(":")
    elif line.startswith("  "):
        reached.setdefault(line.split()[0], set()).add(root)
bad = [(s, sorted(rs)) for s, rs in sorted(reached.items())
       if s in bodies and negative(bodies[s])]
for s, rs in bad:
    print(f"FAIL: {s} is reachable from {', '.join(rs)} and returns a negative value",
          file=sys.stderr)

# Whole-file check: return type kind versus body.
types_bad = []
for m in re.finditer(r"(?m)^([A-Za-z_][\w \t\*]*?)\b(\w+)\(([^;{}]*)\)\n\{\n(.*?)\n\}",
                     text, re.S):
    rt, name, body = m.group(1).strip(), m.group(2), m.group(4)
    kind = g.return_kind(rt)
    if kind in ("unsigned", "enum", "bool", "pointer") and negative(body):
        if name == "vm_mmap" and "unsigned long" == " ".join(rt.split()):
            continue  # IS_ERR_VALUE() convention, declared err_encoded
        types_bad.append(name)
    if "ENOSYS" in body and kind != "signed":
        types_bad.append(name)
for s in types_bad:
    print(f"FAIL: {s} returns a negative value through an unsigned/enum/bool/pointer type",
          file=sys.stderr)
if bad or types_bad:
    sys.exit(1)
n = sum(1 for _ in reached)
print(f"stub reachability: {n} generated stubs named from the probe, open and "
      f"KFD roots; none returns a negative value")
PY

# The upstream userq objects must be in the build, not stubbed.
if grep -qw 'amdgpu_userq\|amdgpu_userq_fence' <(sed -n '/^EXCLUDED_FILES/,/^[^\t ]/p' mk/kernel_config.mk); then
	echo "FAIL: amdgpu_userq/amdgpu_userq_fence are excluded from the build" >&2
	exit 1
fi
if grep -Eq '^\s*(int|void|bool|u32) amdgpu_userq_' linuxu/src/shims/kernel_api_stubs.c; then
	echo "FAIL: generated stubs still define amdgpu_userq_* symbols" >&2
	exit 1
fi

# Runtime contracts against the real objects.
test -f build/libmacamgdu.a || {
	echo 'build/libmacamgdu.a missing; run make lib first' >&2
	exit 1
}
: "${HOSTCFLAGS:=-std=gnu11 -D__KERNEL__ -DCONFIG_DRM_FBDEV_OVERALLOC=0 -DLINUXU_RT_HOST_SHADOW=1 -include linux/autoconf.h -w}"
: "${INCPATHS:=-Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/amd/include/asic_reg -Ithird_party/linux/drivers/gpu/drm/amd/include -Ithird_party/linux/drivers/gpu/drm/amd/amdgpu -Ithird_party/linux/drivers/gpu/drm/amd/pm/inc -Ithird_party/linux/drivers/gpu/drm/amd/amdkfd -Ithird_party/linux/drivers/gpu/drm/amd/ras/ras_mgr -Ithird_party/linux/drivers/gpu/drm/amd/ras/rascore -Ithird_party/linux/drivers/gpu/drm/ttm -Ithird_party/linux/drivers/gpu/drm/scheduler -Ithird_party/linux/drivers/gpu/drm}"
# shellcheck disable=SC2086
clang $HOSTCFLAGS $INCPATHS linuxu/tests/test_stub_contracts.c \
	build/libmacamgdu.a -lpthread -o "$work/test_stub_contracts"
"$work/test_stub_contracts"
