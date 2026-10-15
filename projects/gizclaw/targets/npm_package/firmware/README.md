# GizClaw firmware controller

@gizclaw/firmware is optional. Import it only in a host website that controls an embedded GizClaw firmware HTML page. Firmware pages and display-only embeds run independently without this package.

```js
import { connectFirmware } from '@gizclaw/firmware';

const frame = document.querySelector('#firmware');
const client = await connectFirmware(frame);
const tools = await client.tools.list();
const state = await client.mhs.read({id: 'speaker.main', hwd: 'speaker'});
await client.tools.invoke('lua.app.install', {url: archiveUrl});
client.close();
```

For threaded firmware, keep its existing COOP/COEP hosting headers and delegate cross-origin-isolated to a cross-origin iframe; the firmware response must explicitly permit cross-origin embedding (Cross-Origin-Resource-Policy: cross-origin). The iframe keeps its existing firmware HTML, generated loader, WASM and product lifecycle. The SDK does not load bare WASM, start the firmware, grant browser permissions, stop the firmware, or contain H106 firmware. The product uses the public GizOS C transport with its existing Service; its entry imports no npm/SDK JavaScript. Firmware must expose the version-1 GizOS endpoint before connection completes, usually after its normal Start action.

connectFirmware(frame, {origin?, timeout_ms?}) validates the exact iframe origin and waits up to 30000 ms by default (1..120000). Set origin explicitly when a firmware URL redirects to another origin. Opaque/sandboxed origins are unsupported. The child accepts the embedding parent only; a MessageChannel isolates established calls. One controller is active per firmware frame; reconnecting that frame closes its previous controller. Different frames have independent clients.

The returned client provides tools.list/invoke, mhs.read/write and call(method, Uint8Array, options?). Names, IDs and typed payloads come from the pinned official GizClaw SDK. Calls return Promises and accept {signal?, timeout_ms?}; zero and false preserve optional presence. Requests are copied before transfer, and replies are copied/decoded before delivery acknowledgement. error.code preserves the original signed PAL code; wire-only errors use -1000 and expose rpcCode/rpcMessage. SDK close, navigation and host pagehide reject pending calls; timeout/abort cannot undo writes already executed by the firmware.

close() disconnects this SDK connection and is safe to repeat. Close before removing or navigating the iframe. The firmware continues running and may accept a new controller. The product closes its C adapter before stopping/deinitializing the Service; native failed-join retry retains dependencies.

The package is a self-contained ESM controller plus TypeScript declarations and licenses. Only the controlling host needs to install it. It is packaged by //projects/gizclaw/targets/npm_package/firmware:firmware; :release_tarball builds the deterministic npm tarball consumed by the shared GizOS npm release bundle. Building a tarball does not publish a registry version.
