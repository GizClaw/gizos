# Watchdog recovery follow-up

The retained capture implementation is committed in `8d1ec24b`; reviewer changes were merged in `1565ebb8`. The reviewer physically reset the earlier unresponsive Loader trial before this work resumed. No software recovery or direct flash operation was attempted.

## Pinned SDK findings

SDK revision `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`, `cpu.a` IR (`wdt.c.o`, `p33.c.o`, `poweroff_reboot.c.o`), establishes two production defects. `wdt_init(0xc)` selects four seconds but also enables interrupt mode. Its watchdog ISR prints registers and loops forever. The weak `wdt_clear` unconditionally feeds P33 register `0x80`, bit 6. Emitted production code calls it from the idle hook and the port-yield loop; a progressing core can therefore mask a stopped peer. The earlier trial did request direct reset at one millisecond, so it did not deliberately disable the watchdog. These feed/reset policies predate the retained-block changes; no pre-O10 image was flashed in this follow-up.

`CORE_SYSTEM_RESET` sets bit 4 at `0x10000` and does not disable the P33 watchdog. The production boot patch now always initializes the four-second watchdog and calls `wdt_reset_enable`, clearing interrupt mode. A strong internal-RAM `wdt_clear` consumes progress from both cores before each feed. Its byte admission lock makes one attempt; it never spins waiting for a stopped peer. The underlying RAM P33 primitive serializes the SDK bus and has no callbacks. This detects missing core progress, not every possible task deadlock.

The diagnostic App and Loader hooks now leave watchdog configuration untouched. They report its control byte, disable interrupts on their current core and idle before trial confirmation. Thus the trials exercise the production policy rather than a separately configured diagnostic watchdog.

## Host and native evidence

`//tools/bazel:jieli_watchdog_test` extracts the real board implementation and boot-patch statements. Stopped-core-0, stopped-core-1, healthy alternating progress, boot reset mode and real pthread progress scenarios all fail against the pinned weak feed behavior and old boot patch, then pass with strict Clang/TSan and Linux GCC (`-Wall -Wextra -Werror`). The baseline weak body is reconstructed from the two pinned IR tail calls because the board previously had no override. Existing retained-capture concurrency tests also pass with TSan.

Logs: `/tmp/jieli-watchdog-before.log`, `/tmp/jieli-watchdog-gcc-before.log`, `/tmp/jieli-watchdog-after.log`, `/tmp/jieli-watchdog-gcc-after.log`. Native Loader, PAL and both trial packages build successfully (`/tmp/jieli-watchdog-native.log`). Emitted `wdt_clear` is in internal RAM at `0x01c012cc`; the retained aggregate remains at `0x01c7dd4c`. Host-only iOS analysis passes (`/tmp/jieli-watchdog-ios.log`).

## Board acceptance

The production package `49fc66cf98df5f7eb2daddb79692d2821c003c876d3b0954699ab2b1fd165c91` installed through the Loader's own upgrade flow. UART verified P1 image `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17`, UID `d879349abc9f`, next partition 1 and result 0. Trial results are recorded separately below after both repetitions complete.

Raw artifacts are under `tmp/jieli/pal-review-next/watchdog-rootcause/`: `loader-before.status`, `loader-send.log`, `loader-upgrade.log`, `loader-after.status`, and each trial directory's `uart.raw`, `monitor.log`, `after.status`, `coredump.bin`, `decoded.log` and `result.json`. Raw capture uses the previously instrumented host reader; fixing the ordinary monitor omission remains a separate required item.

