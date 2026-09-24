import assert from "node:assert/strict";
import {
  H2LoaderCapabilities,
  H2LoaderCommands,
  commandAvailable,
} from "../src/h2loader_protocol.js";

assert.equal(H2LoaderCapabilities.ALL, 0x7);
assert.equal(H2LoaderCommands.REBOOT_UPGRADE, 1 << 19);
assert.equal(H2LoaderCommands.WIFI_STATUS, 1 << 20);
assert.equal(H2LoaderCommands.ALL, 0x001f3f3f);
assert.equal(commandAvailable(H2LoaderCommands.STATUS, H2LoaderCommands.STATUS), true);
assert.equal(commandAvailable(0, H2LoaderCommands.STATUS), false);
assert.equal(commandAvailable(0x001f3d3e, H2LoaderCommands.STATUS), true);
assert.equal(commandAvailable(0x001f3d3e, H2LoaderCommands.WIFI_STATUS), true);

assert.throws(() => commandAvailable(0, 3), TypeError);
assert.throws(() => commandAvailable(1 << 6, H2LoaderCommands.STATUS), TypeError);
