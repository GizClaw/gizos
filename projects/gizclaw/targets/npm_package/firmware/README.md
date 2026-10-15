# GizClaw firmware SDK

The running firmware provides `Module.h2GizClaw` without any npm dependency. `@gizclaw/firmware` is an optional object and Promise facade over that same runtime interface. Import it where application code needs the convenient API; firmware startup and raw calls work without it. Merely installing the package does not choose a runtime: pass the already running WASM Module explicitly.

```js
import { createFirmwareClient } from '@gizclaw/firmware';

// The existing firmware has started and registered its Service on this Module.
const client = createFirmwareClient(Module);
const tools = await client.tools.list();
const state = await client.mhs.read({id: 'speaker.main', hwd: 'speaker'});
await client.tools.invoke('lua.app.install', {url: archiveUrl});
client.close();
```

Without the package, the firmware's byte interface remains available:

```js
const id = Module.h2GizClaw.request(136, new Uint8Array(), response => {
  if (response.code !== 0 || response.hasError) return -10;
  console.log(response.nativeTools); // Existing tool-list method.
  return 0; // Synchronous delivery acknowledgement after consuming the reply.
});
if (id < 0) console.error('Request was not admitted:', id);
// Module.h2GizClaw.cancel(id) cancels an accepted call.
```

The raw `request(method, bytes, callback, {timeout_ms?})` accepts canonical method IDs and protobuf bytes. It returns a positive call ID, or a negative PAL admission error with no callback. Accepted callbacks run exactly once, asynchronously after request returns, on the browser main thread. A response contains code, hasError, rpcCode, rpcMessage, copied payload and optional nativeTools. Return a signed integer delivery result synchronously: zero after successful consumption, negative after failure. Throwing or returning a non-integer fails delivery with FORMAT (-15). An async callback is not supported; callers may write their own Promise wrapper while keeping the callback's acknowledgement synchronous. The firmware exposes version, request_capacity, request_limit_bytes, timeout_ms and closed. `cancel(id)` returns zero for a live call, NOT_FOUND (-8) for a finished/unknown call, INVALID_ARG (-1) for an invalid ID and CLOSED (-10) after detach. Cancellation/timeout invokes the accepted callback with CLOSED (-10)/TIMEOUT (-6) once.

`createFirmwareClient(Module)` requires the version-1 byte interface to be ready and throws CLOSED (-10) before registration/after close, UNSUPPORTED (-3) for an incompatible version. It provides tools.list/invoke, mhs.read/write and `call(method, Uint8Array, options?)`. Names, IDs and payload types come from the pinned official GizClaw SDK. Promise calls accept `{signal?, timeout_ms?}`; zero and false preserve optional presence. Request/reply bytes are copied and replies are decoded before successful native delivery completion. error.code preserves signed PAL codes; wire-only errors use -1000 and expose rpcCode/rpcMessage. Limits/default timeout come from the selected runtime. Timeout and abort cannot undo writes already executed.

Multiple clients can share a runtime and are bounded by its shared request capacity; separate runtimes remain independent. close() is safe to repeat and cancels only that client's calls. Other clients, raw calls and firmware continue running. Native adapter close or page teardown invalidates the runtime interface and pending calls; old byte-API references remain closed. C failed-join retry retains its dependencies.

The product registers its existing Service through `h2_gizclaw_local_start()` and `h2_web_gizclaw_open()`, whose firmware target includes the byte binding. Its entry imports no npm SDK or hand-assembled SDK JavaScript. Existing tool/MHS handlers and Lua implementations own all behavior. The SDK binds an existing runtime; it does not load WASM or own its HTML, startup, permissions or lifecycle. An iframe is not required. The package does not add a cross-origin iframe transport; code with access to a runtime can bind it directly.

The package is self-contained ESM plus TypeScript declarations and licenses, with no firmware WASM or runtime compiler dependency. `//projects/gizclaw/targets/npm_package/firmware:release_tarball` builds its deterministic tarball for the shared GizOS npm release bundle. Building it does not publish a registry version.
