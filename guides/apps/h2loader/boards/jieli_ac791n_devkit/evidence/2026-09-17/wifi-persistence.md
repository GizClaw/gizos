# Wi-Fi credential persistence on the DevKit App — 2026-09-17

## Outcome and source

**PASS on the final images.** On UID `d879349abc9f`, `wifi connect` from the UART host provisioned the bench access point `HAIVIVI-MFG` through `h2_pal_wifi_sta_connect_and_save()` in 2 s, `wifi status` then reported `state=5 ip_valid=1 ip=192.168.4.150 saved=1` for that SSID, and every later boot of the display and button Apps (`reboot app` from the App, `reboot app` after `reboot loader`, a fresh `reboot upgrade` install) reconnected from the saved credential without a new `wifi connect`. A wrong password and an unknown SSID both failed inside the 15 s budget with the saved credential unchanged, and the next reboot reconnected. The round ended on the P1 Loader with Stage empty, `last_result=0` and a blank coredump partition.

Tested source is branch `wifi-persistence-ac791n` at `efbabcc3` (GizOS PR #459, Issue #454), which contains `origin/main` `3eeb1b55`; the last firmware-affecting commit is `7285f0fd`, and `efbabcc3` only changes host tests. The board is on `/dev/cu.usbserial-20131240` at 460800 with BLE endpoint `5:818f070641f0`. Every install went through the UART Loader into Partition 2 with `send --file` and `reboot upgrade --monitor`; the Loader in Partition 1 stayed the #449 image `ed7d71a667fe5ef20d09afff11f4909ba33a13af7e852ec4f0ac981541290fdf` throughout, and there was no USB download, format or manual power cycle.

The Loader advertises `capabilities=0x00000005` (UART and BLE) and `command_availability=0x00081d1f`, so it exposes no Wi-Fi command; every Wi-Fi step ran on the App command service (`capabilities=0x00000007`, `command_availability=0x001f3d3f`, which includes the new `wifi status` bit 20). The bench password was supplied for this run only and appears in no repository file; SDK boot logs echo it, so no raw console line is quoted here.

## Defects found and fixed on the way

The first attempt, on the `main` color-bar App `ff4c95adf533f81b4c7fa270b690eeab6c47013544a758b3243dfe6e9440cd2c`, never returned a terminal line and the UART repeated `please add wpasupplicant.a`; the App UART command task stayed blocked (`status` returned `TIMEOUT`) until a BLE `reboot loader`. The recovered LLVM IR showed that `wl_wifi.a/wifi_connect.c.o` defines weak `wpa_supplicant_main`/`wpa_supplicant_uninit`/`wpa_supplicant_req_scan_ssid`/`wifi_set_sta_connect_timeout` stubs and nothing in the layout link extracted the real `wpasupplicant.a/main_none.c.o`; the layout now lists `wpasupplicant.a` before the driver, drops `hostapd_and_wpasupplicant.a` and forces `--undefined=wpa_supplicant_get_state`, and the stub string is absent from all three linked ELFs.

With the supplicant linked, `wifi connect` failed in 2 s with `code=-4 state=7 disconnect_reason=-1`: the provider treated `wifi_enter_sta_mode()`'s `-1` as a failure, but in the SDK's default non-blocking mode that call returns `-1` whenever the station is not already connected. The host also never recognised success, because it waited for `result=connecting` while the device prints `result=connected`. Both are fixed.

The next build timed out with `state=1` and no SDK event: the generated App task tables did not list `tcpip_thread`, `tasklet`, `RtmpMlmeTask`, `RtmpCmdQTask` and `wl_rx_irq_thread`, and SDK `task_create()` fails for any name missing from `task_info_table`. Each Wi-Fi-capable App target now lists those rows in its own task policy.

With the tasks registered the driver still never scanned, because `wifi_on()` without a default mode makes the SDK's first STA entry use its placeholder SSID and the later re-entry never associated. A bench diagnostic that installed the target as a forced, unstored STA default before `wifi_on()` associated, completed the WPA handshake and obtained `192.168.4.150`; the provider now does that on a cold radio.

The following build restored the network at boot, but the next `wifi connect` wedged the App: `connect_and_save` disconnects first, the provider's disconnect was `wifi_off()`, and the following `wifi_on()` looped the lwIP assertion `netif already added` until a BLE `reboot loader`. Bench diagnostics showed that re-entering STA with `wifi_enter_sta_mode()` works from an associated or failed station and that `wifi_enter_smp_cfg_mode()` leaves the association while keeping the radio and lwIP up; STA disconnect now does that and never calls `wifi_off()`.

A timed repeat on the button App then reset the board mid-connect. The SDK exception log saved across the warm reset reported `stackoverflow` in `current_task : RtmpMlmeTask` with a 2796-byte user stack (the SDK demo's 700 words), raised while processing scan completion after the driver's cached-BSSID association was rejected (`CNTL - BSSID not found`). The five App targets now give `RtmpMlmeTask` 1400 words. On the previous build one display-App connect had also returned `TIMEOUT` at 15 s with `state=4` and the address arriving about a second later; that did not recur in any of the 13 connects on the final images below, so its cause is not established.

Two OpenAI review findings were also fixed: passwords longer than 63 bytes are rejected with `INVALID_ARG` before any disconnect, because both SDK copies keep at most 63 bytes, and a failed radio start after `CONNECTING` now reports `FAILED` instead of leaving the snapshot at `CONNECTING`.

## Final artifacts

The native build ran in OrbStack `embed-zig-noble-amd64` from this worktree after sourcing `/Users/idy/h2vivi/firmwares-devenv/export.sh` with `bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64-` on the display, button and loader package targets; the generated display task table contains `{"RtmpMlmeTask", 17u, 1400u, 0u, 0}`. Version strings are `bazel-native-artifacts`, so SHA-256 is the identity.

| Artifact | Package SHA-256 | `app/jieli/update.ufw` SHA-256 | Package / image bytes |
| --- | --- | --- | --- |
| display (color-bar) | `4e5a989b89653f149e0f3b896488b1f64ecdfe3377fbade17ded407c67a6a767` | `9ff146987f30e55cc24d81731026676717dbd91ad7d986d9b1a7b331ec7b5f5a` | 959807 / 970761 |
| button | `b15a8dc7bc318a86964eb01d4e9da87855c5e60bfcf67ccc58cfa07d5671e5f3` | `2ef55c5323728e8173debe29286b2f0154c80d36e301aff77fb35ba121ba1734` | 1373102 / 1388289 |
| loader (built, not installed) | `bbf258541273ba6f12b8b93a7996f9cdbb129087ea6be0a2bfb792e982640d98` | `d7766764257305a0876d387cb06c2467d0beea705e7f061ee9650cbee263a8b6` | 1022438 / 1033653 |

The host CLI was built with `bazel build --config=macos_arm64 //projects/h2loader/targets/cc_binary/cli:h2loader` at `185088fd`; its SHA-256 is `1e40af058a4095ff7a18377d01da6748ea36c369218a8d05183a457ca5b4f1d6`, and `libs/h2loader_host` and the CLI app are unchanged between that commit and `efbabcc3`. A CLI built from `main` rejects the new App status line with `FORMAT (-15)` because its parser refuses availability bits outside its own mask.

## Ordered results on the final images

| Step | Result | Facts |
| --- | --- | --- |
| 1: baseline | Recorded | The board already held the saved network `HAIVIVI-MFG` from the diagnostic builds; no forget or clear command exists, so it stayed. Independent status before install: App in P2, `stage_valid=0`, `last_result=0`. |
| 2: provision | PASS | Display App installed (`send` 34 s, `JIELI_APP_CONFIRM result=OK`). `wifi connect HAIVIVI-MFG <redacted>` rc=0 in 2 s with `H2_LOADER_WIFI result=connected ssid=HAIVIVI-MFG`; `wifi status`: `state=5 ip_valid=1 ip=192.168.4.150 rssi=-59 saved=1 saved_ssid_hex=484149564956492d4d4647`. |
| 2a: disconnect and reconnect | PASS | `wifi disconnect` rc=0 `result=disconnected`; status `state=6 ip_valid=0 ip=0.0.0.0 saved=1`; `wifi connect` again rc=0 in 2 s; status `state=5 ip=192.168.4.150`; coredump `stored_bytes=0 blank=1`. |
| 3: persistence | PASS | `reboot app`: console `JIELI_WIFI_RESTORE saved=1 result=0 ssid=HAIVIVI-MFG`, first `wifi status` 1 s later `state=5 ip_valid=1 ip=192.168.4.150`. `reboot loader` gave independent status `running_partition=1`, `active_checksum=ed7d71a6…`, `stage_valid=0`, `last_result=0`; `reboot app` restored the network again and reported `state=5 ip=192.168.4.150` on the first poll. |
| 4: wrong password | PASS | `wifi connect HAIVIVI-MFG wrong-password-454` rc=3 in 6 s with `H2_LOADER_WIFI result=error code=-4 state=7 disconnect_reason=11 ip_valid=0`; `wifi status` still `saved=1 saved_ssid_hex=484149564956492d4d4647`; independent status `last_result=0`; `reboot app` then gave `state=5 ip=192.168.4.150` on the first poll. |
| 5: unknown SSID | PASS | `wifi connect NO-SUCH-SSID-454 …` rc=3 in 17 s with `H2_LOADER_WIFI result=error code=-6 state=1 disconnect_reason=0 ip_valid=0`; the next `wifi status` showed `state=7 disconnect_reason=10` for that SSID and `saved_ssid_hex=484149564956492d4d4647` unchanged. |
| 6: second App | PASS | Button App installed from the Loader (`send` 45 s, `H2_JIELI_BUTTON_SMOKE_READY … result=0`, `JIELI_APP_CONFIRM result=OK`); its first boot restored the network (`state=5 ip=192.168.4.150`); `wifi connect` rc=0 in 4 s; `reboot app` restored it again on the first poll. |
| 7: final state | PASS | `reboot loader`; independent status `active_role=loader`, `running_partition=1`, `next_partition=1`, `active_checksum=ed7d71a6…`, `stage_valid=0`, `last_result=0`, P2 button image `2ef55c53…`; coredump `stored_bytes=0 blank=1`. The saved bench network was not cleared because no documented command exists. |

## Repeated connect

Before the ordered run, the display App on the final image ran ten back-to-back `wifi connect` calls over BLE while a UART monitor captured the console: all ten returned `result=connected` (4–10 s each including BLE session setup), the console showed ten `LINK UP` lines, zero `exception error`, zero `stackoverflow` and zero `netif already added`, the coredump partition stayed blank, and the final `wifi status` was `state=5 ip_valid=1 ip=192.168.4.150 saved=1`.

## Boundaries

All resets were software resets through Loader lifecycle commands; there was no power-loss test, so this round does not prove that the Pref write survives a power cut mid-commit. The Loader image has no Wi-Fi capability and was not provisioned. Runtime's saved-network set from #420 was not exercised because these launchers do not call `h2_runtime_init()`. AP start and stop still power-cycle the radio and are outside this change. Monitors in the ordered run were bounded at 75 s, so their exit code 124 is expected and not a board-health fact.
