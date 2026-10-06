#!/usr/bin/env bash
# The dext's display calls (dext/sources/MacLinuxGPUXcode.mm display_call):
# an op that can sleep never runs on the client's call, with the production
# functions extracted and DriverKit mocked (linuxu/tests/test_display_async.cpp),
# and the host app's own call checked against the driver's validation.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 - "$test_dir" <<'PY'
import pathlib, sys
source = pathlib.Path("dext/sources/MacLinuxGPUXcode.mm").read_text()
def section(start, end, limit):
    if source.count(start) != 1 or source.count(end) < 1:
        raise SystemExit(f"display async extraction markers changed: {start!r}")
    begin = source.index(start)
    finish = source.index(end, begin)
    text = source[begin:finish]
    if len(text.splitlines()) > limit:
        raise SystemExit("display async extraction exceeded its bounded section")
    return text
stops = section("/* Stopped clients whose imports and output are still to release", "static void observer_display_client_stop_now(uint64_t clientID)\n{", 70)
# display_call is the section's last function: it ends at its own closing
# brace, so whatever the dext places after it (build 263's kick_on_delivery)
# stays out of the extraction.
calls_last = "static kern_return_t display_call(MacLinuxGPUUserClient *client, uint64_t clientID,"
if source.count(calls_last) != 1:
    raise SystemExit(f"display async extraction markers changed: {calls_last!r}")
calls = section("// A client's last display op result (MacLinuxGPUUserClient_IVars::displayResults).", calls_last, 370)
calls_end = source.index("\n}\n", source.index(calls_last)) + 3
calls += source[source.index(calls_last):calls_end]
if len(calls.splitlines()) > 370:
    raise SystemExit("display async extraction exceeded its bounded section")
pathlib.Path(sys.argv[1], "display_async_production.inc").write_text(stops + calls)
PY
# The host app's call (host/display_call.h) against the driver's own
# validation: built against IOKit in a file of its own.
clang -std=gnu11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Ihost -Ilinuxu/tests -c linuxu/tests/display_call_host.c -o "$test_dir/display_call_host.o"
clang++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
  -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$test_dir" -Idext/sources -Ilinuxu/tests -idirafter linuxu/headers \
  linuxu/tests/test_display_async.cpp "$test_dir/display_call_host.o" -framework IOKit -lpthread \
  -o "$test_dir/test_display_async"
"$test_dir/test_display_async"
