# GizClaw ping/speed smoke app

This shared Smoke App validates the firmware GizClaw nanopb RPC adapter against the upstream GizClaw e2e Docker service. Its Runtime contract is target-independent; the current AMOLED H2Loader launcher owns Wi-Fi, image lifecycle, and board acceptance.

Runtime inputs:

```text
GIZCLAW_SERVER_ENDPOINT=<host-lan-ip>:19820
GIZCLAW_CLIENT_PRIVATE_KEY=<generated e2e identity private key>
GIZCLAW_CONNECT_TIMEOUT_MS=45000
```

The endpoint must be reachable from the amoled board Wi-Fi network. Do not use `127.0.0.1` for board acceptance.

The board sequence is:

```text
saved Wi-Fi -> GET /server-info -> connect -> poll -> outbound ping -> outbound speedtest -> poll -> close
```

`SKIP gizclaw-ping-speed reason=server_info_unavailable` is only emitted before the board RPC case starts, when required inputs, Wi-Fi, or `/server-info` are unavailable. After board `/server-info` succeeds, connect, protobuf RPC, stream length, and close errors are reported as `FAIL`.
