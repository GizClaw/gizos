# DevKit PAL Core qualification

This independent managed App runs Core contract v2 (41 cases, 46 vtable operations) on the DevKit ESP32-S3's actual providers. Atomic is a separate package; this does not qualify the other PAL domains such as networking or media. The transport is the existing H2Loader USB Serial/JTAG channel using iostreamikcp, not an external UART0 cable.

## Build and run

Use the repository-pinned ESP-IDF and tool versions. Keep the Bazel disk cache enabled; set `IDF_PATH` and `IDF_TOOLS_PATH` to the installed pinned SDK/tools.

```sh
bazel build --config=esp32s3 --lockfile_mode=off \
  --//tools/bazel:firmware_version=pal-core-devkit-<unique-build> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-core/devkit:package
bazel build --lockfile_mode=off //projects/h2loader/targets/cc_binary/cli:h2loader
```

The package and its checksum/image manifest are in `bazel-bin/projects/e2e/targets/h2loader_tar_zlib/pal-core/devkit/package/`. Use the CLI's host binary and the currently enumerated port. Before sending, verify the device identity from live `status`; a port name alone is insufficient. The validation device was UID `9888e0115c52`, board `devkit`, target `esp32s3`.

```sh
h2loader --no-ble --port /dev/cu.usbmodem201313231 status
h2loader --no-ble --port /dev/cu.usbmodem201313231 send --file <package.update.tar.zlib>
h2loader --no-ble --port /dev/cu.usbmodem201313231 status
h2loader --no-ble --port /dev/cu.usbmodem201313231 --wait-timeout 120 reboot upgrade --monitor
```

Require the staged package SHA and image SHA to match the build manifest before reboot. Capture the new image's `H2_PAL_CORE_E2E_BOOT ... version=...` marker; reports before that marker may belong to the previous App. Collect all 41 unique case records and reject conflicting replays. Qualification requires:

```text
contract=2 passed=41 failed=0 blocked=0 not_run=0 complete=1 qualified=1 cleanup=0 rc=0
```

The launcher executes once per boot, then replays the same immutable report slowly so console bytes lost during Loader handoff cannot hide a case. Replays are not additional runs. A host SIGINT stopping the monitor is not a test failure. Afterward, read `status` and `coredump status`: require matching active image/version, valid P2 App, empty Stage, successful Loader result and no crash. Use H2Loader managed install/reboot only; the Loader partition is retained.

## Real observations

- Native lifecycle counters cover Tasks, reserved stack bytes, queues, mutexes, semaphores, conditions, timers and the static firmware-info provider. Native PAL heap counters report live allocations and SDK-reported usable bytes.
- The serial management fixture uses a separate SDK heap allocator with the same PSRAM capabilities. Asynchronous transport buffers do not enter the Core baseline. Core Memory and native Timer allocations retain the actual instrumented PAL allocator; no snapshot fields are replaced with zero.
- Task policy is installed normally. Its PSRAM allocator hook forwards real stack allocation and records the address/range. Each tested worker checks its FreeRTOS stack base and a stack-local address for the 4/16/64 KiB requests. Allocation-failure injection rejects this actual stack allocation path.
- An extra 100-cycle Task probe reports native kernel task count, stack owners, SDK heap allocated bytes/blocks, the three requested stack sizes and failed allocation recovery. SDK heap evidence is distinct from the PAL counters.
- The event fixture exclusively owns H2 subscriptions on the actual ESP event provider. Runtime's unrelated event capability stays unsupported; H2Loader runs its serial service without BLE. The shared SDK event loop may persist.
- Timer uses native workers and a static reaper for callback self-destruction. External stop/destroy waits for an in-flight callback. The Core suite checks all seven Timer operations plus one-shot, periodic/isolation and churn.
- Log levels are observed through the actual ESP sink. Firmware version comes from the app descriptor. Timing is checked against `esp_timer_get_time`. A separate 120-second watchdog reports a stalled case.

Image confirmation preserves the H2Loader management path and does not turn a failed Core report into a pass. Qualification is always the explicit ledger.

## Baseline and fixes

`evidence/2026-09-28/baseline.json` records the initial 27 PASS / 3 FAIL / 11 BLOCKED result. Timer was unsupported and the launcher lacked resource observations. Queue close discarded buffered messages and allowed a blocked sender to report success after close; both provider behaviors were corrected.

The initial external-unsubscribe failure was a test-harness error: asynchronous post completion shared the unsubscribe completion semaphore. It now has its own completion signal, with a host regression that delivers events asynchronously. ESP's event provider did not need that behavioral change.

Enabling every case also exposed insufficient allocation alignment. Native PAL allocation/reallocation now preserves `max_align_t`, original data and the old allocation on failure. The resource observer was isolated from unrelated serial packet allocations without relaxing any Core resource comparison.

## Verified device result (2026-09-28)

The installed `pal-core-devkit-20260928-r6` image passed **41/41**, with **0 FAIL, 0 BLOCKED, 0 NOT_RUN, cleanup=0, qualified=1**. The supplementary 100-cycle Task probe passed; actual stacks were 4096, 16384 and 65536 bytes, and a rejected stack allocation returned NO_MEMORY followed by successful recovery. Native kernel task and stack-owner counts returned to their baselines.

The final active P2 image SHA was `2595a4ceed5689bab4fcda586a21fe311c5d9364437bf620ad6443bce7fa9b09`. The management channel remained responsive, Stage was empty, Loader's last result was zero and the coredump partition was blank. Loader P1 remained `0.2.3-atomic-541`.

[The parsed result](evidence/2026-09-28/qualified.json) contains every case, Task observations, image identity and final status. [The raw boot log](evidence/2026-09-28/qualified-boot.log), send ACK, build manifest, status and coredump logs are retained beside it. The SDK whole-heap probe observed -7828 bytes / -8 blocks due to unrelated asynchronous transport buffers; only the PAL Core baseline comparison is an exact zero-delta gate. Desktop acceptance, asynchronous-event regression, allocator/task regressions, interface inventory and the real Chromium Core test also passed.
