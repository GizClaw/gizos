// Packs the approved COMBO / ARMOR BREAK artwork into two fixed RGBA rows.
// Usage: NODE_PATH=... node pack_impact_labels.mjs combo.png armor-break.png output.h2r8
import { createRequire } from 'node:module';
import fs from 'node:fs';
import zlib from 'node:zlib';

const sharp = createRequire(import.meta.url)('sharp');
const [comboSource, breakSource, output] = process.argv.slice(2);
if (!comboSource || !breakSource || !output) {
  throw Error('combo.png armor-break.png output.h2r8 required');
}

const CELL_WIDTH = 192;
const CELL_HEIGHT = 96;

async function fit(source, width, height) {
  const image = sharp(source).ensureAlpha().trim({ threshold: 3 });
  const { data, info } = await image
    .resize(width, height, {
      fit: 'inside',
      kernel: 'lanczos3',
      withoutEnlargement: true,
    })
    .raw()
    .toBuffer({ resolveWithObject: true });
  const left = Math.floor((CELL_WIDTH - info.width) / 2);
  const top = Math.floor((CELL_HEIGHT - info.height) / 2);
  return { input: data, raw: info, left, top };
}

const combo = await fit(comboSource, 184, 70);
const armorBreak = await fit(breakSource, 178, 88);
const atlas = await sharp({
  create: {
    width: CELL_WIDTH,
    height: CELL_HEIGHT * 2,
    channels: 4,
    background: { r: 0, g: 0, b: 0, alpha: 0 },
  },
})
  .composite([
    combo,
    { ...armorBreak, top: CELL_HEIGHT + armorBreak.top },
  ])
  .raw()
  .toBuffer();

const header = Buffer.alloc(8);
header.write('H2R8');
header.writeUInt16LE(CELL_WIDTH, 4);
header.writeUInt16LE(CELL_HEIGHT * 2, 6);
fs.writeFileSync(output, Buffer.concat([header, zlib.deflateSync(atlas, { level: 9 })]));
console.log(`${output}: COMBO / ARMOR BREAK rows; ${CELL_WIDTH}x${CELL_HEIGHT * 2} RGBA`);
