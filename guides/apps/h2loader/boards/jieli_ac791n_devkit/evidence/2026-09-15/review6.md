# Review 6 acceptance — 2026-09-15

Base: `6fbd2852`. Local fixes: input `100b861d`, Wi-Fi persistence `c01fe68f` plus SDK include ordering `27020aa6`, subscription quiescence `3cfb59e8`, standalone MP4 ownership `ce1d6c2a`.

## Host regressions

- Input: `test_jieli_input_lifecycle.py` failed its new `invalid_input` case before repair under strict Clang and GCC. Display/touch output NULL checks precede open state; button NULL reads do not access ADC. All cases pass after.
- Wi-Fi: `test_jieli_wifi_save.py` failed because the STA vtable callback was absent. The real callback now uses the shared `libs/wifi_sta` transaction, retaining radio admission through fresh association, IP validation and durable save. Success replaces settings; connect failure preserves old credentials; save failure propagates. The native build additionally exposed SDK `bool` typedef versus `stdbool.h` include ordering; its strict host reproduction fails before and passes with SDK headers first.
- Events: `test_jieli_runtime_events.py` failed on `assert(!atomic_load(&unsubscribe_done))` while the handler still borrowed its heap context. Clang/GCC pass after repair; Clang ThreadSanitizer passes the pthread fixture, including self-unsubscribe and concurrent init/deinit. External unsubscribe retires admission and waits for all dispatches; self unsubscribe returns without waiting and retains the slot until dispatches finish. The caller must keep context alive for other already-running calls. Darwin/Windows providers similarly count in-flight callbacks and wait on a condition; ESP delegates unregister to `esp_event_handler_instance_unregister`. JieLi retains synchronous delivery and existing owner/operation references.
- MP4: the runner layout case failed with the missing `native_firmware` BUILD on both hosts. The complete runner tests now pass; `:firmware` remains the terminal target. The Loader-managed MP4 target is unchanged.

Logs are retained locally as `/tmp/review6-{input,wifi,events,path}-*.log` and `/tmp/review6-wifi-order-*.log`. Touched Bazel tests passed with `--config=macos_arm64`: input lifecycle, Wi-Fi save/operations, shared Wi-Fi STA, wl82 event threads/Runtime integration, and JieLi runner. The host-only compatibility selections remain in place. iOS analysis passed:

```sh
bazel build --config=ios_sim_arm64 --nobuild --keep_going //tools/bazel:all
```

## Native build

OrbStack `embed-zig-noble-amd64`, pinned SDK `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `--config=ac791n` and `--symlink_prefix=bazel-amd64-`: all six targets passed in 102.340 seconds.

```text
//projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package
//projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit:package
//projects/example/targets/h2loader_tar_zlib/button/jieli_ac791n_devkit:button_package
//projects/example/targets/h2loader_tar_zlib/touch/jieli_ac791n_devkit:touch_package
//projects/example/targets/h2loader_tar_zlib/audio-system/jieli_ac791n_devkit:audio_system_package
//projects/example/targets/native_firmware/mp4-player/jieli_ac791n_devkit:firmware
```

Build log: `/tmp/review6-native-after.log`. No native firmware was installed through vendor download tools.

## Button hardware

Re-listed `/dev/cu.*`; independent status identified UID `d879349abc9f` on `/dev/cu.usbserial-20131240`, 460800 baud. The original P1 Loader SHA was `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`.

Installed only through UART Loader `send --file` into P2, followed by `reboot upgrade --monitor`. Button package SHA: `e95050705a3bf8e2ed8bed9337aca1bf064daa255af113da685de052a1001fe5`. P2 image SHA: `f63ee7aef3d5e5a3751654dba1372e972b18fdd2a262f2467aed730b195d81f5`.

Initial trial capture contains:

```text
H2_JIELI_BUTTON_SMOKE stage=runtime-init result=0
H2_JIELI_BUTTON_SMOKE stage=input-start result=0
H2_JIELI_BUTTON_SMOKE_READY buttons=8 display=480x320 result=0
```

Independent status reported that P2 image active, `stage_valid=0` and `last_result=0`. Because the initial monitor missed confirmation, the same installed image was relaunched via `reboot loader`, independent P1 status, and `reboot app --monitor`. This capture contains:

```text
JIELI_APP_CONFIRM result=OK code=0 target=0 transport=0
```

The automatic capture checker required READY and confirmation in one capture and failed that condition. Both markers were captured across these two boots of the identical installed image; this is the observation limit. Independent UART status after relaunch again identified the same P2 image and empty Stage. The final `reboot loader` and independent status verified original P1 SHA unchanged, `active_role=loader`, `running_partition=1`, `stage_valid=0`. No >90-second loss of independent responsiveness occurred.

Raw captures/statuses and structured result: `tmp/jieli/review6/button/`. Touch/audio/display were built but not installed in this round. The button check is lifecycle evidence, not a hardware injection of the unsubscribe race; that race is covered by the pthread/TSan regression.

## Wi-Fi hardware limit

No bench AP credentials were found in the AC791N evidence or local JieLi scripts. The sole credential-pattern candidate was a Wi-Fi source-rewrite script containing the `NOT_FOUND_SSID` event name. Per the requested condition, connected Wi-Fi and persistence across Loader reboot are **SKIP**. Host tests prove save ordering and errors; they do not prove bench association or flash persistence across a physical board restart.

## Issue draft

The corrected Issue #177 body preserves the original section headings and prose outside the tree, except for the requested MP4 path correction. The original tree had 27 generic leaves: 21 receive concrete contracts/scenarios; six are removed because those paths are not in the current branch diff (one, `test_jieli_audio_pal.py`, no longer exists). All other stale leaves are removed and missing changed files are added, including committed historical evidence. The final audit compares the exact leaf set with `git diff --name-only origin/main...HEAD`, rejects duplicates/placeholders and checks that headings are unchanged.
