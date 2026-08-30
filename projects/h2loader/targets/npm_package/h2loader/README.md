# @gizclaw/h2loader

Browser SDK for inspecting H2Loader packages and operating devices through Web Serial.

## Install

Configure the GizClaw GitHub Packages registry and install the package:

```ini
@gizclaw:registry=https://npm.pkg.github.com
```

```sh
npm install @gizclaw/h2loader
```

## Use

```js
import { createH2Loader } from "@gizclaw/h2loader";

const loader = await createH2Loader();
try {
  const ports = await loader.getAuthorizedPorts();
  for (const port of ports) {
    console.log(port.id, port.usbVendorId, port.usbProductId);
  }
} finally {
  await loader.close();
}
```

The SDK requires a secure browser context and a browser with Web Serial support. npm is the distribution format for browser applications; this package does not provide a Node.js serial-port runtime.

Each returned port exposes its canonical Web PAL `id`, such as `web-serial-1`.
Use this ID for row identity, selection, and operation routing during the current
SDK client lifetime. USB VID/PID are display metadata and must not be used to
identify a device because multiple authorized devices can share them.

## Status protocol

`status(port)` and `stage(port, blob)` return the exact connected status
contract. The breaking `0.2.0` lifecycle API is `stage`, `stageUrl`,
`abortStage`, `rebootApp`, `rebootLoader`, and `rebootUpgrade`; the former
`install`, `rollback`, and `restart` entry points are not part of this version.
`capabilities` is stable hardware presence only:
UART bit 0, Wi-Fi bit 1, and BLE bit 2. `commandAvailability` is the required
20-bit device-owned command mask; `0` authoritatively means that no command is
currently available. The SDK exports `H2LoaderCapabilities`,
`H2LoaderCommands`, and `commandAvailable()` so consumers do not duplicate bit
assignments. Host Core and firmware check the same command bit again at the
execution boundary.

Lifecycle state is returned directly as `activeRole`, `activeVersion`,
`activeChecksum`, `activeImageSize`, `runningPartition`, `nextPartition`,
`bootIntent`, `stage`, `partition1`, `partition2`, and `lastResult`. Metadata
contains the package and image identity plus `valid`. Removed install-state,
loader-upgrade, manual-hold, and app-confirmed fields are not reconstructed by
the SDK. Unsigned 64-bit sizes are decimal strings so JavaScript does not round
them.

This is a lockstep protocol version: there is no legacy `H2_APP_STATUS`,
missing-field fallback, capability-to-command inference, or role reconstruction.
