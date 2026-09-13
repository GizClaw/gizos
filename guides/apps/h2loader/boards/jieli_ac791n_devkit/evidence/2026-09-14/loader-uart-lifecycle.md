# 2026-09-14 — current-source Loader self-update and UART lifecycle

Source: `2a814d324455a9f2995c3b4348e82bb300bb77b7`, clean worktree before
building. Device UID `d879349abc9f`; exclusively UART
`/dev/cu.usbserial-20131240`, 460800 baud. The port was re-listed and `lsof`
found no stale reader. No BLE, USB recovery, manual reset, or other device was
used. P1 was written only by the normal Loader self-update flow.

## Root cause and current-source verification

The previous [failed run](../2026-09-13/uart-lifecycle-stat-failure.md) reported:

```text
H2_JIELI_IMAGE_READ_SD_ERROR step=stat partition=2 offset=0 bytes=65536 read=0 rc=-8
```

That old label also covered a successful stat falsely reporting a directory.
The documented SDK inspection established that `fdir_exist` tests whether
`fopen` succeeds, not the FAT directory bit. Commit `56cff72e` replaced that
classification with `fget_attr` / `F_ATTR_DIR`. Current source additionally
contains the single-encoding SDK `fopen` and single-level mkdir changes from
`0b982f45` / `2a814d32`. No new production fix was needed in this run.
This is a combined current-source hardware verification, not an isolated A/B
experiment attributing every outcome to one of those commits.

The distinct-image update now passes the formerly failing source read after
candidate reboot and confirmation, copies the complete image, verifies P1,
and converges. Ordered selected lines are retained in
[the trace](./loader-self-update-trace.log); full local capture is
`tmp/jieli/2026-09-14-loader/install.log`.

```text
H2_JIELI_BOOT_INFO result=-1 base=0x0 bytes=0 version=0 logical=2
H2_JIELI_LOADER_TRIAL confirmed=1 publish_gate=before-copy-p1
H2_JIELI_LOADER_HEADER published=1 confirmed=1
H2_JIELI_STARTUP_EVENT event=2 code=0
H2_JIELI_IMAGE_READ_SHADOW partition=2 offset=0 bytes=65536
H2_JIELI_IMAGE_READ_SHADOW partition=2 offset=917504 bytes=12413
H2_JIELI_UPDATE_WRITE_DONE expected=929917 native=929917 result=0
H2_JIELI_IMAGE_READ_SHADOW partition=1 offset=0 bytes=65536
H2_JIELI_BOOT_INFO result=0 base=0x4020 bytes=929853 version=2 logical=1
H2_JIELI_STARTUP_EVENT event=4 code=0
```

The initial old-P1 startup also logged `H2_JIELI_TRIAL_ROLLBACK
app_bootable=0 action=command-mode` for the retained unconfirmed PAL App.
It precedes update event 1; it is not a failed trial of the new Loader.
No `H2_JIELI_IMAGE_READ_SD_ERROR` appeared during this update. The final
monitor was intentionally interrupted (exit 130) after event 4, then an
independent status succeeded.

## Package identities and before/after status

[All three package/image SHA-256s and sizes](./loader-artifacts.json).
Loader package (918365 bytes):
`9b84d5fdf10b69c56bace2f3bbb4b9b4594526d0f62789f1d1fd5e1bfd0612ca`.
Loader image (929917 bytes):
`42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd`.

[Initial status](./loader-initial.status) confirmed old P1 image
`7130cfe2386c86a7dcf82ccd15854f14b64fb525be14a984cfdaa5dd19d64dfa`;
P2 and Stage held PAL App package
`f2d528a110827e8161154d4c7df925103f1b6a76931454b3a5afc4aae631997d`,
image `fec8c47945b29ac9f294d177d507d19705195c34ba8f76c63a9d60d5ff95cfb7`.
Both partitions were valid; P1 running/next, last_result=0.

After send, [immediately-before-install status](./loader-before-install.status)
recorded unchanged P1/P2, Stage equal to the new Loader package/image above,
and last_result=0. Send acknowledged all 918365 bytes and the exact package
SHA. [After-install status](./loader-after-install.status) independently
confirmed both partitions valid and carrying the new Loader package/image,
P1 running/next, Stage empty, boot_intent=auto, last_result=0.

## Commands and host/native validation

Native build succeeded in OrbStack in 86.932 seconds, 26 actions:

```sh
orb -m embed-zig-noble-amd64 bash -lc 'cd /Users/idy/GizClaw/gizos/.claude/worktrees/qrcode-library-selection-fab623 && source /Users/idy/h2vivi/firmwares-devenv/export.sh && unset IDF_PATH H2LOADER_IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_PATH && bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64- //projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package //projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit:package //projects/example/targets/h2loader_tar_zlib/crash-before-confirm/jieli_ac791n_devkit:package'
```

Packages were copied from the VM's `bazel-amd64-bin` to the shared worktree's
`tmp/jieli/2026-09-14-loader/`. CLI and runner were built with
`bazel build --config=macos_arm64` and their documented cc_binary targets.
Four host tests freshly passed (no cached test results, 3.866 seconds overall):

```sh
bazel test --config=macos_arm64 --nocache_test_results \
  //tools/bazel:jieli_sd_directory_test \
  //projects/e2e/apps/pal/app:pal_e2e_test \
  //projects/e2e/apps/pal/app:pref_e2e_test \
  //projects/h2loader/apps/e2e-runner:h2loader_e2e_runner_test
```

[Host test output](./loader-host-tests-fresh.log), with trailing whitespace
trimmed in the committed copy. No behavior or test was
changed; existing attribute fault-injection and directory regression coverage
was rerun. An earlier invocation returned a cached directory-test PASS.

