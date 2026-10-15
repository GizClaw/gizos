# iperf-server

Touch-operated network throughput bench under `projects/e2e`. The portable App owns the LVGL screen, asynchronous service controller, family-specific PAL iperf3 servers, cancellation and measurement snapshots. The AMOLED launcher owns Runtime assembly and temporary SoftAP IPv4 DHCP / IPv6 SLAAC configuration. It does not alter saved STA credentials.

Select **IPv4**, **IPv6** or **Dual stack**, then tap **Start server**. **Stop server** cancels accepted clients and measurements, joins the server tasks, and stops the AP. Change modes while stopped. Boot defaults to dual stack, stopped; UI readiness confirms the H2Loader App only after Display and Touch open and the first rendered frame completes.

If a task join or AP stop fails, the controller retains `STOPPING` and the error with its resources still owned. The screen shows **Stop failed** and enables **Retry stop**; Start and mode changes stay locked until cleanup succeeds. Each Stop request makes one cleanup attempt and waits for an explicit retry after failure.

The controller owns cleanup from the start of every network-start attempt. The AMOLED adapter attempts partial-start cleanup once, returns any cleanup error, and retains its AP state for Stop/retry. The stop hook is a successful no-op when no network is owned; a failed start never spins until cleanup succeeds.

Shutdown makes one manager cleanup attempt and exits that task. `destroy()` joins it and retries cleanup once on the caller; an error retains the App and any unjoined task/server/AP resources for a later destroy retry. Runtime and callbacks remain borrowed until successful destruction. Task joins and network callbacks remain blocking according to their own contracts. A fatal manager mutex error closes request handling and leaves retained cleanup to destruction.

| Setting | Value |
| --- | --- |
| SSID | `GizOS-iPerf` |
| Password | `gizosiperf` |
| Security / channel | WPA2 / 6 |
| IPv4 | `192.168.4.1`, DHCP enabled in IPv4 and dual modes |
| IPv6 | `fd53:697a:6f73:626::1/64`, on-link SLAAC in IPv6 and dual modes |
| iperf3 port | `5201`, TCP control, TCP or UDP data |

IPv6 mode stops DHCPv4 and removes the AP IPv4 address. Router advertisements carry a zero router lifetime; this is a local test LAN with no Internet gateway or DNS service. Stopping/removing the RA timer and PCB happens on the TCPIP task before the Wi-Fi provider destroys the AP interface. Every restart creates a fresh AP netif. Dual stack uses two V6ONLY/family-specific listeners, one test at a time per family. TCP/UDP support forward and reverse (`-R`) tests. Parallel streams, bidirectional tests, authentication and SCTP are not provided by this App.

The UI reports connected stations and cumulative local RX/TX throughput for each family. A successful test also retains the local and remote result, byte/packet totals, UDP loss and jitter in the controller snapshot; failures remain explicit result codes. Data blocks are limited to 128 KiB and received JSON to 4 KiB; UDP receive buffers reserve 64 KiB. Transient Touch IO/timeouts retry for up to eight consecutive reads and cancel the pending gesture without creating a click; persistent failure unwinds the UI. The image uses serial-only H2Loader recovery, keeping BLE controller SRAM available to Wi-Fi/LCD DMA. The launcher initializes Wi-Fi before Display and caps the LCD DMA chunk at eight rows (5888 bytes). Buffers use the launcher's PSRAM allocator; the 368 × 448 RGB565 UI frame uses 322 KiB and full-frame rendering. Network and protocol tasks never call LVGL. The stop callback slices blocking I/O to 100 ms so idle control connections and stalled data handshakes do not hold the screen's stop action until their full deadlines.

## Build and install

```sh
bazel build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/iperf-server/amoled:package
```

Use the existing H2Loader transport: inspect the directed AMOLED port/UID and preserve firmware, P1, Stage and coredump evidence; send the package, reboot upgrade, then verify the exact running image and `H2_IPERF_SERVER_UI_READY`. The default UI waits for touch input; flashing alone does not qualify throughput or physical touch operation.

For automated two-board qualification, `--define=H2_IPERF_SERVER_AUTOSTART_MODE=4`, `6` or `46` starts the same controller after Display/Touch readiness. Default `0` retains stopped-at-boot operation. This does not inject touch events or qualify finger interaction. Pair it with `//projects/e2e/targets/h2loader_tar_zlib/iperf-client/devkit:package` and reboot the client after each server mode change; see the [client App](../iperf-client/README.md).

## Client commands

Join `GizOS-iPerf` first. Clients use the iperf3 protocol (ESP-IDF's stock iperf2 example is a different protocol).

```sh
iperf3 -4 -c 192.168.4.1 -t 10
iperf3 -4 -c 192.168.4.1 -t 10 -R
iperf3 -6 -c fd53:697a:6f73:626::1 -t 10
iperf3 -6 -c fd53:697a:6f73:626::1 -u -b 10M -l 1200 -t 10
iperf3 -6 -c fd53:697a:6f73:626::1 -u -b 10M -l 1200 -t 10 -R
```

A GizOS MCU can use `h2_iperf_client_run()` against either displayed address. Use `-l` up to 128 KiB; the official TCP default of 128 KiB fits. Wireless throughput depends on the board/radio and environment, so no fixed speed threshold is claimed.

## Validation

```sh
bazel test --config=macos_arm64 //projects/e2e/apps/iperf-server/app:controller_test //projects/e2e/apps/iperf-server/app:ui_test
bazel test --config=macos_arm64 //libs/iperf:all
```

The controller test uses real PAL sockets: 16 TCP/UDP forward/reverse scenarios across single-family and dual modes, eight official iperf3 client interop scenarios, six control/data-handshake/result-exchange cancellation scenarios, four active-test cancellations, oversized block/JSON rejection and recovery, failed startup recovery, queued-start cancellation and repeated teardown/rebind. Five persistent cleanup failure scenarios cover AP stop after normal stop/partial startup, retained worker join, manager mutex failure and a network-start error with active network ownership; destroy returns an error without freeing borrowed/live state and succeeds on retry after the fault is cleared. The UI test sends ten Touch PAL down/up pairs through the production LVGL input callback to select/start/stop all three modes and retry a failed stop, and verifies Display/Touch lifecycle; it exports production RGB565 rendering as PPM for inspection. These host tests do not replace an AMOLED optical/touch check or an external client's over-the-air throughput run.

The AMOLED SDK defaults are board-owned. Its canonical board file supplies IPv6/SLAAC policy; `boards/amoled/esp32s3/layouts/h2loader/sdkconfig.iperf-server.defaults` supplies the existing INFO diagnostics and 12-socket/6-active-TCP/4-listener budget. The firmware target declares this variant and the common Loader defaults as project support files.
