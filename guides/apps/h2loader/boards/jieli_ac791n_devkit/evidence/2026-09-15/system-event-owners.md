# System-event owner lifetime correction

The startup-only test in `058498ff` missed shared teardown. At `40f8c636`, the
launcher's init and Runtime's init both succeeded, but Runtime deinit destroyed
the provider and its still-owned launcher/BLE subscriptions. This correction is
in the provider; no target exceptions or alternative lifecycle modes are added.

## Contract and implementation

One wl82 atomic state word contains phase, 14-bit init-owner count and 15-bit
in-flight operation count. First init reserves INITIALIZING, creates the registry
and publishes ACTIVE with one owner. Further ACTIVE inits atomically add owners;
16383 owners is the limit and overflow returns `H2_PAL_ERR_FULL` without acquiring
an owner. INITIALIZING/CLOSING init returns BUSY. Each balanced deinit drops one
owner; only the last atomically changes ACTIVE to CLOSING. Operations retained
before closing keep the registry alive, and the final operation destroys it.
This also preserves last-owner deinit from a callback without self-waiting.
Extra deinit while inactive does nothing. Callers must retire their subscriptions
and must not release another caller's owner.

## Failing-before and passing-after

- Updated Runtime/provider test leaves the launcher subscriber registered while
  `h2_runtime_deinit` executes, then posts through the original provider. On
  `40f8c636` it fails at `h2_pal_system_event_post(...) == H2_PAL_OK` after teardown,
  with both Clang and GCC. After the fix it delivers to the launcher, and final
  launcher deinit destroys the registry without leaks; extra deinit is harmless.
- The pthread fixture fails before at `live == 1` after releasing the second init
  owner. After the fix it covers owner saturation, partial release, last release,
  callback-driven release with one/two owners, INITIALIZING/CLOSING BUSY, and
  retained post/subscribe operations during teardown.
- Four threads each acquire/post/release 2000 times, both with an anchored launcher
  owner/subscription and without one (racing final teardown and fresh init).
  Apple Clang ThreadSanitizer passes with no reported races. The direct strict
  Clang/GCC runs and the existing core provider fixture also pass warning-free.

Commands:

```sh
python3 tools/bazel/tests/test_jieli_runtime_events.py
orb -m embed-zig-noble-amd64 bash -lc 'cd /Users/idy/GizClaw/gizos/.claude/worktrees/qrcode-library-selection-fab623 && python3 tools/bazel/tests/test_jieli_runtime_events.py'
JIELI_TEST_CFLAGS='-fsanitize=thread -g -O1' python3 tools/bazel/tests/test_jieli_runtime_events.py RuntimeEventsTest.test_owner_threads
bazel test --config=macos_arm64 \
  //native_component_src/jieli/wl82/h2_pal_core:test_jieli_wl82_runtime_events \
  //native_component_src/jieli/wl82/h2_pal_core:test_jieli_wl82_event_threads \
  //native_component_src/jieli/wl82/h2_pal_core:test_jieli_wl82_platform_core
bazel build --config=ios_sim_arm64 --nobuild --keep_going //tools/bazel:all
```

Existing host-only test compatibility selects are retained. Compiler flags are
`-std=c11 -Wall -Wextra -Werror`; compilers are Apple Clang 21 and GCC 13.3.
Logs are `/tmp/jieli-owners-before-{clang,gcc}.log`,
`/tmp/jieli-owners-threads-before.log`, `/tmp/jieli-owners-after-{clang,gcc}.log`,
`/tmp/jieli-owners-core-{clang,gcc}.log`, `/tmp/jieli-owners-tsan.log`,
`/tmp/jieli-owners-bazel.log`, and `/tmp/jieli-owners-ios.log`.

## Native build

Loader, display, button, touch and audio-system production packages all build in
`embed-zig-noble-amd64` with `--config=ac791n --symlink_prefix=bazel-amd64-`, in
95.765 seconds. Build uses the existing AC791N environment setup, clearing the
unrelated IDF variables. SDK pin is `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`.
The source is `40f8c636` plus this owner-count correction; package hashes and the
provider source digest are recorded in the accompanying acceptance data.
Native build log: `/tmp/jieli-owners-native.log`.

## Button hardware acceptance

UART ports were re-listed. Independent initial status verified UID `d879349abc9f`,
`/dev/cu.usbserial-20131240` at 460800, original P1 Loader and empty Stage.
Button was installed through that Loader into P2. The P1 Loader was not replaced;
no USB DL, format, physical reset or recovery action was used.

The installation monitor recorded `runtime-init=0`; its subsequent independent
App status showed the correct new P2 image and Stage empty, but the monitor missed
the confirmation text. A no-Stage return to Loader followed by `reboot app
--monitor` captured the complete startup:

```text
H2_JIELI_BUTTON_SMOKE stage=runtime-init result=0
H2_JIELI_BUTTON_SMOKE_READY buttons=8 display=480x320 result=0
JIELI_APP_CONFIRM result=OK code=0 target=0 transport=0
```

Independent App status after stopping the monitor verified image
`290b6b9776f1ba1e7855d7e99539cd83e3076516225b992662d1a3e1885f3cf1`, P2 running,
Stage empty, `last_result=0`. A final `reboot loader` and separate status returned
to P1 image `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`,
next P1, Stage empty and `last_result=0`. No UART status request timed out.
The observed install/relaunch/return interval after `reboot upgrade` was 54.554 s.

[Acceptance data](./system-event-owners.json) contains all five package hashes,
the provider source digest and independent App/P1 status snapshots. Raw commands
and logs are under `tmp/jieli/event-owners/`: `accept.py`, `button/send.log`,
`button/boot.log`, `button/after.status`, `button/relaunch.log`,
`button/after-relaunch.status` and `button/returned.status`.

This board run verifies button startup and trial confirmation. Runtime teardown
while the launcher stays alive is proven by the host regression, not by pretending
the button App normally shuts its Runtime down. Loader/display/touch/audio-system
were built, not installed in this round. Physical button interaction was not tested.
