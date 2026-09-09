// Pack three reviewed 2x2 VFX storyboards into H106 and AMOLED H2RS atlases.
// Usage: NODE_PATH=... node pack_clash_atlas.mjs equal.png player.png enemy.png h106.h2rs amoled.h2rs
import { createRequire } from 'node:module';
import fs from 'node:fs';
import zlib from 'node:zlib';

const require = createRequire(import.meta.url);
const sharp = require('sharp');
const [equalSource, playerSource, enemySource, h106Output, amoledOutput] = process.argv.slice(2);
if (!equalSource || !playerSource || !enemySource || !h106Output || !amoledOutput) {
  throw new Error('equal.png player.png enemy.png h106.h2rs amoled.h2rs required');
}

async function storyboardFrames(source, width, height) {
  const image = sharp(source);
  const metadata = await image.metadata();
  if (!metadata.width || !metadata.height) throw new Error(`invalid image: ${source}`);
  const halfWidth = Math.floor(metadata.width / 2);
  const halfHeight = Math.floor(metadata.height / 2);
  const frames = [];
  for (let row = 0; row < 2; row++) for (let col = 0; col < 2; col++) {
    const left = col * halfWidth;
    const top = row * halfHeight;
    const cropWidth = col === 0 ? halfWidth : metadata.width - left;
    const cropHeight = row === 0 ? halfHeight : metadata.height - top;
    const frame = await sharp(source)
      .extract({left, top, width: cropWidth, height: cropHeight})
      .resize(width, height, {fit: 'fill', kernel: sharp.kernel.lanczos3})
      .sharpen({sigma: .55})
      .ensureAlpha(1)
      .raw()
      .toBuffer();
    frames.push(frame);
  }
  return frames;
}

async function pack(output, width, height) {
  const frames = [];
  for (const source of [equalSource, playerSource, enemySource]) {
    frames.push(...await storyboardFrames(source, width, height));
  }
  const compressed = frames.map(frame => zlib.deflateSync(frame, {level: 9}));
  let offset = 12 + compressed.length * 8;
  const header = Buffer.alloc(offset);
  header.write('H2RS');
  header.writeUInt16LE(width, 4);
  header.writeUInt16LE(height, 6);
  header.writeUInt16LE(compressed.length, 8);
  compressed.forEach((frame, index) => {
    header.writeUInt32LE(offset, 12 + index * 8);
    header.writeUInt32LE(frame.length, 16 + index * 8);
    offset += frame.length;
  });
  fs.writeFileSync(output, Buffer.concat([header, ...compressed]));
  console.log(`${output}: ${compressed.length} frames, ${width}x${height}, ${offset} bytes`);
}

await pack(h106Output, 240, 240);
// H2RS tiles are bounded to 256x256. Integer 2x composition restores the
// approved 368x448 AMOLED frame without non-integral sampling drift.
await pack(amoledOutput, 184, 224);