The [acceptance facts](pal-watchdog-recovery.md#retained-acceptance-facts-pal-watchdog-recovery) record **two successful App and two successful Loader watchdog trials**. All four returned to P1 without human action, reported control `0x1c`, and produced checksum-valid 2,096-byte coredumps with reset reason `0x4`. Both App trials retained App origin; both Loader trials enabled Loader-origin recovery. The App trials stopped core 0 and core 1 respectively. Elapsed values include unpack/install and host reader shutdown, not just the watchdog timeout.

The reviewer independently verified P1 after the earlier 60-second host-command timeout. It was a host reconnect delay, not a board blackout. Under the revised rule, a delayed monitor is terminated and a separate UART status command determines responsiveness within 90 seconds after reset. The second trials and their subsequent ordinary Loader returns passed that independent check. No human reset or flash recovery occurred in this acceptance round.

Two attempts before the successful second App trial were rejected by `enter_trial_boot()`'s same-image guard and are excluded from watchdog acceptance: `app-watchdog-2` and `app-watchdog-2-after-alternate`. The intervening PAL diagnostic reported ten passed cases but deliberately does not confirm or clear the Display-based trial key. A normal current-source Display App then confirmed (`JIELI_APP_CONFIRM result=OK`, Stage cleared), permitting the second deliberate App fault. No guard was disabled or weakened. The initial Display package-copy path error is preserved in `display-between-trials/send.log`; it did not install firmware.

Additional artifacts: `app-watchdog-2-after-confirm/`, `loader-watchdog-2/`, `display-confirmation/` and `pal-between-trials/` under the same raw evidence directory. Host fixtures were rechecked with Clang/TSan and Linux GCC (`/tmp/jieli-watchdog-accept-host.log`, `/tmp/jieli-watchdog-accept-gcc.log`); the current-source Display native build also passes. Final whole-batch PAL/UART/BLE/audio acceptance follows the remaining host/O11 work.

## Retained acceptance facts: pal-watchdog-recovery

Historical run summary transcribed from `pal-watchdog-recovery.json`; raw capture removed from implementation scope. Values below retain their original units. Absent image/package SHA, partition, Stage, `last_result` or case totals were not recorded in this capture; this summary does not claim them.

| Fact | Recorded value |
| --- | --- |
| status | `two App and two Loader watchdog trials passed` |
| trials[1].trial | `app-watchdog` |
| trials[1].iteration | `1` |
| trials[1].package_sha256 | `e5dd04a30dbbcd47cb6476f8b1bcfe18195ed1020bc533915d29219ba62d754d` |
| trials[1].p1_sha256 | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| trials[1].reset_reason | `4` |
| trials[1].boot_stage | `80` |
| trials[1].log_bytes | `2048` |
| trials[1].control | `28` |
| trials[1].core | `0` |
| trials[1].recovery_marker | `False` |
| trials[1].elapsed_to_status_s | `45.933155083999736` |
| trials[1].status.device_uid | `d879349abc9f` |
| trials[1].status.active_role | `loader` |
| trials[1].status.active_checksum | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| trials[1].status.active_image_size | `936841` |
| identity.stage_package_checksum | `e5dd04a30dbbcd47cb6476f8b1bcfe18195ed1020bc533915d29219ba62d754d`; `9a5fbb4ab72bd94702137c55120138b418e381061f8809bf7ec53402befce6ca` |
| identity.stage_image_checksum | `0cfa44ebf362a7d71ab09248cf69baf0b1a10371477cc245fb55a5d10455e63e`; `51bde9e23b96681dbe8d0c6a44aaaf6a010d228212407217c99a1e48cca9d09b` |
| identity.partition_1_package_checksum | `49fc66cf98df5f7eb2daddb79692d2821c003c876d3b0954699ab2b1fd165c91` |
| identity.partition_1_image_checksum | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| identity.partition_2_package_checksum | `e5dd04a30dbbcd47cb6476f8b1bcfe18195ed1020bc533915d29219ba62d754d`; `9a5fbb4ab72bd94702137c55120138b418e381061f8809bf7ec53402befce6ca` |
| identity.partition_2_image_checksum | `0cfa44ebf362a7d71ab09248cf69baf0b1a10371477cc245fb55a5d10455e63e`; `51bde9e23b96681dbe8d0c6a44aaaf6a010d228212407217c99a1e48cca9d09b` |
| trials[2].trial | `loader-watchdog` |
| trials[2].iteration | `1` |
| trials[2].package_sha256 | `9a5fbb4ab72bd94702137c55120138b418e381061f8809bf7ec53402befce6ca` |
| trials[2].p1_sha256 | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| trials[2].reset_reason | `4` |
| trials[2].boot_stage | `80` |
| trials[2].log_bytes | `2048` |
| trials[2].control | `28` |
| trials[2].core | `1` |
| trials[2].recovery_marker | `True` |
| trials[2].elapsed_to_status_s | `32.713561124983244` |
| trials[2].status.device_uid | `d879349abc9f` |
| trials[2].status.active_role | `loader` |
| trials[2].status.active_checksum | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| trials[2].status.active_image_size | `936841` |
| trials[3].trial | `app-watchdog` |
| trials[3].iteration | `2` |
| trials[3].package_sha256 | `e5dd04a30dbbcd47cb6476f8b1bcfe18195ed1020bc533915d29219ba62d754d` |
| trials[3].p1_sha256 | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| trials[3].reset_reason | `4` |
| trials[3].boot_stage | `80` |
| trials[3].log_bytes | `2048` |
| trials[3].control | `28` |
| trials[3].core | `1` |
| trials[3].recovery_marker | `False` |
| trials[3].elapsed_to_status_s | `45.70265858300263` |
| trials[3].status.device_uid | `d879349abc9f` |
| trials[3].status.active_role | `loader` |
| trials[3].status.active_checksum | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| trials[3].status.active_image_size | `936841` |
| trials[4].trial | `loader-watchdog` |
| trials[4].iteration | `2` |
| trials[4].package_sha256 | `9a5fbb4ab72bd94702137c55120138b418e381061f8809bf7ec53402befce6ca` |
| trials[4].p1_sha256 | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| trials[4].reset_reason | `4` |
| trials[4].boot_stage | `80` |
| trials[4].log_bytes | `2048` |
| trials[4].control | `28` |
| trials[4].core | `1` |
| trials[4].recovery_marker | `True` |
| trials[4].elapsed_to_status_s | `32.76299770799233` |
| trials[4].status.device_uid | `d879349abc9f` |
| trials[4].status.active_role | `loader` |
| trials[4].status.active_checksum | `635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17` |
| trials[4].status.active_image_size | `936841` |
| excluded_attempts[1] | `app-watchdog-2: same-image rollback guard before hook` |
| excluded_attempts[2] | `app-watchdog-2-after-alternate: PAL diagnostic does not clear Display trial guard` |
| intervening_confirmation | `display-confirmation: JIELI_APP_CONFIRM result=OK; stage cleared` |
| reviewer_correction | `Prior 60-second host timeout was not a board hang; reviewer independent UART status passed. Revised safety uses independent status within 90 seconds after reset.` |

| Status checkpoint(s) | Running partition / Stage / result |
| --- | --- |
| record.trials[1], record.trials[3] | `running_partition=1; next_partition=1; last_result=0; boot_intent=auto; stage_valid=1; partition_1_image_checksum=635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17; partition_1_package_checksum=49fc66cf98df5f7eb2daddb79692d2821c003c876d3b0954699ab2b1fd165c91; partition_2_image_checksum=0cfa44ebf362a7d71ab09248cf69baf0b1a10371477cc245fb55a5d10455e63e; partition_2_package_checksum=e5dd04a30dbbcd47cb6476f8b1bcfe18195ed1020bc533915d29219ba62d754d; stage_image_checksum=0cfa44ebf362a7d71ab09248cf69baf0b1a10371477cc245fb55a5d10455e63e; stage_package_checksum=e5dd04a30dbbcd47cb6476f8b1bcfe18195ed1020bc533915d29219ba62d754d` |
| record.trials[2], record.trials[4] | `running_partition=1; next_partition=1; last_result=-7; boot_intent=auto; stage_valid=1; partition_1_image_checksum=635662ded9c2bc0e687038139cbd0af3bc22df212bbd0e86ce7bb5b9a9998e17; partition_1_package_checksum=49fc66cf98df5f7eb2daddb79692d2821c003c876d3b0954699ab2b1fd165c91; partition_2_image_checksum=51bde9e23b96681dbe8d0c6a44aaaf6a010d228212407217c99a1e48cca9d09b; partition_2_package_checksum=9a5fbb4ab72bd94702137c55120138b418e381061f8809bf7ec53402befce6ca; stage_image_checksum=51bde9e23b96681dbe8d0c6a44aaaf6a010d228212407217c99a1e48cca9d09b; stage_package_checksum=9a5fbb4ab72bd94702137c55120138b418e381061f8809bf7ec53402befce6ca` |
