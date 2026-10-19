# Mosaico full E2E rerun — 2026-10-19

Status: **rerun completed with open failures; both boards restored**. Main baseline `d3ff30bc4a3c9660af62ea04cf15a35c0c70a502`, branch source head `12189174a635fc804c7c290981be80a3cf61957a`. Each rebuilt package owns its source-input/diff/configuration manifest and package/native-image hashes; historical pre-rebase passes do not qualify these images. IDF is pinned to `ddb3852ab5c7f9e6dbc268203ef9d612de391bf8` (6.2).

The operator permits board adaptations and tests. Shared PAL/Runtime failures must be reported without changing those implementations. GizClaw business E2E remains excluded. Mosaico UID is `1c2904d0a629`; the explicitly authorized AMOLED peer UID is `94a990281a12`.

## Layer review

The existing branch is broader than board-only code: 373 changed paths include public Camera/IMU contracts, H2Peer/H2SCTP/iperf changes, ESP native providers, the RV32 libco adapter/backend and shared build contracts. These remain GizOS repository integrations; no new product/App architecture layer was added. Runtime production source is unchanged against this baseline; its branch delta is tests. This review identifies ownership boundaries, not an assertion that every historical added line received a new semantic review.

This rerun adds only three board-specific E2E compositions (`pal-ipv6`, `iperf-client`, `lua-runtime`), a board-owned iperf configuration and related documentation. Launchers own startup tasks and Runtime lifecycle and call the existing portable Apps. Their actual test results are listed below.

One concrete lower-layer modification was found: board CMake applied patches to a copied SDK DVP driver and the managed SC101IOT sensor. Both patch applications are removed. The R129 native diagnostic builds with the pinned unmodified drivers; old patched-camera receipts cannot qualify it. Fresh unpatched automatic capture passed 17/17 cases and 40 frames. Manual preview subsequently timed out once before a successful 77-frame repeat; this intermittent failure remains open. The external SDK checkout is clean.

## Package inventory

All 25 managed-package directories were enumerated. Four are role/platform-specific: `iperf-server/amoled` is used as the authorized peer; `coredump` and `pal-ble-smoke` have only Jieli launchers; `pal-storage-backup` is a BK FlashDB backup utility whose README explicitly excludes E2E qualification. These are not reported as Mosaico package passes. Mosaico crash/recovery and BLE coverage are tracked in the separate Loader and Lua Link rows.

## Current matrix

| Suite | Current status | Required scope |
| --- | --- | --- |
| `atomic` | PASS | 56 × 2 |
| `gizclaw-e2e` | EXCLUDED_BY_USER | Excluded by operator |
| `iperf` | PASS | 14 × 2, 28 AMOLED TCP/UDP/SCTP witnesses |
| `libco-smoke` | PASS | 6 phases, 10,000 switches × 10 boots |
| `lua-link` | WORKLOAD_PASS_WITH_ROM_PEER_RECOVERY | five bilateral rounds plus actual USB loss and both Loader returns |
| `pal-audio` | PASS | 24 × 2; 30-second soak per boot |
| `pal-audio-decoder` | PASS | 29 × 2 |
| `pal-core` | PASS | 41 × 2 boots |
| `pal-crypto` | PASS | 22 × 2 |
| `pal-display` | PASS | 24 × 2 boots |
| `pal-http` | PASS | 45 × 2, actual HTTP/TLS witnesses |
| `pal-json` | PASS | 15 × 2 |
| `pal-mqtt` | PASS | 36 × 2, nonce-bound MQTT witnesses |
| `pal-net-tls` | PASS | 37 mandatory × 2; optional multicast unsupported |
| `pal-storage` | PASS | 36 across phases 1/2/3/4/4 |
| `pal-webrtc` | PASS | 43 × 2, Pion peer |
| `pal-wifi` | PASS | 38 cases plus restart persistence; two full 600-second peer windows after seed |
| `webrtc-performance` | FAIL_RECORDED | 2 default-off original smoke runs; 200 ms P99, missing ≤5, fairness ≥800‰ |
| `pal-ipv6` | FAIL_RECORDED | 56 including 43 WebRTC; separate H2Peer-to-H2Peer bench |
| `iperf-client` | WORKLOAD_PASS_WITH_PEER_RECOVERY | IPv4 30 + IPv6 30 + dual-stack 60; three rounds each |
| `lua-runtime` | PASS | 9 × 2, existing multi-worker registry |
| `loader-full-recovery` | PASS | 51 unchanged cases; actual URL download and induced coredump evidence |
| `mosaico-board` | FAIL_RECORDED | unpatched camera; actual RGBW/touch/preview/tones and left insertion |
| `aec-calibration` | UNSUPPORTED_CAPABILITY | ES8311 observer unsupported; no acoustic AEC PASS |

