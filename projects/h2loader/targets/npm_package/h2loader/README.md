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
