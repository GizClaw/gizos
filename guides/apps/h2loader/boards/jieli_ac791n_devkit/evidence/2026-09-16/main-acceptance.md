# Main acceptance round — 2026-09-16

## Outcome and source

**FAIL: button image startup log capture.** Loader self-update, PAL 10/10, UART 25/25, BLE 22/22 twice and audio-system smoke passed. Both CLI button captures lost READY and confirmation text after input-start result=0 despite correct independent P2 state; that state does not substitute for the missing lines. The reviewer authorized audio, final status and focused raw-reader diagnosis after this failed item. Final independent status verifies the new P1 Loader, Stage empty and last_result=0.

Tested firmware and host source is origin/main `667cd92585a92bcc81001090bbf90a5ed907ab10`, verified against the remote at the start of this round. Build checkout was on `jieli-main-acceptance` at documentation-only commit `9c1a0e58`; the continuation began at documentation-only commit `a7148fd9`. Both diffs from that main revision contain only this evidence page and its board-guide link; the same frozen packages and host binary bytes were used throughout. No production source or suite code was modified. The pre-existing untracked `lock` file was preserved.

## Ordered results

| Step | Result | Result codes and coverage |
| --- | --- | --- |
| 1: build | PASS | Six native packages built, exit 0, 151.208 s; host CLI and runner built, exit 0, 0.746 s. |
| 2: install-loader | PASS | Distinct-image P2 trial, confirmation, P2-to-P1 copy-back; independent P1/P2 package and image identities match the new Loader, Stage empty, last_result=0. |
| 3: PAL | PASS | Ten distinct case results captured, all 0; aggregate `result=0 passed=10 failed=0`; verified P2 identity and returned to original/new P1 Loader. |
| 4: UART lifecycle | PASS | Complete detached run: 25/25, rc=0, process exit 0, 329.006 s. Earlier externally interrupted partial run: 20/20 PASS, process exit 130. |
| 5: BLE lifecycle twice | PASS | Terminal.app: 22/22, rc=0, exit 0, 372.900 s; then 22/22, rc=0, exit 0, 369.925 s. |
| 6: button image startup log capture | FAIL | Correct P2 App, Stage empty and last_result=0 twice; READY and confirmation text missing twice after input-start result=0. |
| 7: audio-system | PASS | READY captured; 30.801 s subsequent streaming observation, 24 microphone peak reports; independent active P2 App, Stage empty, last_result=0; returned to P1. |
| 8: final independent state | PASS | New P1 Loader, Stage empty, last_result=0 after audio; repeated after focused button diagnosis, with P2 retaining button App. Overall round retains the failed button item. |

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

### First-pass bytes and ledger replay

The raw monitor capture SHA-256 is `e8185f99ac8f5acd6c6fb9ba0f433aeb8e32a68625e3f33838daf007bd3f47cd` (32152 bytes). A context-preserving hex dump is saved outside the worktree as `/tmp/jieli-main-acceptance-2026-09-16/pal-first-pass-garble.hex.txt`; byte offsets are zero-based in that capture. The corrupt span is `[30100, 30176)` (`0x7594` through `0x75df`), 76 bytes, after `H2_PAL_FS op=mkdir phase=enter` and before `H2_PAL_E2E suite=1 case=2 result=0`.

```text
00007594  f7 fc f0 f0 fe ff fe ed fb ff fc fe fc ea e3 fc
000075a4  fe fa ae fa ff da fe d4 fa fc c1 8c c2 8c e3 fa
000075b4  d0 aa 71 18 fa aa 6e 70 5c 55 fe 53 89 ba 0a 5a
000075c4  87 02 96 8d 58 38 ac 7c d0 8a fc dc 42 76 de 42
000075d4  0b d6 ec 8a e0 67 02 6f da 95 8e ff
```

The frame implementation defines six-byte magic `H2IKCP`, an 18-byte header, payload length at offset 12 and IEEE CRC-32 at offset 14 over the payload. The complete captured CLI output contains zero such magic candidates, so no CRC-valid frame encloses this garble in the saved capture: these bytes are in the monitor's text output stream. This is post-filter CLI output, not a pre-parser UART wire recording; it cannot prove whether corruption occurred on the wire, in a frame discarded before output, or in text handling. No CRC-valid-frame or root-cause claim is made beyond the observed output classification.