Evidence is kept outside Git in `e2e-evidence/20261019/full-rerun-r127`: `CURRENT_RESUME.json`, individual qualification receipts, immutable packages/source manifests, raw device logs and actual peer ledgers. A build alone is never a hardware pass. The Net/TLS core receipt does not qualify full-Net multicast; AEC remains an unsupported capability. IPv6 on-link AP/SLAAC evidence does not prove IPv6-only WAN/default-route behavior.

## Outstanding results

WebRTC R130 passed boot 1 but failed boot 2 at loaded-audio P99 235.584 ms (limit 200 ms); boot 1 was 99.919 ms. Fairness was 1014/975 per mille, with 0/2 missing loaded frames. Both full peer windows had low compilation load (maximum 0.6/0.4% summed build CPU). This reproduces the open performance gate; root cause is not yet isolated. No shared PAL/Runtime implementation was modified. Raw metrics, peer witnesses and independent checks are in `rtc130-original-smoke-verification.json`.

IPv6 R129 reached 53/56 with clean resource counters. Failed cases: `tls-insecure-test-only` (timeout -6), `ipv6-socket-isolation` (-15), and `webrtc-ipv6-matrix` (-7). The embedded WebRTC failure is `channel-close-event` (20-second timeout), which blocks six following cases. Actual peer service and nonce-bound logs are retained. This is not qualified, and no shared implementation fix was made. An independent fresh repeat completed 54/56: socket isolation and the same channel-close timeout reproduced, while TLS_INSECURE passed. All retained resource counters and cleanup_error remained zero; both devices returned to Loader with empty Stage. The repeat is diagnostic evidence, not a complete two-boot qualification.

The AMOLED mode-6 peer update acknowledged the full package but failed to reconnect to its public command transport. Exclusive queries and USB software reset could not recover it; the operator physically reconnected USB. R30 then booted, confirmed and started its IPv6 service, with blank crash storage. Recovered workload evidence is separate; the install/reconnect issue remains unresolved and is not classified as a Mosaico throughput failure.

The `iperf-client` workload is complete: IPv4 30/30, IPv6 30/30 after the physical peer recovery, and dual-stack 60/60, each matched to actual peer records with public network lifecycle/settings checks. `iperf-client-functional-summary.json` binds all 120 cases; unattended peer installation remains unqualified because of the retained mode-6 recovery issue. UDP offered rates and actual received rates/loss remain distinct in the raw case records.

Lua Link completed five bilateral rounds and actual lost:-10/hold-ok handling; Mosaico returned to Loader. AMOLED public return timed out (-6); raw USB showed ROM download mode. Its MAC was verified, but ROM run did not finish and an explicit USB reset still did not restore public command response. ROM registers subsequently confirmed GPIO0 strap low with FORCE_DOWNLOAD=0. An esptool watchdog reset booted the existing App without Flash writes; both public Loader returns, unchanged P1 and blank crash storage then passed. The source of the strap condition is unresolved. This remains WORKLOAD_PASS_WITH_ROM_PEER_RECOVERY, not an unattended full Lua Link PASS.

## Unpatched board diagnostic

