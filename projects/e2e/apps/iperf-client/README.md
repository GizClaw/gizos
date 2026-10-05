# iperf Client

`app:iperf_client` runs the same TCP/UDP throughput matrix over explicit IPv4 and IPv6 addresses using Runtime and the existing portable `iperf_e2e` runner. The portable network entry uses public Wi-Fi/Net and verifies Runtime IP event/state, disconnect/reconnect and saved-credential preservation. Image confirmation and device lifecycle belong to the launcher. The DevKit target is `//projects/e2e/targets/h2loader_tar_zlib/iperf-client/devkit:package`; the BK7258 target is `//projects/e2e/targets/h2loader_tar_zlib/iperf-client/bk7258_v3_202405:package`.

The client temporarily associates with the AMOLED server AP `GizOS-iPerf` / `gizosiperf`, channel 6. It never calls `connect_and_save`, clears credentials or modifies the Loader partition. The App does not start its management BLE service and Wi-Fi power save is set to NONE. BK SDK still initializes its Bluetooth controller; this is not a controller-off bench. IPv4 comes from DHCP, IPv6 from the server's on-link ULA/SLAAC. After 15 seconds for DHCP, RA and DAD, the App identifies the available families and requires matching PAL and Runtime readiness. IPv6-only requires public Wi-Fi GOT_IP with IPv4 invalid and IPv6 ready, without entry-side SDK initialization. Reboot the App after changing the server mode so the station starts with a fresh netif.

Each enabled family runs three rounds, with ten five-second measurements per round:

- Unlimited TCP, 16-KiB blocks, client → AMOLED and AMOLED → client (`-R`).
- UDP at 5, 10, 20 and 40 Mbit/s, 1200-byte payloads, both directions.

IPv4-only and IPv6-only each produce 30 cases. Dual stack produces 60 cases, first IPv4 then IPv6; these are separate streams and their throughput is never added. The UDP payload avoids IP fragmentation with an ordinary 1500-byte MTU in either family. The ramp measures loss under increasing offered load; configured bitrate is not achieved throughput.

`H2_IPERF_E2E_CASE` reports receiver and sender bps, UDP highest received sequence (including gaps), receiver loss and jitter. UDP loss percentage is `lost / packets * 100`; the sequence denominator already includes missing datagrams. The target token `bk7258-m46-f6-r2` identifies board, server mode, wire family and round. Receive bps uses the receiver's own measured data interval; the `ms` field is client wall time including setup/results. `H2_IPERF_CLIENT_LINK` records actual local addresses, RSSI, channel, BSSID and radio policy. `H2_IPERF_CLIENT_MEMORY` records internal SRAM and PSRAM before/after each case. `H2_IPERF_CLIENT_COMPLETE` accounts for every case, including failures. PASS means a successful result exchange with received payload; there is no arbitrary throughput or loss pass threshold.

The BK launcher places benchmark buffers and its 32-KiB runner stack in the existing PSRAM provider so receive buffers do not exhaust internal Wi-Fi/control memory. It pauses and joins UART management while measuring, writes ledger records through a bounded UART PAL sink, drains/closes the sink and restarts management before image confirmation. Capture all ledger bytes: CLI protocol demultiplexing can consume binary-looking SDK output, so qualification can hand off from the actual BOOT monitor to a sole directed raw UART collector before setup begins. Keep that same boot's BOOT prefix and raw capture hashes; missing records invalidate the run.

## Build and verify

```sh
bazel test --config=macos_arm64 //projects/e2e/apps/iperf-client/app:client_test
bazel build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/iperf-client/devkit:package
bazel build --config=bk7258 //projects/e2e/targets/h2loader_tar_zlib/iperf-client/bk7258_v3_202405:package
```

The host test uses real loopback IPv4/IPv6 sockets, exercises 60 TCP/UDP exchanges including repeated dual-family rounds, validates non-fail-fast accounting on a refused endpoint and rejects inconsistent mode/family configuration before traffic. Hardware qualification additionally requires exact UID/port, source/package/image hashes, complete serial logs and final coredump/firmware state.

## Two-board performance qualification

On AMOLED select the desired mode and Start server, then boot the client. Automated qualification can build the same AMOLED target with `--define=H2_IPERF_SERVER_AUTOSTART_MODE=4`, `6` or `46`; the default is `0` (touch-controlled, stopped at boot). This option starts the existing controller after the real Display/Touch readiness callback, without injecting touch events. The UI continues to show the real mode and can stop the service.

For each server mode: record the installed server image, wait for LISTENING, freshly boot the same client image, capture all cases and COMPLETE, and compare the client-inferred mode with the installed server mode. Compare the median of the three receive-throughput measurements for each protocol/direction/offered-rate tuple; retain UDP loss and jitter for each round. Record failures separately and do not relabel failed runs as performance results. Preserve original Loader, Stage, saved STA credentials and coredump state; restore the operator's images after testing. Radio measurements depend on board position, channel contention and RSSI, so these are measurements of the actual bench rather than guaranteed device specifications.

This App and the AMOLED touch server measure TCP and UDP. `libs/iperf` also implements SCTP-over-UDP, but neither of these images wires a SCTP provider. They speak iperf3, not the iperf2 protocol used by the stock ESP-IDF example.

## Public IP readiness acceptance

The launcher never creates link-local/SLAAC through the SDK. `H2_IPERF_CLIENT_LINK` also reports PAL station state, IPv4 validity, IPv6 readiness and matching Runtime IP readiness. `H2_IPERF_CLIENT_COMPLETE` requires public readiness, a real disconnect that clears Wi-Fi/Runtime/Netif IP state, a second public association with the same mode and an unchanged canonical saved-credential signature. No password or signature bytes are printed. These lifecycle checks are separate from optical/finger touchscreen acceptance and from Internet/default-router qualification. The network setup uses a 20-second association call followed by at most 45 seconds for address/event matching; the first 15 seconds allow both families to settle before classifying the server mode.
