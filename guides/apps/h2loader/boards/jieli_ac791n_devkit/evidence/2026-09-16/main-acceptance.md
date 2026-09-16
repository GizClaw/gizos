# Main acceptance round — 2026-09-16

## Outcome and source

**Incomplete: native builds, distinct-image Loader self-update and PAL 10/10 passed; the UART lifecycle runner was cancelled after 20 passing cases and reported FAIL.** The remaining five UART cases and both BLE rounds, button and audio-system hardware checks were not executed. No hardware retry or workaround followed the failing suite summary; the cancelling signal's origin is unknown. This record does not establish a board fault or a main-source regression.

Tested firmware and host source is origin/main `667cd92585a92bcc81001090bbf90a5ed907ab10`, verified against the remote at the start of this round. Build checkout was on `jieli-main-acceptance` at documentation-only commit `9c1a0e58`, whose diff from that main revision contains only this evidence page and its board-guide link. No production source or suite code was modified. The pre-existing untracked `lock` file was preserved.

## Ordered results

| Step | Result | Result codes and coverage |
| --- | --- | --- |
| 1: build | PASS | Six native packages built, exit 0, 151.208 s; host CLI and runner built, exit 0, 0.746 s. |
| 2: install-loader | PASS | Distinct-image P2 trial, confirmation, P2-to-P1 copy-back; independent P1/P2 package and image identities match the new Loader, Stage empty, last_result=0. |
| 3: PAL | PASS | Ten distinct case results captured, all 0; aggregate `result=0 passed=10 failed=0`; verified P2 identity and returned to original/new P1 Loader. |
| 4: UART lifecycle | INCOMPLETE / runner FAIL | 20/25 executed, all 20 PASS with case rc=0; summary `result=FAIL rc=1 cases=20 passed=20 failed=0 elapsed_ms=273566`; process exit 130. |
| 5: BLE lifecycle twice | NOT RUN | 0/22 cases in each planned round; no result codes. |
| 6: button App | NOT RUN | Package built; READY/confirmation not observed because installation was not attempted. |
| 7: audio-system | NOT RUN | Package built; READY, 30-second streaming and retained Runtime not verified. |
| 8: final independent state | SAFE STOP STATE VERIFIED | P1 and P2 retain the new Loader, Stage empty, last_result=0; this does not complete the omitted acceptance steps. |

## Builds and immutable artifacts

The native build used OrbStack `embed-zig-noble-amd64`, sourced `/Users/idy/h2vivi/firmwares-devenv/export.sh`, unset `IDF_PATH H2LOADER_IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_PATH`, and ran the following from the worktree:

```sh
bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build \
  --config=ac791n --symlink_prefix=bazel-amd64- \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package \
  //projects/e2e/targets/h2loader_tar_zlib/pal/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_tar_zlib/button/jieli_ac791n_devkit:button_package \
  //projects/example/targets/h2loader_tar_zlib/audio-system/jieli_ac791n_devkit:audio_system_package \
  //projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_tar_zlib/crash-before-confirm/jieli_ac791n_devkit:package
```

All six outputs were copied from the VM immediately after successful build, frozen outside the worktree, and hashed together with their decompressed `app/jieli/update.ufw` contents. All packages target `jieli_ac791n_devkit` / `wl82`; Loader is the Loader role and the other five are App packages. Runtime version strings are `bazel-native-artifacts`, so SHA-256 is the identity evidence.

| Artifact | Package SHA-256 | `app/jieli/update.ufw` SHA-256 | Package / image bytes |
| --- | --- | --- | --- |
| loader | `d3cfaf7229b7b0d230c281ca0d1a6add06d497a7b8740df01eac0192c92730c0` | `28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f` | 928062 / 938857 |
| pal | `84ac89f2d9ff21ebbfa8240bfee7be4f48f672a0b995ce6e92233facab76f2b8` | `af2c7593f927fa545d375931fc84f5a75d7f4a45c3af213ece6f633c2783b91d` | 891152 / 902249 |
| button | `b01937d9c3bcd6287b49220b0901fe6c5a48050e9295a52266c8818cfd3b07e2` | `eb5f587ff952469649d6c0e03915c104a2413e8e11f61f9a5ff60e9f0c57a0dd` | 1278347 / 1292785 |
| audio | `6ba539c95c8881be7f315a7abd93072b1d48df27ef02a841895848a2205be858` | `c7112d145bfe4347af5e54e4b414e1f28b957b58a546e273af9cb6fe8288e9ae` | 1078818 / 988157 |
| display | `2fe72e699adcecb7564f38d9698e51caa3bace6a446e540d2d53f21ad9256def` | `3456983635c01aba34669641ec95128c778429d5d52dac9849ca04e48925b060` | 865780 / 876637 |
| crash | `67adb3dc0fc3a3d421cfe7d500ae70a0382ddfc8888b555278c0cc3f2749719e` | `5d861924a352c30279548a6dd0d9bfec4c5bfa558ad09bbb5b36f06bd44035ad` | 896644 / 907645 |