| Capture segment | Case lines captured | Aggregate |
| --- | --- | --- |
| First segment before first aggregate | Core 2/3/4/5/6/10/11 and Wi-Fi 27: eight lines, all result=0; Filesystem 13 and Core 1 absent | result=0 passed=10 failed=0 |
| First complete replay | Filesystem 13, Core 1/2/3/4/5/6/10/11 and Wi-Fi 27: all ten lines, all result=0 | result=0 passed=10 failed=0 |
| Following three replays | All ten lines in each replay | result=0 passed=10 failed=0 each |

The complete UART rerun emitted all 25 start/completion/result records with no non-text bytes in its runner output, and all four monitor cases passed. The runner counts monitor bytes but does not persist their raw text (`on_log=null`), so this does not prove that every device boot-text line was captured. No new dropped-text symptom was observed to trigger the conditional PAL rerun; PAL was not rerun.

## Original UART partial run and interrupt source

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

Source inspection identifies exit 130 as the CLI cancellation path: SIGINT or SIGTERM sets `g_cancelled`; the runner skips crash/coredump execution when cancelled and turns a previously successful aggregate into `H2_PAL_EXIT` (rc=1). Subsequent host-session audit identified the external command below. Exit 130 alone does not distinguish SIGINT from SIGTERM in this CLI. This partial run is not accepted as 25/25 and is not a board or suite case failure; the reviewer-authorized full rerun is recorded separately.

The interrupt source was a concurrent Claude Bash session `39bf46d4-a14a-406c-8c3f-474ffc5da31f`, which executed `pkill -f "h2loader.*monitor"` at **2026-09-16 14:02:35.837 UTC** while operating another device. Its full-command regex matches this runner's path component `h2loader` and option `--monitor-ms 3000`; default `pkill` delivery is SIGTERM. The UART run started at approximately 13:58:10 UTC and completed its last case/summary around 14:02:44 UTC. The shell command, timestamp, session identifier and exact argv match identify the host-side cause; a kernel signal-sender PID was not captured. The runner contains no self-raise/send path for these signals.

The original launch was `python3 /tmp/jieli-main-acceptance-2026-09-16/suite.py uart > /tmp/jieli-main-acceptance-2026-09-16/uart-run.log 2>&1` through `exec_command`, with `yield_time_ms=1000` (yield interval, not a timeout). There was no `timeout`, `read -t`, Terminal/osascript session, detached background job or observed HUP for that run. The wrapper recorded neither of its own stop conditions. Therefore the earlier classification as an unexplained suite failure is superseded by a host-side externally interrupted partial run.

```text
H2_LOADER_E2E_CASE state=complete transport=uart iteration=1 name=install-loader result=PASS rc=0 elapsed_ms=56290 bytes=928062 total=928062
H2_LOADER_E2E result=FAIL rc=1 cases=20 passed=20 failed=0 elapsed_ms=273566
suite_exit 130 elapsed_s 273.867 first_failure_stop False no_output_stop False
```

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

## Complete UART and BLE lifecycle runs

The UART rerun was launched by a detached Python supervisor with `stdin=DEVNULL`, redirected output under `/tmp`, and `start_new_session=True`; the runner itself also owns a new session/process group. There is no outer timeout or no-output deadline, and the suite retains its default 120000 ms connect/command budget. The supervisor only sends a stop signal after a genuinely failing case; none occurred. The tool launch returned immediately and progress was polled from files.

To avoid recurrence of the identified broad process-name match, the unchanged host executables were copied to `/tmp/jieli-main-acceptance-2026-09-16/ac791n-lifecycle-runner` and `ac791n-cli`; SHA-256 matches the built originals listed above. Package paths and argv contain no `h2loader.*monitor` match. A setup-only `--help` invocation initially exposed the runner's relative dylib/runfiles dependency; its original `e2e-runner.runfiles` directory was linked beside the copy and `--help` passed before the successful full run. An attempted start before that setup correction failed in dyld before runner main or any case; it performed no suite device operation and is retained separately as `uart-launch-setup` outside the worktree.

Both BLE runs were launched through Terminal.app using AppleScript `do script`, with output redirected under `/tmp`; neither used UART monitor cases or Wi-Fi/URL options. Before each BLE suite, a Terminal-launched BLE status at `5:818f070641f0` matched UID `d879349abc9f` and P1 identity against independent UART status. BLE installation was exercised by the ordinary full suite. Independent UART snapshots were taken after each suite, with no simultaneous UART reader.

