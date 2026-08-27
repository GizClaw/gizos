import assert from "node:assert/strict";
import {
  H2LoaderCapabilities,
  H2LoaderCommands,
  commandAvailable,
  decodeH2LoaderStates,
} from "../src/h2loader_protocol.js";

assert.equal(H2LoaderCapabilities.ALL, 0x7);
assert.equal(H2LoaderCommands.LOADER_UPGRADE, 1 << 19);
assert.equal(commandAvailable(H2LoaderCommands.STATUS, H2LoaderCommands.STATUS), true);
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