Host command: `bazel build --config=macos_arm64 //projects/h2loader/targets/cc_binary/cli:h2loader //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner`. CLI binary SHA-256 is `b360042996b46fff044e62cd70c73dc7491e54a285cec6b4ec4db33a35db5a8d`; runner binary SHA-256 is `3054e8a0e0b1ad02cd2ec9a3487e3000bb8571d95f797be24ecd34fcfa25b223`.

The initial native invocation used nonexistent button `:package` instead of `:button_package`; it exited 1 during target loading, 8.969 s, 0 processes. The reviewer clarified that execution typos do not trigger the board/suite stop rule. All labels, including audio-system `:audio_system_package`, were checked in their BUILD files before the successful build. No native compilation or hardware failure is inferred from that initial invocation.

## Loader self-update

The initial Loader image was `eef1e22a75a51174843937dba6ad155be2f1a3bbda16113eb5fbd9243caa4017`, package `f96cf7e5ec91b66aa7aba0c2ff1c46c710f7784c24419ffca3ebd8fcdf9fea7a`. UART `send --file` completed with rc=0 in 31.077 s; independent staged status matched the new Loader package/image before `reboot upgrade --monitor`.

The ordinary monitor captured `H2_JIELI_LOADER_TRIAL confirmed=1 publish_gate=before-copy-p1`, startup events 1, 3, 2 and 4 with code 0, and shadow reads from P2 and P1. The monitor was deliberately stopped with SIGINT after 65.241 s (exit 130); its exit is not a board-health check. The subsequent independent status verified both valid partitions as the new Loader, empty Stage and last_result=0, proving the normal self-update converged. This is distinct-image self-update; the later UART suite's install-loader case reinstalls the same image.

## PAL case capture

The standard PAL package was installed through UART `send --file` (rc=0, 29.240 s) and `reboot upgrade --monitor`, with no added tracing, code changes or diagnostic probes. The first capture segment contained binary/garbled bytes and omitted Filesystem 13 and Core 1. The same uninterrupted monitor later received the PAL target's built-in value-only ledger replay, which supplied every required line; this was not a second test run, reinstall or reboot. Initial output is therefore incomplete, but the final retained ledger has no missing required case.

```text
H2_PAL_E2E suite=64 case=13 result=0
H2_PAL_E2E suite=1 case=1 result=0
H2_PAL_E2E suite=1 case=2 result=0
H2_PAL_E2E suite=1 case=3 result=0
H2_PAL_E2E suite=1 case=4 result=0
H2_PAL_E2E suite=1 case=5 result=0
H2_PAL_E2E suite=1 case=6 result=0
H2_PAL_E2E suite=1 case=10 result=0
H2_PAL_E2E suite=1 case=11 result=0
H2_PAL_E2E suite=32 case=27 result=0
H2_PAL_E2E result=0 passed=10 failed=0
```

The PAL monitor was deliberately stopped with SIGINT after 66.051 s and exited 130. Independent status verified the PAL image running in P2, last_result=0 and Stage valid. This PAL diagnostic intentionally does not confirm its trial: its source retains Stage and lets reset recover to Loader. A temporary host helper incorrectly expected normal-App Stage clearing and exactly ten raw log occurrences despite ledger replay; it raised an assertion after the capture. Read-only source inspection and the already captured ten distinct results corrected that helper interpretation without changing or rerunning the board test. An explicit UART `reboot loader` then returned to the new P1 in 2.505 s; independent status retained the PAL Stage as designed. The following UART suite's ordinary send/abort cases replaced and cleared that Stage.

