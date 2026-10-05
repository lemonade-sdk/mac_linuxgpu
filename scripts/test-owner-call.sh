#!/usr/bin/env bash
# The dext's session calls (dext/sources/MacLinuxGPUXcode.mm owner_call):
# a call that can sleep never runs on the delivery thread, with the
# production functions extracted and DriverKit mocked
# (linuxu/tests/test_owner_call.cpp). Also checks that host/owner_call.h
# carries session_state.h's numbers.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 - "$test_dir" <<'PY'
import pathlib, re, sys
source = pathlib.Path("dext/sources/MacLinuxGPUXcode.mm").read_text()
start = "// ----------------------------------------------------------------\n// Session calls off the delivery thread"
end = "// A client's last display op result (MacLinuxGPUUserClient_IVars::displayResults)."
if source.count(start) != 1 or source.count(end) != 1:
    raise SystemExit("owner call extraction markers changed")
text = source[source.index(start):source.index(end)]
if len(text.splitlines()) > 480:
    raise SystemExit("owner call extraction exceeded its bounded section")
pathlib.Path(sys.argv[1], "owner_call_production.inc").write_text(text)
state = pathlib.Path("dext/sources/session_state.h").read_text()
client = pathlib.Path("host/owner_call.h").read_text()
def number(text, name):
    found = re.search(r"#define\s+" + name + r"\s+\(?(\w+)", text)
    if not found:
        raise SystemExit(f"{name} not found")
    return found.group(1).rstrip("u")
for driver, mirror in (("MLG_SELECTOR_OWNER_RESULT", "MLG_OWNER_CALL_SELECTOR_RESULT"),
                       ("MLG_OWNER_ASYNC_HEADER", "MLG_OWNER_CALL_HEADER"),
                       ("MLG_OWNER_ASYNC_SCALARS", "MLG_OWNER_CALL_SCALARS"),
                       ("MLG_OWNER_ASYNC_MAX_SCALARS", "MLG_OWNER_CALL_MAX_SCALARS")):
    if number(state, driver) != number(client, mirror):
        raise SystemExit(f"host/owner_call.h {mirror} differs from session_state.h {driver}")
backend = pathlib.Path("dext/sources/dext_compute_backend.inc").read_text()
runtime = re.search(r"#define DEXT_RUNTIME_BUILD (\d+)u", backend)
if not runtime or int(runtime.group(1)) < int(number(state, "MLG_SESSION_CALLS_ASYNC_BUILD")):
    raise SystemExit("DEXT_RUNTIME_BUILD predates MLG_SESSION_CALLS_ASYNC_BUILD: clients would refuse this driver")
python = pathlib.Path("scripts/mlg_owner_call.py").read_text()
bounds = re.search(r"^BOUND_S, LONG_BOUND_S, LONG_SELECTORS = (\d+), (\d+), \(([\d, ]+)\)", python, re.M)
header_bounds = (re.search(r"#define MLG_OWNER_CALL_BOUND_MS\s+(\d+)u", client),
                 re.search(r"#define MLG_OWNER_CALL_LONG_BOUND_MS\s+(\d+)u", client))
long_cases = sorted(int(n) for n in re.findall(r"case (\d+)u:", client.split("mlg_owner_call_bound_ms")[1].split("default:")[0]))
if not bounds or not all(header_bounds) or \
        int(bounds.group(1)) * 1000 != int(header_bounds[0].group(1)) or \
        int(bounds.group(2)) * 1000 != int(header_bounds[1].group(1)) or \
        sorted(int(n) for n in bounds.group(3).split(",")) != long_cases:
    raise SystemExit("scripts/mlg_owner_call.py bounds differ from host/owner_call.h")
for driver, mirror in (("MLG_SELECTOR_OWNER_RESULT", "OWNER_RESULT"),
                       ("MLG_OWNER_ASYNC_HEADER", "OWNER_HEADER"),
                       ("MLG_OWNER_ASYNC_SCALARS", "OWNER_SCALARS"),
                       ("MLG_OWNER_ASYNC_MAX_SCALARS", "OWNER_MAX_SCALARS"),
                       ("MLG_SESSION_CALLS_ASYNC_BUILD", "SESSION_CALLS_ASYNC_BUILD")):
    found = re.search(r"^" + mirror + r"\s*=\s*(\d+)", python, re.M)
    if not found or found.group(1) != number(state, driver):
        raise SystemExit(f"scripts/mlg_owner_call.py {mirror} differs from session_state.h {driver}")
PY
clang++ -std=c++20 -fblocks -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
  -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$test_dir" -Idext/sources -idirafter linuxu/headers \
  linuxu/tests/test_owner_call.cpp -lpthread -o "$test_dir/test_owner_call"
"$test_dir/test_owner_call"
# The host side: host/owner_call.h's bounded wait.
clang -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -Ihost \
  linuxu/tests/test_owner_call_host.c -framework IOKit -framework CoreFoundation \
  -o "$test_dir/test_owner_call_host"
"$test_dir/test_owner_call_host"