The R129 diagnostic passed Core 8/8, Audio 24/24 with a 30-second soak, and Camera 17/17 with 40 frames over two start/stop cycles. The operator confirmed RGBW, touch and three audible tones. The first manual preview returned a frame acquisition timeout (-6) with zero frames; a short restart captured 7 frames in 1.312 seconds. A subsequent requested sustained preview captured 77 frames over 12.137 seconds and cleaned up successfully, with normal continuous video confirmed by the operator. The later pass does not erase the initial intermittent failure. Its root cause is unresolved; it is not yet established as a shared PAL/Runtime bug. Raw evidence and separate attempt records are bound by `board129-manual-qualification.json`. The fresh empty-slot boot passed Core 8/8 and Audio 24/24 with a 30-second soak and left ABSENT. Powered insertion was recognized and the camera descriptor identified, but capture timed out after 9 first-cycle frames; the restarted second cycle completed 20 frames. The total was 29/40, with 16 passing checks and two failed checks (acquire and frame count). `board129-left-insertion.json` preserves this second acquisition failure. No SDK, BSP, sensor or shared PAL/Runtime implementation fix was applied.

## Test infrastructure observations

- R28 AMOLED iperf was accidentally built with its default client role. It refused confirmation, and the incomplete managed-install result is retained. After returning the original Loader and aborting only that owned Stage, R32 with explicit server role was installed and verified on the real peer. This is a test configuration error, not a shared PAL/Runtime product failure.
- The initial libco observer expected `fifo`; the actual source phase is `fifo-yield`. Its failed observation is retained separately; only the corrected complete ten-boot receipt can close this gate.
- The first R130 preflight used loopback although Pion binds the LAN address. That error occurred before DUT installation and is archived separately; the corrected real two-boot performance result is reported above.
- Initial Wi-Fi fixture activation used the normal CLI upgrade verifier, which requires cleared Stage before the fixture completes its deliberate 600-second trial. That host orchestration error occurred before DUT qualification. The retry retains accepted-start evidence and separately verifies the complete window, unchanged settings and eventual confirmation; no firmware gate is relaxed.
- Wi-Fi builds whose versions lack the existing `pal-wifi-` persistence prefix are build-only. R131/R132 use the required protocol before any new Wi-Fi hardware qualification. No persistence or confirmation gate is relaxed.

## CI follow-up

At checkpoint `407ca43d`, platform builds including ESP32-S31 passed. Linux and macOS build/test jobs stopped at a VitePress dead link in the Mosaico guide; the link now points to the immutable repository record and local `//guides:build` passes. The coverage job ran 942 tests: 940 passed, while `//libs/iperf:loopback_test` and the GizClaw browser test failed. A fresh uncached macOS iperf loopback repeat passes (21.6 seconds); this does not close the Linux coverage failure. GizClaw remains outside this hardware acceptance scope. No shared implementation change was made for these CI failures.

## Original firmware restoration

A fresh double read of Mosaico’s complete 16 MiB Flash matches byte for byte, SHA-256 `ff0af503fff49d23ce6c9fd016eb7e26975ba4ca8aaf3dd6dab2bf1c1bfed170`. Testing temporarily replaced both P1 and P2 through the documented Loader relay and then used the standalone board diagnostic. The original complete 16 MiB Flash has now been restored, with an independent byte-for-byte full readback matching that backup. Public original P1/P2 roles, versions, sizes and checksums match; the device is in the original Loader with empty Stage, last_result=0 and blank crash storage. `full-regression-original-layout-restored.json` binds both full readback and public validation.

The AMOLED original App has been restored. Public original P1/P2 roles, versions, sizes and checksums match; Stage is empty and crash storage is blank (`amoled-original-app-restored.json`). This is not a whole-Flash restoration claim for AMOLED.

See the [baseline assessment](esp_mosaico-baseline-20261019.md) and [historical qualification record](esp_mosaico-qualification.md) for earlier artifact-bound results.