## UART lifecycle stop

The unmodified runner was invoked with the following options; package paths below identify the frozen artifacts in the table, and the report was written outside the worktree:

```sh
e2e-runner --uart /dev/cu.usbserial-20131240 --baud 460800 \
  --expected-board jieli_ac791n_devkit --expected-target wl82 \
  --app-firmware /tmp/jieli-main-acceptance-2026-09-16/display.tar.zlib \
  --loader-firmware /tmp/jieli-main-acceptance-2026-09-16/loader.tar.zlib \
  --crash-firmware /tmp/jieli-main-acceptance-2026-09-16/crash.tar.zlib \
  --monitor-ms 3000 --report /tmp/jieli-main-acceptance-2026-09-16/uart.json
```

The outer host wrapper watched the runner output to stop on the first failing case or 75 seconds without output, with the runner's default 120000 ms command/connect settings unchanged. Neither outer stop condition fired: `first_failure_stop=False no_output_stop=False`. The runner finished install-loader successfully, then emitted a FAIL summary and exited 130, with no crash/coredump case started. The 273.566 s suite elapsed time and 273.867 s outer elapsed time do not establish when the signal arrived.

Source inspection identifies exit 130 as the CLI cancellation path: SIGINT or SIGTERM sets `g_cancelled`; the runner skips crash/coredump execution when cancelled and turns a previously successful aggregate into `H2_PAL_EXIT` (rc=1). The captures do not identify the signal sender or distinguish SIGINT from SIGTERM. No claim is made that a board case failed, that a command timed out, or that this establishes a main regression. The incomplete suite is not accepted as 25/25 and was not rerun.

| UART case | Result | rc | Elapsed ms |
| --- | --- | --- | --- |
| help | PASS | 0 | 512 |
| status | PASS | 0 | 490 |
| stats | PASS | 0 | 800 |
| legacy-commands-absent | PASS | 0 | 510 |
| send | PASS | 0 | 32000 |
| stage-abort-after-send | PASS | 0 | 990 |
| monitor | PASS | 0 | 3301 |
| reboot-loader-monitor | PASS | 0 | 8300 |
| reboot-upgrade-monitor | PASS | 0 | 60334 |
| reboot-app-monitor | PASS | 0 | 5694 |
| app-help | PASS | 0 | 1120 |
| app-status | PASS | 0 | 490 |
| app-stats | PASS | 0 | 960 |
| app-memory | PASS | 0 | 940 |
| app-legacy-commands-absent | PASS | 0 | 700 |
| app-send | PASS | 0 | 31620 |
| app-stage-abort-after-send | PASS | 0 | 970 |
| reboot-app-preserves-stage | PASS | 0 | 34575 |
| reboot-loader-preserves-stage | PASS | 0 | 32970 |
| install-loader | PASS | 0 | 56290 |
| install-crash-app | NOT RUN | unavailable | unavailable |
| coredump-status | NOT RUN | unavailable | unavailable |
| coredump-dump | NOT RUN | unavailable | unavailable |
| coredump-erase | NOT RUN | unavailable | unavailable |
| coredump-status-after-erase | NOT RUN | unavailable | unavailable |

## Independent UART snapshots

Every snapshot below came from a standalone `h2loader --no-ble --port /dev/cu.usbserial-20131240 --baud 460800 status` command after the active host monitor/runner had exited. `lsof` found no remaining UART reader before each poll; no stale reader needed killing. Every poll exited 0 and reported UID `d879349abc9f`, board `jieli_ac791n_devkit`, target `wl82`, valid P1/P2 metadata and last_result=0. Runner-internal statuses are not substituted for these health checks.

In the table, `old-loader` denotes package `f96cf7e5ec91b66aa7aba0c2ff1c46c710f7784c24419ffca3ebd8fcdf9fea7a` / image `eef1e22a75a51174843937dba6ad155be2f1a3bbda16113eb5fbd9243caa4017`; `old-app` denotes package `b1fde0e35f8f2d74c65b3fba3aa9270b937a2ca14ca938ff488952e5a5421839` / image `111475df6c2d2aa7509683f08bf204a7ffd945d1e2d5fa75ea747780c79cf033`. `loader` and `pal` refer to the exact package/image SHA pairs in the artifact table above. Stage references identify the same exact pair, not merely a role or version.

