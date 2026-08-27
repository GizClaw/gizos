import assert from "node:assert/strict";
import test from "node:test";

import { createH2LoaderPortRegistry } from "../src/h2loader_ports.js";
import {
  H2LoaderCapabilities,
  H2LoaderCommands,
  commandAvailable,
  decodeH2LoaderStates,
} from "../src/h2loader_protocol.js";

test("exports and validates the H2Loader status protocol", () => {
  assert.equal(H2LoaderCapabilities.ALL, 0x7);
  assert.equal(H2LoaderCommands.LOADER_UPGRADE, 1 << 19);
  assert.equal(
    commandAvailable(H2LoaderCommands.STATUS, H2LoaderCommands.STATUS), true);
  assert.equal(commandAvailable(0, H2LoaderCommands.STATUS), false);

  const app = decodeH2LoaderStates("0x000000000001586a");
  assert.equal(app.role, "app");
  assert.equal(app.installState, "confirmed");
  assert.equal(app.appConfirmed, true);
  assert.equal(app.installedValid, true);
  assert.equal(app.mfgMode, "disabled");
  assert.deepEqual(app.mfgSteps, Array(22).fill(0));

  assert.throws(() => decodeH2LoaderStates("0x1586a"), TypeError);
  assert.throws(() => decodeH2LoaderStates("0x0000000000010914"), RangeError);
  assert.throws(() => decodeH2LoaderStates("0xc000000000010915"), RangeError);
  assert.throws(() => commandAvailable(0, 3), TypeError);
});

test("exposes the canonical Web Serial ID and preserves object identity", () => {
  const ports = createH2LoaderPortRegistry();
  const first = ports.wrap({
    id: "web-serial-1",
    usbProductId: 0x1001,
    usbVendorId: 0x303a,
  });

  assert.equal(first.id, "web-serial-1");
  assert.equal(ports.id(first), "web-serial-1");
  assert.equal(ports.wrap({ id: "web-serial-1" }), first);
  assert.equal(Object.isFrozen(first), true);
});

test("keeps identical USB devices distinct by canonical ID", () => {
  const ports = createH2LoaderPortRegistry();
  const first = ports.wrap({
    id: "web-serial-1",
    usbProductId: 0x1001,
    usbVendorId: 0x303a,
  });
  const second = ports.wrap({
    id: "web-serial-2",
    usbProductId: 0x1001,
    usbVendorId: 0x303a,
  });

  assert.notEqual(first, second);
  assert.notEqual(first.id, second.id);
  assert.deepEqual([first.id, second.id], ["web-serial-1", "web-serial-2"]);
});

test("removes forgotten ports without reusing another device object", () => {
  const ports = createH2LoaderPortRegistry();
  const forgotten = ports.wrap({ id: "web-serial-1" });
  const retained = ports.wrap({ id: "web-serial-2" });

  ports.delete(forgotten);

  assert.equal(ports.id(forgotten), undefined);
  assert.notEqual(ports.wrap({ id: "web-serial-1" }), forgotten);
  assert.equal(ports.wrap({ id: "web-serial-2" }), retained);
});
