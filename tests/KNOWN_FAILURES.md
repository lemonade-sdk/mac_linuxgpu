# Known test failures

Every `make test` target listed here fails for a known reason and is skipped:
its recipe calls `scripts/known-failure.sh`, which prints `SKIP <target>` with
the reason. The test code stays in place. Fix the cause, then delete the line
to bring the test back. Anything not listed still fails the run.

An entry marked `(CI)` fails only on the hosted CI runner (a shared macOS VM
whose timing these tests' windows do not allow for). It is skipped only where
`CI` is set, as GitHub Actions sets it, and still runs on a developer Mac.

To run the listed tests anyway, set `MAC_LINUXGPU_RUN_KNOWN_FAILURES=1`.

## Excluded

- `test-mutex-completion` (CI) — the 9 ms bound on a completion woken 5 ms in is exceeded on the runner (test_mutex_completion.c:104).
- `test-iokit-dma` (CI) — hold_waits_for_inflight_operation's 20 ms release inside the 50 ms shutdown hold misses on the runner (test_iokit_dma.cpp:695).
- `test-display-pipeline` (CI) — a flip does not complete within 200 ms of its copy on the runner's software GPU fixture, so the output worker stops (-ETIME).
- `test-scanout` (CI) — the same 200 ms flip deadline as test-display-pipeline, missed on the runner (test_scanout.c:305, errno ETIME).
