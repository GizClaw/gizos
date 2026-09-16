# Shared launcher and Runtime event reuse — 2026-09-15

## Follow-up: teardown ownership

This record only established startup reuse at `40f8c636`; its test stopped the
launcher subscriber before Runtime teardown and therefore missed a real ownership
bug. The provider had no init-owner count, so Runtime deinit destroyed the registry
while the launcher still owned it. The [owner-count correction](./system-event-owners.md)
adds lifecycle references and tests that keep the launcher subscriber alive through
Runtime deinit. The startup results below remain historical evidence, not proof of
safe shared teardown.

## Finding disposition

The review of `5b1d822a` alleged that the launcher's event initialization makes
button, touch and audio-system fail Runtime initialization with `H2_PAL_ERR_BUSY`.
The two calls exist, but that error does **not** describe this revision:
`system_event_init` already returns OK for `EVENT_ACTIVE`, preserving the registry
and existing BLE subscriptions. BUSY applies to initialization/closing transitions.
This behavior was already present in `a37942f5`, before the portable-atomic change
`b743fa41` and the reviewed head. No production ownership change or target exception
was introduced for this finding.

At that revision, the launcher initialized the provider before BLE subscribed;
Runtime initialization reused the registry and added subscriptions, but repeated
init did not acquire an independent owner. The test removed the launcher subscriber
before Runtime deinit, masking teardown of the still-owned provider. Keeping Runtime
alive in the tested Apps only established startup behavior, not correct ownership.

## Host regression

`test_jieli_wl82_runtime_events.c` links the real Runtime and JieLi core providers
with the existing deterministic SDK fake. It initializes the launcher provider,
registers a BLE subscriber, calls `h2_runtime_init`, posts an event to the retained
subscriber, starts/runs a smoke task through the PAL task provider, then cleans up.

- The unchanged reviewed production code passes with `runtime-init=0`.
- A temporary source copy that changes the ACTIVE reuse branch to return BUSY
  fails at the Runtime success assertion (`runtime-init=-18`). This is a sensitivity
  check, **not** claimed as a failing-before result from `5b1d822a`.
- Both results were checked with Apple Clang 21 and OrbStack GCC 13.3, using
  `-std=c11 -Wall -Wextra -Werror`. No compiler warnings occurred.
- `//native_component_src/jieli/wl82/h2_pal_core:test_jieli_wl82_runtime_events`
  and the existing event concurrency test pass under `--config=macos_arm64`.
  The new cc_test has a host-only compatibility select. The direct compiler runner
  is `tools/bazel/tests/test_jieli_runtime_events.py`.

Logs: `/tmp/jieli-review-events-{clang,gcc}.log`,
`/tmp/jieli-review-events-busy-{clang,gcc}.log`,
`/tmp/jieli-review-events-bazel.log`. The mutation was compiled from a temporary
copy; the working provider was never replaced.

## Native and board acceptance

The three production packages were built together from `d44dcd40`, containing the
AP-field, CCCD-read and audio-probe fixes. The event provider, Runtime and three
target entrypoints are unchanged from the review base. Build ran in
`embed-zig-noble-amd64`, with `--config=ac791n --symlink_prefix=bazel-amd64-`.
The first attempt was blocked by host ESP-IDF environment variables; clearing
`IDF_PATH`, `H2LOADER_IDF_PATH`, `IDF_PYTHON_ENV_PATH`, `IDF_TOOLS_PATH` as in the
existing AC791N build script allowed the build to pass in 64.286 seconds.

UART was re-enumerated before use. UID `d879349abc9f`, port
`/dev/cu.usbserial-20131240`, baud 460800. The stable P1 Loader image remained
`5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada` throughout.
All packages were sent through the running UART Loader and installed into P2;
no USB DL, format, manual reset or P1 replacement was used.

| Target | Observed startup | Trial / independent status |
| --- | --- | --- |
| button | runtime-init=0, input-start=0, `H2_JIELI_BUTTON_SMOKE_READY ... result=0` | confirmation OK; App P2 identity matches, Stage empty; returned to P1 |
| touch | runtime-init=0, input-start=0, `H2_JIELI_TOUCH_SMOKE_READY ... result=0` | confirmation OK; App P2 identity matches, Stage empty; returned to P1 |
| audio-system | runtime-init=0, run=0, `H2_JIELI_AUDIO_SYSTEM_READY mic=1 speaker=1` | confirmation OK; App P2 identity matches, Stage empty; returned to P1 |

[Package hashes and independent status snapshots](./runtime-event-reuse.json)
record every image and package SHA. The final board state is responsive P1 Loader,
next P1, boot intent Loader, Stage empty, `last_result=0`; P2 retains audio-system.
There was no independent UART status timeout and no 90-second unresponsive interval.

### Observation limits and retained attempts

Installation monitors did not capture the complete confirmation sequence. The
button monitor initially stopped while the App was entering its target; a later
independent status showed Stage cleared. Touch/audio installation status already
showed Stage empty. For each App, a subsequent `reboot loader` → `reboot app`
without Stage captured READY and `JIELI_APP_CONFIRM result=OK code=0`, followed by
an independent App status and a verified return to P1. Thus confirmation is backed
by both persistent state and a complete relaunch log, not inferred from process exit.

One preliminary touch status had SDK startup text interleaved with the status line;
the harness stopped before sending anything, retained it, then obtained a clean
independent status. Raw attempts, commands and successful relaunches remain under
`tmp/jieli/review-5b1d822a/`: `accept.py`, `relaunch.py`, per-target `boot.log`,
`relaunch.log`, `relaunch.status`, `returned.status`, and
`touch-status-interleaved/before.status`.

This verifies task startup and Loader trial lifecycle. It does not establish
physical button/touch interaction, display quality, acoustic quality, connected
Wi-Fi/AP traffic, or live BLE Read Blob boundary behavior. The other three review
fixes have separate failing-before Clang/GCC host regressions.
