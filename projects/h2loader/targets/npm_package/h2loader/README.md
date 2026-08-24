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
  console.log(ports);
} finally {
  await loader.close();
}
```

The SDK requires a secure browser context and a browser with Web Serial support. npm is the distribution format for browser applications; this package does not provide a Node.js serial-port runtime.