| Suite | Cases | Passed / failed | Aggregate rc / process exit | Seconds |
| --- | --- | --- | --- | --- |
| uart-full | 25 | 25 / 0 | 0 / 0 | 329.006 |
| ble-1 | 22 | 22 / 0 | 0 / 0 | 372.900 |
| ble-2 | 22 | 22 / 0 | 0 / 0 | 369.925 |

| Case | UART full (rc / ms) | BLE 1 (rc / ms) | BLE 2 (rc / ms) |
| --- | --- | --- | --- |
| help | 0 / 722 | 0 / 1979 | 0 / 2663 |
| status | 0 / 501 | 0 / 2638 | 0 / 4301 |
| stats | 0 / 770 | 0 / 2623 | 0 / 2539 |
| legacy-commands-absent | 0 / 709 | 0 / 2297 | 0 / 2460 |
| send | 0 / 28920 | 0 / 29326 | 0 / 29366 |
| stage-abort-after-send | 0 / 770 | 0 / 3526 | 0 / 2659 |
| monitor | 0 / 3290 | not a case for this transport | not a case for this transport |
| reboot-loader-monitor | 0 / 8061 | not a case for this transport | not a case for this transport |
| reboot-upgrade-monitor | 0 / 55783 | not a case for this transport | not a case for this transport |
| reboot-app-monitor | 0 / 6185 | not a case for this transport | not a case for this transport |
| app-help | 0 / 719 | 0 / 2402 | 0 / 2700 |
| app-status | 0 / 500 | 0 / 2279 | 0 / 2581 |
| app-stats | 0 / 981 | 0 / 2549 | 0 / 2559 |
| app-memory | 0 / 939 | 0 / 2730 | 0 / 2391 |
| app-legacy-commands-absent | 0 / 720 | 0 / 6691 | 0 / 2638 |
| app-send | 0 / 29210 | 0 / 29507 | 0 / 31469 |
| app-stage-abort-after-send | 0 / 630 | 0 / 2226 | 0 / 2460 |
| reboot-app-preserves-stage | 0 / 34915 | 0 / 40687 | 0 / 41676 |
| reboot-loader-preserves-stage | 0 / 33841 | 0 / 36278 | 0 / 37017 |
| install-loader | 0 / 56700 | 0 / 59702 | 0 / 58410 |
| install-crash-app | 0 / 60150 | 0 / 69350 | 0 / 72722 |
| coredump-status | 0 / 510 | 0 / 4291 | 0 / 2269 |
| coredump-dump | 0 / 920 | 0 / 2380 | 0 / 2650 |
| coredump-erase | 0 / 2050 | 0 / 3737 | 0 / 4021 |
| coredump-status-after-erase | 0 / 510 | 0 / 2613 | 0 / 2158 |
| install-app | not a case for this transport | 0 / 63089 | 0 / 58216 |

## Button image startup log capture: failed item

The button package was installed once through UART Loader into P2. The ordinary install monitor captured `H2_JIELI_BUTTON_SMOKE stage=runtime-init result=0` and `H2_JIELI_BUTTON_SMOKE stage=input-start result=0`, followed by bytes `fe e4 a4`; neither `H2_JIELI_BUTTON_SMOKE_READY` nor `JIELI_APP_CONFIRM result=OK` was present anywhere in the capture. Monitoring was deliberately stopped after 65.273 s (exit 130). Independent status verified active App P2 with the exact button image, Stage empty and last_result=0. This establishes persisted install/confirmation state, but does not substitute for the requested text evidence.

Following the board's previously documented capture procedure, the App was returned to the new P1 and then normally relaunched once with `reboot app --monitor`, without reinstalling it. This capture again reached input-start result=0, then bytes `7a fe e1 de d8`, and again lacked both required markers. Monitoring was stopped after its 45-second observation window; independent status again verified active button App P2, Stage empty and last_result=0. No process cancellation or failed Runtime/input result is used as the explanation for these missing lines.

