"use strict";

const path = require("node:path");

const packageRoot = process.argv[2];
if (!packageRoot) {
  throw new Error("missing lv_font_conv package path");
}
const cli = require(path.resolve(packageRoot, "lib", "cli"));

cli.run(process.argv.slice(3)).catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
