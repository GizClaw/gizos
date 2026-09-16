# Final PAL acceptance — 2026-09-15

Firmware source is `3d7db276`; host source includes `fd8c61d5` and `c3009198`. The five production packages were built together from that source immediately before committing the buffered-poll change. Their frozen hashes and acceptance facts are in [acceptance data](pal-final-acceptance.md#retained-acceptance-facts-pal-final-acceptance). No diagnostic RX or host instrumentation is included. Earlier package results and diagnostic failures remain in the [historical round](./pal-final-round.md).

## Completed checks

- Native Loader, Display App, crash-before-confirm App, public PAL and audio-system packages build under the requested AC791N OrbStack configuration (113.697 seconds).
- Strict Clang/GCC real-source buffered-poll fixture: twelve failing-before combinations, sixteen passing-after combinations including four partial-frame controls. Existing deadline, handoff and iostream tests pass. The handoff/READY and erase fixtures have their own recorded fail-before runs. The host/CLI regression set and host-only iOS analysis pass.
- Production Loader installation through the running Loader's own upgrade flow passes in 52.693 seconds. Both partitions converge to image `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`, Stage is empty, and `last_result=0`.
- The unchanged full UART lifecycle passes **25/25** in **331.305 seconds**, including install-loader, App/crash rollback and coredump read/erase. `reboot-loader-monitor` passes in **7.821 seconds**. The ordinary fixed CLI previously completed Loader reboot in **3.55 seconds**; the suite timing also includes its configured monitor interval and verification.
- Public PAL runs without tracing. The ordinary CLI monitor captures all ten case lines and `result=0 passed=10 failed=0`: Filesystem suite 64/case 13; Core suite 1/cases 1–6, 10, 11; offline Wi-Fi suite 32/case 27. Every case returns zero. Monitor elapsed time is 32.372 seconds; exit 130 is the deliberate SIGINT after observing the complete aggregate. Independent App identity and return-to-P1 status checks pass.

- Full BLE lifecycle through Terminal.app passes **22/22 twice**, in **371.637** and **366.230 seconds**. Both runs include App installation over BLE, App commands and transfer, Stage-preserving reboots, install-loader, crash rollback and coredump read/erase. The second log contains a CoreBluetooth disconnect notification near the last status; that case and the suite both complete successfully without an external retry.

- Audio-system startup and 30 seconds of streaming pass: READY observed, 23 microphone peak reports, correct running App hash, then a successful return to P1. The monitored install/observation takes 62.534 seconds. This does not establish acoustic quality or controlled stop/restart.

Final independent UART status: UID `d879349abc9f`, active/running P1 Loader `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`, `next_partition=1`, `boot_intent=loader`, `stage_valid=0`, `last_result=0`. P1 and P2 are valid; P2 holds audio App `7714f5006e8020c8f51be7b421ad7692fa181d3b6b917bc171acaa617e5f0111`. No human reset or recovery was needed for this final round. UART and BLE test processes have exited.

## Defects and decisions

- Host text-after-frame omission and reboot-history verification: `fd8c61d5`, [handoff evidence](./pal-host-transport.md). A temporary OPEN-ACK parser discarded bytes it had consumed beyond the ACK. The permanent session now receives every trailing byte. Ordinary reboot treats `last_result` as prior installation history; upgrade verification still requires success.
- Host reset reconnect: `c3009198` retires the old KCP conversation when a standalone READY banner arrives. It ignores timestamped historical SDK log replay. The real-source reset/READY fixture fails before and passes after with Clang/GCC.
- Device buffered-frame progress: `3d7db276`, [root cause and regression](./pal-buffered-poll.md). This separately explains the install-loader stall after a correctly received OPEN ACK. No timeout, retry or deadline was weakened.
- O11: `417dcc98`, [SDK audit and fault injection](./pal-upgrade-erase.md). The pinned updater ignores the erase callback result. Verified erase failures therefore latch and gate all later completion/header publication. Native erase errors, dirty readback, short readback and false SDK-success completion fail closed. Physical erase faults were not injected.
- O4: `90a0fc56`, [cross-provider decision](./pal-event-contract.md). Desktop and ESP disagree. Recommend asynchronous queue admission with owned payloads and safe callback retirement; synchronous admission-bounded dispatch is the alternative. No JieLi-only contract change was made.
- O10 watchdog: `b3ca72c5` follows retained capture `8d1ec24b`. Two App and two Loader watchdog trials previously returned to P1 without human action, with correct origin and valid dumps; [paired trial evidence](./pal-watchdog-recovery.md). The final lifecycle crash cases additionally exercise rollback and coredump persistence, not a replacement for those watchdog-specific trials.

## Evidence and limits

Raw final-package commands, reports and logs are under `tmp/jieli/pal-review-next/final-v2/`: `install.json`, `upgrade.log`, `uart.json`, `uart.log`, and the BLE reports/logs. PAL raw output is `tmp/jieli/pal-review-next/diagnostic-runs/final-v2-pal/1/pal.log`, with `result.json`, `after.status` and `returned.status`. Audio raw output and independent final status are `tmp/jieli/pal-review-next/diagnostic-runs/final-v2-audio/1/pal.log` and `returned.status`. Log/status names are deliberately code spans, not VitePress links.

The current UART layout does not provide physical USB deadline coverage. Connected Wi-Fi/AP traffic, never-completing scan recovery, controlled audio stop/restart, physical touch/button/display fault teardown, power-loss atomicity and physical erase fault injection are not established by this round. ESP/BK buffered-poll changes have host-fixture coverage, not new native/board acceptance. O3 recovery and O4 require decisions; O7 concurrency/DNS and O9 rename/open SDK audits remain open in the [ledger](./pal-review.md).

Host validation logs are `/tmp/jieli-final-host-recheck.log` (12 host/CLI tests), `/tmp/jieli-final-gcc-recheck.log`, and `/tmp/jieli-buffered-{before,gcc-before,after,gcc-after,ios,native}.log`. Final guides validation is `/tmp/jieli-final-acceptance-guides-final.log`; `git diff --check` passes. Commits remain local; the unrelated untracked `lock` file is untouched.

## Retained acceptance facts: pal-final-acceptance

Historical run summary transcribed from `pal-final-acceptance.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| date | `2026-09-15` |
| firmware_source | `3d7db276` |
| packages.loader.package_sha256 | `9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae` |
| packages.loader.package_bytes | `925953` |
| packages.loader.image_sha256 | `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada` |
| packages.loader.image_bytes | `937001` |
| packages.display.package_sha256 | `74f0547a4726bcfaf5c734d3b4562b7db085863c0412cab3512c2b3dc7b53a08` |
| packages.display.package_bytes | `863735` |
| packages.display.image_sha256 | `e660ea7cc70eb68a181a19cfe9f8450778788d166f23b83df66c663ef2ba7d76` |
| packages.display.image_bytes | `874845` |
| packages.crash.package_sha256 | `bc40762ead4c1392bad2ab6298cd14924ac098e257d63551ba317010fb93ae37` |
| packages.crash.package_bytes | `894831` |
| packages.crash.image_sha256 | `5e0f2aa3056dd18ce470fb1336d5f3452eb07623cc9d96b41d8b2ed1eec4ce49` |
| packages.crash.image_bytes | `905821` |
| packages.pal.package_sha256 | `08e7aee35e5919a572991038b9161549dbe4608cfd1fdde941ec180fefe6145d` |
| packages.pal.package_bytes | `888782` |
| packages.pal.image_sha256 | `293529fcb71f5cbf59df3160b441d968e0e4aed7b840d704607b31aa0fa4902f` |
| packages.pal.image_bytes | `900009` |
| packages.audio.package_sha256 | `71a071846a295fda29f5a7f430a2408702b1ed84a7a95bf9341afcec09edad2e` |
| packages.audio.package_bytes | `1076611` |
| packages.audio.image_sha256 | `7714f5006e8020c8f51be7b421ad7692fa181d3b6b917bc171acaa617e5f0111` |
| packages.audio.image_bytes | `986141` |
| install.before.device_uid | `d879349abc9f` |
| install.before.active_role | `loader` |
| install.before.active_checksum | `744b336bfe6ffc099709f795f81b19be1d500d362bfde2ffbff1c239cd98e257` |
| install.before.active_image_size | `937257` |
| identity.partition_1_package_checksum | `9767e22acc990e3f3e342ee1f4a6a46989ff5fd783ef629061a8b9f5392391c6`; `9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae` |
| identity.partition_1_image_checksum | `744b336bfe6ffc099709f795f81b19be1d500d362bfde2ffbff1c239cd98e257`; `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada` |
| identity.partition_2_package_checksum | `9767e22acc990e3f3e342ee1f4a6a46989ff5fd783ef629061a8b9f5392391c6`; `9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae`; `71a071846a295fda29f5a7f430a2408702b1ed84a7a95bf9341afcec09edad2e` |
| identity.partition_2_image_checksum | `744b336bfe6ffc099709f795f81b19be1d500d362bfde2ffbff1c239cd98e257`; `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`; `7714f5006e8020c8f51be7b421ad7692fa181d3b6b917bc171acaa617e5f0111` |
| install.after.device_uid | `d879349abc9f` |
| install.after.active_role | `loader` |
| install.after.active_checksum | `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada` |
| install.after.active_image_size | `937001` |
| install.seconds | `52.69283012498636` |
| uart.case_count | `25` |
| uart.case_results | `PASS=25` |
| uart.result | `PASS` |
| uart.rc | `0` |
| uart.uart_endpoint | `/dev/cu.usbserial-20131240` |
| uart.uart_baud_rate | `460800` |
| uart.repeat | `1` |
| uart.monitor_duration_ms | `3000` |
| uart.app_firmware.bytes | `863735` |
| uart.app_firmware.sha256 | `74f0547a4726bcfaf5c734d3b4562b7db085863c0412cab3512c2b3dc7b53a08` |
| uart.loader_firmware.bytes | `925953` |
| uart.loader_firmware.sha256 | `9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae` |
| uart.crash_firmware.bytes | `894831` |
| uart.crash_firmware.sha256 | `bc40762ead4c1392bad2ab6298cd14924ac098e257d63551ba317010fb93ae37` |
| uart.firmware_url.bytes | `0` |
| uart.coredump.expected_bytes | `2096` |
| uart.summary.cases | `25` |
| uart.summary.passed | `25` |
| uart.summary.failed | `0` |
| uart.summary.elapsed_ms | `331305` |
| pal[1].case_count | `10` |
| pal[1].case_results | `0=10` |
| pal[1].variant | `final-v2-pal` |
| pal[1].iteration | `1` |
| pal[1].outcome | `aggregate` |
| pal[1].monitor_rc | `130` |
| pal[1].elapsed_s | `32.37237712502247` |
| pal[1].package_sha256 | `08e7aee35e5919a572991038b9161549dbe4608cfd1fdde941ec180fefe6145d` |
| pal[1].image_sha256 | `293529fcb71f5cbf59df3160b441d968e0e4aed7b840d704607b31aa0fa4902f` |
| pal[1].core_pass | `True` |
| pal[1].aggregate[1][1] | `0` |
| pal[1].aggregate[1][2] | `10` |
| pal[1].aggregate[1][3] | `0` |
| ble[1].case_count | `22` |
| ble[1].case_results | `PASS=22` |
| ble[1].result | `PASS` |
| ble[1].rc | `0` |
| ble[1].uart_baud_rate | `460800` |
| ble[1].ble_endpoint | `5:818f070641f0` |
| ble[1].repeat | `1` |
| ble[1].monitor_duration_ms | `0` |
| ble[1].app_firmware.bytes | `863735` |
| ble[1].app_firmware.sha256 | `74f0547a4726bcfaf5c734d3b4562b7db085863c0412cab3512c2b3dc7b53a08` |
| ble[1].loader_firmware.bytes | `925953` |
| ble[1].loader_firmware.sha256 | `9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae` |
| ble[1].crash_firmware.bytes | `894831` |
| ble[1].crash_firmware.sha256 | `bc40762ead4c1392bad2ab6298cd14924ac098e257d63551ba317010fb93ae37` |
| ble[1].firmware_url.bytes | `0` |
| ble[1].coredump.expected_bytes | `2096` |
| ble[1].summary.cases | `22` |
| ble[1].summary.passed | `22` |
| ble[1].summary.failed | `0` |
| ble[1].summary.elapsed_ms | `371637` |
| ble[2].case_count | `22` |
| ble[2].case_results | `PASS=22` |
| ble[2].result | `PASS` |
| ble[2].rc | `0` |
| ble[2].uart_baud_rate | `460800` |
| ble[2].ble_endpoint | `5:818f070641f0` |
| ble[2].repeat | `1` |
| ble[2].monitor_duration_ms | `0` |
| ble[2].app_firmware.bytes | `863735` |
| ble[2].app_firmware.sha256 | `74f0547a4726bcfaf5c734d3b4562b7db085863c0412cab3512c2b3dc7b53a08` |
| ble[2].loader_firmware.bytes | `925953` |
| ble[2].loader_firmware.sha256 | `9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae` |
| ble[2].crash_firmware.bytes | `894831` |
| ble[2].crash_firmware.sha256 | `bc40762ead4c1392bad2ab6298cd14924ac098e257d63551ba317010fb93ae37` |
| ble[2].firmware_url.bytes | `0` |
| ble[2].coredump.expected_bytes | `2096` |
| ble[2].summary.cases | `22` |
| ble[2].summary.passed | `22` |
| ble[2].summary.failed | `0` |
| ble[2].summary.elapsed_ms | `366230` |
| audio[1].case_count | `0` |
| audio[1].variant | `final-v2-audio` |
| audio[1].iteration | `1` |
| audio[1].outcome | `audio-ready-observed-30s` |
| audio[1].monitor_rc | `130` |
| audio[1].elapsed_s | `62.53413145901868` |
| audio[1].core_elapsed_s | `30.9461777919787` |
| audio[1].package_sha256 | `71a071846a295fda29f5a7f430a2408702b1ed84a7a95bf9341afcec09edad2e` |
| audio[1].image_sha256 | `7714f5006e8020c8f51be7b421ad7692fa181d3b6b917bc171acaa617e5f0111` |
| audio[1].audio_ready | `True` |
| audio[1].mic_reports | `23` |
| final_status.device_uid | `d879349abc9f` |
| final_status.active_role | `loader` |
| final_status.active_checksum | `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada` |
| final_status.active_image_size | `937001` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.install.before | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=744b336bfe6ffc099709f795f81b19be1d500d362bfde2ffbff1c239cd98e257; partition_1_package_checksum=9767e22acc990e3f3e342ee1f4a6a46989ff5fd783ef629061a8b9f5392391c6; partition_2_image_checksum=744b336bfe6ffc099709f795f81b19be1d500d362bfde2ffbff1c239cd98e257; partition_2_package_checksum=9767e22acc990e3f3e342ee1f4a6a46989ff5fd783ef629061a8b9f5392391c6` |
| record.install.after | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=0; partition_1_image_checksum=5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada; partition_1_package_checksum=9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae; partition_2_image_checksum=5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada; partition_2_package_checksum=9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae` |
| record.final_status | `running_partition=1; next_partition=1; last_result=0; boot_intent=loader; stage_valid=0; partition_1_image_checksum=5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada; partition_1_package_checksum=9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae; partition_2_image_checksum=7714f5006e8020c8f51be7b421ad7692fa181d3b6b917bc171acaa617e5f0111; partition_2_package_checksum=71a071846a295fda29f5a7f430a2408702b1ed84a7a95bf9341afcec09edad2e` |