| Capture | Raw SHA-256 | Non-text span (zero-based offsets) | Bytes | Required markers |
| --- | --- | --- | --- | --- |
| button-upgrade | `841e254948424c019f72b894333f88ee44702964acbdb7a6fcc2c01628255ff6` | [45564, 45567) | `fe e4 a4` | READY absent; confirmation absent |
| button-relaunch | `a6574e6f0f63f4ccf58dc8284ac97865c3142ec37aac69cc7a778f18ce58e9cc` | [13587, 13592) | `7a fe e1 de d8` | READY absent; confirmation absent |

Context hex dumps are retained only under `/tmp/jieli-main-acceptance-2026-09-16/button-upgrade-garble.hex.txt` and `button-relaunch-garble.hex.txt`. The following SDK startup text appears after these direct status writes; its order is not treated as independent proof of an extra reset or board failure. Source inspection locates READY immediately before target-entry release and confirmation immediately after successful target return; the captures do not establish why those writes were lost. The observed button-task event output and independent active App status are retained as positive evidence, without claiming controlled physical button testing.

Step 6 is explicitly failed as **button image startup log capture**. The reviewer authorized continuation through audio and final status before a focused raw-reader diagnosis; no source changes were made.

## Audio-system smoke

The frozen audio package was installed through UART Loader into P2. The monitor captured `H2_JIELI_AUDIO_SYSTEM_READY mic=1 speaker=1 aec=dac-software-ref`, then observed 30.801 seconds of continued streaming with 24 `H2_SMOKE_AUDIO_MIC peak=` reports. The 63.002-second install/monitor invocation ended with deliberately requested SIGINT (exit 130) after the observation window. Independent status after closing the monitor showed the exact audio image still active as App in P2, Stage empty and last_result=0, establishing that the Runtime-retaining entry left the image running. This is streaming/log and independent-state evidence, not a listening test of acoustic quality.

`reboot loader` returned to the unchanged new P1; both audio-returned and the subsequent step-8 independent status showed running/next P1, Stage empty and last_result=0, with the audio image retained in P2. Only after these checks was the same frozen button package restored through Loader into P2 for the requested diagnosis.


## Open issue: button image startup log capture

**Host monitor text-path defect: open, no code fix attempted.** The corrected raw UART capture contains both required lines intact immediately after input-start result=0, while both CLI monitor captures lost them at that location and emitted short garbage spans. This establishes that the board emits the bytes and isolates the observed button loss to the host monitor path under this comparison. It is the same family of text-loss symptom as PAL's 76-byte garble; an identical underlying cause for PAL is not established by this button-only experiment. The diagnostic success does not change the failed button acceptance item.

After audio and step 8, the same frozen button package was restored through the UART Loader into P2, independently verified as active App with Stage empty and last_result=0, and returned to the new P1 before the diagnostic boot. Exact-device `lsof` found no stale UART reader; no kill was needed. The raw method was `/bin/cat /dev/cu.usbserial-20131240 > /tmp/jieli-main-acceptance-2026-09-16/button-raw-boot.bin`, with cat already holding the port while `stty -f /dev/cu.usbserial-20131240 460800 raw -echo -hupcl clocal cs8 -parenb -cstopb -ixon -ixoff -crtscts min 1 time 0` configured it. A subsequent `stty -a` verified `speed 460800 baud` before the reboot trigger. Cat PID 50204 was the sole UART owner.

A separate CLI invocation launched through Terminal.app first checked BLE UID and both partition identities, then issued `ac791n-cli --transport bleikcp --port 5:818f070641f0 reboot app` (exit 0). It never opened UART. Cat captured before this trigger and for 40.250 seconds afterward, covering the complete boot, then was deliberately terminated by its exact PID (SIGTERM, exit -15). Independent UART status was taken only after cat exited. This avoids competing UART readers and the sequential-reboot late-open capture gap.

The first raw-reader setup attempt configured stty before cat opened the port; closing/reopening the device reverted the speed to 9600, as the saved settings show. Its garbage-only capture is invalid for host/device attribution and is preserved under `/tmp/jieli-main-acceptance-2026-09-16/raw-setup-9600/`. That attempt also performed a software App reboot and returned to independently verified P1. The corrected setup above therefore required one additional diagnostic reboot; this is a disclosed host setup error, not a device or suite failure. There was one valid full-boot raw comparison at verified 460800.

