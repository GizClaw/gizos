import assert from 'node:assert/strict';
import { createFirmwareClient } from '@gizclaw/firmware';

// No DOM, iframe, MessageChannel or ambient WASM singleton is required.
assert.equal(globalThis.window, undefined);
const callbacks = new Map();
let serial = 0, delivery;
const abi = {
  version: 1, request_capacity: 4, request_limit_bytes: 1, timeout_ms: 30000, closed: false,
  request(method, bytes, callback) {
    const id = ++serial;
    if (method === 9) { callbacks.set(id, callback); return id; }
    queueMicrotask(() => {
      delivery = callback({code: method === 2 ? 2 ** 40 : 0, hasError: false,
        rpcCode: 0, rpcMessage: '', payload: new Uint8Array([1, 2, 3]), nativeTools: null});
    });
    return id;
  },
  cancel(id) {
    const callback = callbacks.get(id);
    if (!callback) return -8;
    callbacks.delete(id);
    callback({code: -10, hasError: false, rpcCode: 0, rpcMessage: '', payload: new Uint8Array(), nativeTools: null});
    return 0;
  },
};
const runtime = {h2GizClaw: abi};
const client = createFirmwareClient(runtime), sibling = createFirmwareClient(runtime);
assert.deepEqual(await client.call(1, new Uint8Array()), new Uint8Array([1, 2, 3]));
assert.equal(delivery, 0); // Request-byte bound does not constrain replies.
await assert.rejects(client.call(1, new Uint8Array(2)), {code: -13});
await assert.rejects(client.call(2, new Uint8Array()), {code: -15});
assert.equal(delivery, -15);
const controller = new AbortController();
const aborted = client.call(9, new Uint8Array(), {signal: controller.signal});
controller.abort();
await assert.rejects(aborted, {code: -10});
const closed = client.call(9, new Uint8Array());
client.close(); client.close();
await assert.rejects(closed, {code: -10});
await assert.rejects(client.call(1, new Uint8Array()), {code: -10});
assert.deepEqual(await sibling.call(1, new Uint8Array()), new Uint8Array([1, 2, 3]));
assert.equal(callbacks.size, 0);
runtime.h2GizClaw = {...abi};
await assert.rejects(sibling.call(1, new Uint8Array()), {code: -10});
assert.throws(() => createFirmwareClient({h2GizClaw: {...abi, version: 99}}), {code: -3});
assert.throws(() => createFirmwareClient({h2GizClaw: null}), {code: -10});
console.log('GIZCLAW_FIRMWARE_NPM runtime_without_dom=PASS');