For each standalone UART command, the executable was
`bazel-bin/projects/h2loader/targets/cc_binary/cli/h2loader` with
`--no-ble --port /dev/cu.usbserial-20131240 --baud 460800`:
`status`, `send --file tmp/jieli/2026-09-14-loader/jieli_ac791n_devkit-loader-wl82.update.tar.zlib`,
`status`, `reboot upgrade --monitor`, then independent `status` after stopping
the monitor. An initial send invocation omitted required `--file` and exited
2 before transfer; it was corrected, not counted as a hardware run.

Full UART suite command, after installing the current-source Loader:

```sh
bazel-bin/projects/h2loader/targets/cc_binary/e2e-runner/e2e-runner \
  --uart /dev/cu.usbserial-20131240 --baud 460800 \
  --expected-board jieli_ac791n_devkit --expected-target wl82 \
  --app-firmware tmp/jieli/2026-09-14-loader/jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib \
  --loader-firmware tmp/jieli/2026-09-14-loader/jieli_ac791n_devkit-loader-wl82.update.tar.zlib \
  --crash-firmware tmp/jieli/2026-09-14-loader/jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib \
  --monitor-ms 3000 --report tmp/jieli/2026-09-14-loader/uart-lifecycle.json \
  > tmp/jieli/2026-09-14-loader/uart-lifecycle.log 2>&1
```

## Full UART result and final state

**25/25 PASS**, rc=0, 560.440 seconds, one run, no runner/test changes.
[Raw JSON report](./loader-uart-lifecycle.json) and
[selected ordered trace](./loader-uart-lifecycle-trace.log).
Full unfiltered local log remains `tmp/jieli/2026-09-14-loader/uart-lifecycle.log`;
selected traces omit routine traffic and retain lifecycle/error markers.

The status read at the end of `reboot-loader-preserves-stage`, immediately
before the `install-loader` case starts staging, records P1 package/image
as the new Loader, P2 package
`354ad51fe0520caf0dd2508a1cdfe137f18191f3ddee6db6f5b6879a4ab1c2d9`
and image `e42c0e08bd8fc00362ea9d75d0656010b29cbc92d3604c68e2b5e2e3b5941aef`,
Stage empty, last_result=0. This UART snapshot is in the preceding case's
JSON status; there was no second reader during the runner. The runner then
staged and installed the Loader package. Its final status records identical
valid P1/P2 Loader metadata, Stage empty, last_result=0.

This suite's Loader case is a same-image install (current Loader already in
P1), and logs events 1 and 4. It is not a second distinct-image trial/copy
proof. The standalone update above provides that proof, including the
new provider reading the shadow written by the old Loader. The full suite
additionally exercises staging/image writes with the current provider.

| Case | Result | Elapsed ms |
| --- | --- | ---: |
| help | PASS | 464 |
| status | PASS | 269 |
| stats | PASS | 533 |
| legacy-commands-absent | PASS | 466 |
| send | PASS | 29941 |
| stage-abort-after-send | PASS | 582 |
| monitor | PASS | 3268 |
| reboot-loader-monitor | PASS | 128914 |
| reboot-upgrade-monitor | PASS | 55864 |
| reboot-app-monitor | PASS | 5252 |
| app-help | PASS | 463 |
| app-status | PASS | 277 |
| app-stats | PASS | 523 |
| app-memory | PASS | 477 |
| app-legacy-commands-absent | PASS | 470 |
| app-send | PASS | 31072 |
| app-stage-abort-after-send | PASS | 599 |
| reboot-app-preserves-stage | PASS | 34575 |
| reboot-loader-preserves-stage | PASS | 31658 |
| install-loader | PASS | 52131 |
| install-crash-app | PASS | 178807 |
| coredump-status | PASS | 499 |
| coredump-dump | PASS | 700 |
| coredump-erase | PASS | 2028 |
| coredump-status-after-erase | PASS | 588 |

`reboot-loader-monitor` took 128.914 seconds and `install-crash-app` took
178.807 seconds. Both encountered long reconnect waits while the board kept
emitting READY; the existing runner eventually reconnected without manual
reset or altered timeouts. The reconnect latency remains unexplained. Raw
logs also contain mixed READY/binary fragments and host
`darwin_netif default-route-reconcile-failed result=-6` warnings; no claim is
made that these are fixed or responsible for the waits. A passing suite
must not hide these observations.

Crash App emitted `H2_CRASH_BEFORE_CONFIRM_READY action=crash`, then P1
reported `H2_JIELI_TRIAL_ROLLBACK app_bootable=0 action=command-mode`.
The runner verified 2096 coredump bytes, exported them, erased the dump and
confirmed the blank state.

[Independent final UART status](./loader-final.status) confirms UID
`d879349abc9f`, P1 valid/running/next with new Loader image
`42c39b7aae00917e44e9807503fe57fdb4ccad25b58793ab12087a2da03d22cd`,
boot_intent=auto, last_result=0. P2 and Stage retain crash App package
`a6fb82b90e15d34c1889b7c8802c7f08957081f1efc71f7f29bdb10c5d83483b`,
image `a75bdeca184ecc78ebdf5126dfc75df3170976dbe4a4fcfddfa7da363c3ac93e`.
P2 metadata is valid but the unconfirmed crashed App is rejected for boot;
this is the expected rollback fixture, not a healthy runnable App. UART was
released (`lsof` found no holder). No post-suite cleanup/install was performed.

No new BLE, actual power-cycle, interruption-during-write, or broad PAL
acceptance is claimed. This closes the current-source normal self-update
and full UART lifecycle retest, while retaining the reconnect-latency caveat.