The valid raw capture is 35245 bytes, SHA-256 `c73911092640dd0e49aac18a84c7deaec14de321c5ea0afc7ee739ae3b82e230`. Input-start begins at byte 16554, READY at 16604, and confirmation at 16695. The region is intact CRLF-delimited ASCII, with no truncation, interleaving or garbage between these lines; the whole capture contains no `H2IKCP` frame magic. Context hex dumps for both CLI captures and the raw capture are retained under `/tmp/jieli-main-acceptance-2026-09-16/` as `button-upgrade-garble.hex.txt`, `button-relaunch-garble.hex.txt` and `button-raw-gap.hex.txt`.

```text
H2_JIELI_BUTTON_SMOKE stage=input-start result=0
H2_JIELI_BUTTON_SMOKE_READY buttons=8 display=480x320 result=0
JIELI_TARGET_APP result=0
JIELI_APP_CONFIRM result=OK code=0 target=0 transport=0
```

The [2026-09-15 16-byte torn-header recovery run](../2026-09-15/p2-header-reinstall.md) also lost these same two lines, so the CLI capture symptom is reproducible across rounds. The current raw comparison narrows the button issue to the host path; it does not retrospectively supply missing acceptance text to the older run.

After the valid raw boot, independent status verified active button App P2, Stage empty and last_result=0. An explicit UART `reboot loader` returned to the unchanged new P1; both raw-button-returned and round-final independent snapshots confirmed Loader P1, next P1, Stage empty and last_result=0. No source code was changed during diagnosis.

## Independent UART snapshots

Every snapshot below came from a standalone `h2loader --no-ble --port /dev/cu.usbserial-20131240 --baud 460800 status` command after the active host monitor/runner had exited. `lsof` found no remaining UART reader before each poll; no stale reader needed killing. Every poll exited 0 and reported UID `d879349abc9f`, board `jieli_ac791n_devkit`, target `wl82`, valid P1/P2 metadata and last_result=0. Runner-internal statuses are not substituted for these health checks.

In the table, `old-loader` denotes package `f96cf7e5ec91b66aa7aba0c2ff1c46c710f7784c24419ffca3ebd8fcdf9fea7a` / image `eef1e22a75a51174843937dba6ad155be2f1a3bbda16113eb5fbd9243caa4017`; `old-app` denotes package `b1fde0e35f8f2d74c65b3fba3aa9270b937a2ca14ca938ff488952e5a5421839` / image `111475df6c2d2aa7509683f08bf204a7ffd945d1e2d5fa75ea747780c79cf033`. `loader`, `pal`, `display`, `crash`, `button` and `audio` refer to the exact package/image SHA pairs in the artifact table above. Stage references identify the same exact pair, not merely a role or version.

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
| reviewer-current | loader | 1 / 1 | auto | 0 / empty | loader | loader | 0 |
| uart-launch-setup-before | loader | 1 / 1 | auto | 0 / empty | loader | loader | 0 |
| uart-launch-setup-after | loader | 1 / 1 | auto | 0 / empty | loader | loader | 0 |
| uart-full-before | loader | 1 / 1 | auto | 0 / empty | loader | loader | 0 |
| uart-full-after | loader | 1 / 1 | auto | 1 / crash | loader | crash | 0 |
| ble-1-before | loader | 1 / 1 | auto | 1 / crash | loader | crash | 0 |
| ble-1-after | loader | 1 / 1 | auto | 1 / crash | loader | crash | 0 |
| ble-2-before | loader | 1 / 1 | auto | 1 / crash | loader | crash | 0 |
| ble-2-after | loader | 1 / 1 | auto | 1 / crash | loader | crash | 0 |
| button-before | loader | 1 / 1 | auto | 1 / crash | loader | crash | 0 |
| button-staged | loader | 1 / 1 | auto | 1 / button | loader | crash | 0 |
| button-after | app | 2 / 2 | auto | 0 / empty | loader | button | 0 |
| button-loader-before-relaunch | loader | 1 / 1 | loader | 0 / empty | loader | button | 0 |
| button-after-relaunch | app | 2 / 2 | auto | 0 / empty | loader | button | 0 |
| button-returned | loader | 1 / 1 | loader | 0 / empty | loader | button | 0 |
| continuation-final | loader | 1 / 1 | loader | 0 / empty | loader | button | 0 |
| audio-before | loader | 1 / 1 | loader | 0 / empty | loader | button | 0 |
| audio-staged | loader | 1 / 1 | loader | 1 / audio | loader | button | 0 |
| audio-after | app | 2 / 2 | auto | 0 / empty | loader | audio | 0 |
| audio-returned | loader | 1 / 1 | loader | 0 / empty | loader | audio | 0 |
| step8-after-audio | loader | 1 / 1 | loader | 0 / empty | loader | audio | 0 |
| raw-button-restore-staged | loader | 1 / 1 | loader | 1 / button | loader | audio | 0 |
| raw-button-restored-app | app | 2 / 2 | auto | 0 / empty | loader | button | 0 |
| raw-button-ready-loader | loader | 1 / 1 | loader | 0 / empty | loader | button | 0 |
| raw-button-before | loader | 1 / 1 | loader | 0 / empty | loader | button | 0 |
| raw-button-after | app | 2 / 2 | auto | 0 / empty | loader | button | 0 |
| raw-button-returned | loader | 1 / 1 | loader | 0 / empty | loader | button | 0 |
| round-final | loader | 1 / 1 | loader | 0 / empty | loader | button | 0 |

