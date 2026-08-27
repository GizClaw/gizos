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

## Dynamic command availability

The status objects returned by `status(port)`, `install(port, blob)`, and `stage(port, blob)` include `commandAvailability` when the connected firmware reports dynamic lifecycle gates. The unsigned 32-bit mask currently defines:

- `1 << 0`: reboot from H2Loader to App is available.
- `1 << 1`: reboot from H2Loader to H2Loader is available.

A present value of `0` is authoritative: neither command is currently available. Unknown bits are preserved for forward compatibility but do not authorize either known command. Static `capabilities` still indicates whether a command exists; `commandAvailability` is the connected-state gate.

Firmware predating this contract omits the property. Consumers should use property presence or `Number.isInteger(status.commandAvailability)` and apply their legacy fallback only when it is absent. The SDK exposes this status snapshot for UI decisions; Host Core and the device still enforce availability again when a command executes.