| Checkpoint | Active role | Running / next | Boot intent | Stage valid / identity | P1 identity | P2 identity | last_result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| stopped-independent | loader | 1 / 1 | loader | 0 / empty | old-loader | old-app | 0 |
| loader-before | loader | 1 / 1 | loader | 0 / empty | old-loader | old-app | 0 |
| loader-staged | loader | 1 / 1 | loader | 1 / loader | old-loader | old-app | 0 |
| loader-after | loader | 1 / 1 | auto | 0 / empty | loader | loader | 0 |
| pal-before | loader | 1 / 1 | auto | 0 / empty | loader | loader | 0 |
| pal-staged | loader | 1 / 1 | auto | 1 / pal | loader | loader | 0 |
| pal-after | app | 2 / 2 | auto | 1 / pal | loader | pal | 0 |
| pal-returned | loader | 1 / 1 | loader | 1 / pal | loader | pal | 0 |
| uart-before | loader | 1 / 1 | loader | 1 / pal | loader | pal | 0 |
| uart-after | loader | 1 / 1 | auto | 0 / empty | loader | loader | 0 |
| final | loader | 1 / 1 | auto | 0 / empty | loader | loader | 0 |

Final independent status, after stopping the UART suite and without any further installation or reset:

```text
H2_LOADER_STATUS board=jieli_ac791n_devkit target=wl82 chip=ac791n device_uid=d879349abc9f capabilities=0x00000005 command_availability=0x00081d1e active_role=loader active_version=bazel-native-artifacts active_checksum=28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f active_image_size=938857 running_partition=1 next_partition=1 boot_intent=auto stage_valid=0 stage_package_checksum=- stage_package_size=0 stage_image_checksum=- stage_image_size=0 stage_role=unknown stage_version=- stage_board=- stage_target=- partition_1_valid=1 partition_1_package_checksum=d3cfaf7229b7b0d230c281ca0d1a6add06d497a7b8740df01eac0192c92730c0 partition_1_package_size=928062 partition_1_image_checksum=28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f partition_1_image_size=938857 partition_1_role=loader partition_1_version=bazel-native-artifacts partition_1_board=jieli_ac791n_devkit partition_1_target=wl82 partition_2_valid=1 partition_2_package_checksum=d3cfaf7229b7b0d230c281ca0d1a6add06d497a7b8740df01eac0192c92730c0 partition_2_package_size=928062 partition_2_image_checksum=28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f partition_2_image_size=938857 partition_2_role=loader partition_2_version=bazel-native-artifacts partition_2_board=jieli_ac791n_devkit partition_2_target=wl82 last_result=0 mfg_mode=1 mfg_steps=0000000000000000000000
```

## Limits and skipped work

Wi-Fi credential persistence was explicitly skipped because no bench AP credentials were supplied. PAL Wi-Fi case 27 is offline coverage only. No connected Wi-Fi/AP or HTTP cases were enabled in the lifecycle runner.

Both planned BLE 22-case rounds were skipped after the UART stop; no Terminal.app BLE invocation or BLE installation occurred, and endpoint `5:818f070641f0` was not revalidated. Button and audio-system packages were built but not installed; there is no evidence here for their READY/confirmation, physical input, audio streaming, acoustic quality, or Runtime-retaining entry behavior. The crash-before-confirm package was built but its UART case was never reached.

All resets performed were Loader-command software resets. There was no power cut, manual power cycling, USB DL, format operation, other serial port access or P1 modification outside the normal Loader self-update flow. No independent status timeout or 90-second unresponsive post-reset interval was observed. Early PAL capture loss was filled by the same run's ledger replay, but the initial boot transcript remains incomplete.

Raw commands, build logs, monitor logs, runner report, helper scripts and status captures remain outside the worktree under `/tmp/jieli-main-acceptance-2026-09-16`. No raw `.log`, `.status` or `.json` capture from this round was added to the worktree. All review-relevant identities, status checkpoints, counts, result codes and limits are retained inline here.

## Documentation validation

`bazel test --config=macos_arm64 //guides:guides_build_test //tools/bazel:jieli_evidence_test` passed both targets with exit 0 (guides test cached, evidence test freshly executed); the documentation build completed successfully. `git diff --check` passed. These checks validate the record, not the skipped hardware acceptance steps.