Final independent `round-final` status after the focused button diagnosis and return to P1 (earlier `final` and `continuation-final` snapshots retain the previous partial-run checkpoints):

```text
H2_LOADER_STATUS board=jieli_ac791n_devkit target=wl82 chip=ac791n device_uid=d879349abc9f capabilities=0x00000005 command_availability=0x00081d1f active_role=loader active_version=bazel-native-artifacts active_checksum=28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f active_image_size=938857 running_partition=1 next_partition=1 boot_intent=loader stage_valid=0 stage_package_checksum=- stage_package_size=0 stage_image_checksum=- stage_image_size=0 stage_role=unknown stage_version=- stage_board=- stage_target=- partition_1_valid=1 partition_1_package_checksum=d3cfaf7229b7b0d230c281ca0d1a6add06d497a7b8740df01eac0192c92730c0 partition_1_package_size=928062 partition_1_image_checksum=28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f partition_1_image_size=938857 partition_1_role=loader partition_1_version=bazel-native-artifacts partition_1_board=jieli_ac791n_devkit partition_1_target=wl82 partition_2_valid=1 partition_2_package_checksum=b01937d9c3bcd6287b49220b0901fe6c5a48050e9295a52266c8818cfd3b07e2 partition_2_package_size=1278347 partition_2_image_checksum=eb5f587ff952469649d6c0e03915c104a2413e8e11f61f9a5ff60e9f0c57a0dd partition_2_image_size=1292785 partition_2_role=app partition_2_version=bazel-native-artifacts partition_2_board=jieli_ac791n_devkit partition_2_target=wl82 last_result=0 mfg_mode=1 mfg_steps=0000000000000000000000
```

## Limits and skipped work

Wi-Fi credential persistence was explicitly skipped because no bench AP credentials were supplied. PAL Wi-Fi case 27 is offline coverage only. No connected Wi-Fi/AP or HTTP cases were enabled in the lifecycle runner.

Both full BLE rounds and audio streaming smoke completed. Button installation and persistent confirmation state were verified, but READY/confirmation text capture failed twice and remains a failed acceptance item. The round does not establish long-duration BLE/RF stability, controlled physical button interaction, acoustic quality, controlled audio stop/restart or power-loss atomicity.

Resets were software resets through Loader lifecycle commands and the expected crash-before-confirm rollback; none was a physical power cut. There was no power cut, manual power cycling, USB DL, format operation, other serial port access or P1 modification outside the normal Loader self-update flow. No independent status timeout or 90-second unresponsive post-reset interval was observed. Early PAL capture loss was filled by the same run's ledger replay, but the initial boot transcript remains incomplete.

Raw commands, build logs, monitor logs, runner report, helper scripts and status captures remain outside the worktree under `/tmp/jieli-main-acceptance-2026-09-16`. No raw `.log`, `.status` or `.json` capture from this round was added to the worktree. All review-relevant identities, status checkpoints, counts, result codes and limits are retained inline here.

## Documentation validation

`bazel test --config=macos_arm64 //guides:guides_build_test //tools/bazel:jieli_evidence_test` passed both targets with exit 0 (guides test cached, evidence test freshly executed); the documentation build completed successfully. `git diff --check` passed. These documentation checks are separate from the hardware results above.
