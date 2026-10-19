# Button confirmation console flush — 2026-09-17

## Outcome and source

The device-side fix for the lost button confirmation console passed hardware acceptance: a monitored relaunch now captures `H2_JIELI_BUTTON_SMOKE_READY buttons=8 display=480x320 result=0`, `JIELI_APP_CONFIRM result=OK code=0 target=0 transport=0` and `JIELI_TRIAL_TIMER state=deleted id=11`, and independent status shows the button App confirmed in Partition 2 with an empty Stage. The change adds a session-admission gate to the shared App transport so the App answers no `SESSION_OPEN` until its confirmation console has been produced on the raw pre-session path; the host then captures those lines exactly as a raw `cat` does. This round was run on UID `d879349abc9f` over `/dev/cu.usbserial-20131240` at 460800 with the fixed button package built for `jieli_ac791n_devkit`/`wl82`; the tested App image checksum is `6d6975091d9dceff5c1a7da80c69e90bc01dfefd1466fd60525bcf99e2a743a8` and the unchanged P1 Loader is `28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f`.

## Ordered results

The fixed button package was installed once into Partition 2 through the UART Loader and relaunched under the ordinary `reboot app --monitor` with an unmodified CLI, and the monitor displayed the three required lines in order after `input-start result=0`; independent status then reported `running_partition=2`, `stage_valid=0` and `last_result=0` for the button App image, after which an explicit `reboot loader` returned to `running_partition=1`.

A second relaunch repeated `reboot app --monitor` while a byte-level tee recorded every inbound byte at the POSIX serial read before any parsing, and that raw capture contains `H2_JIELI_BUTTON_SMOKE_READY`, `JIELI_APP_CONFIRM result=OK` and `JIELI_TRIAL_TIMER state=deleted id=11`, with the first `H2IKCP` session-acknowledgement frame appearing on the wire only after the trial-timer line, proving the confirmation reaches the host raw before the reliable session opens.

The unmodified PAL package was installed once and relaunched untraced, and its first capture pass contained all ten distinct case lines including `H2_PAL_E2E suite=1 case=1 result=0` and `H2_PAL_E2E suite=1 case=2 result=0` with aggregate `H2_PAL_E2E result=0 passed=10 failed=0`; the twelve-byte garble that replaced cases 1 and 2 in the 2026-09-16 round did not recur, so no PAL loss remains open from this defect.

One earlier relaunch attempt reset to the Loader shortly after `input-start result=0` and before any confirmation was produced, so it lost no confirmation text; that reset is an independent App-startup flake unrelated to the console path, and an immediate repeat relaunch captured every required line, so it is recorded here rather than attributed to the flush change.

The board was returned to the Partition 1 Loader and the final independent status reported `running_partition=1`, `next_partition=1`, `stage_valid=0`, `last_result=0` and `partition_1_image_checksum=28123452b282b325471e8f64fa5d462d314c63c934bf6fea66e6e3936bd3258f`.

## Boundaries

This round establishes only the confirmation-console capture, the raw wire presence and PAL first-pass completeness on this one board; it does not cover power-loss atomicity, long-duration BLE or RF stability, physical button interaction, or any host monitor change, and all resets were software resets through Loader lifecycle commands with no USB download, format or manual power cycling.
