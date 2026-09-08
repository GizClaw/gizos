// Build-time packing only. Artwork/poses are authored by imagegen, not this tool.
// Usage: NODE_PATH=... node pack_action_atlas.mjs source.png output.h2rs
// Black-key source uses the same additive-dark art convention as the game.
import { createRequire } from 'node:module';
import fs from 'node:fs';
import zlib from 'node:zlib';
const require = createRequire(import.meta.url);
const sharp = require('sharp');
const [source, output] = process.argv.slice(2);
if (!source || !output) throw new Error('source.png output.h2rs required');
const { data, info } = await sharp(source).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
const tiles = [];
const size = 192;
for (let row = 0; row < 4; row++) for (let col = 0; col < 4; col++) {
  const opponent=source.includes('opponent');
  // Reviewed sheet's row baselines are slightly offset from the nominal grid.
  // Import rectangles include the full boots; never cut through the silhouette.
  const rows=opponent?[0,326,635,945,1254]:[0,314,628,941,1254];
  const left = Math.round(col * info.width / 4), top = Math.round(rows[row]*info.height/1254);
  const width = Math.round((col + 1) * info.width / 4) - left;
  const height = Math.round(rows[row+1]*info.height/1254) - top;
  const rgba = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const src = ((y + top) * info.width + x + left) * 4, dst = (y * width + x) * 4;
    data.copy(rgba, dst, src, src + 4);
  }
  // Flood only the outer black background, keeping enclosed dark armor opaque.
  // Explicit color-key import, never a white/checkerboard removal heuristic.
  const seen = new Uint8Array(width * height), queue = [];
  const visit = (x, y) => {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const p = y * width + x;
    if (seen[p] || Math.max(rgba[p*4],rgba[p*4+1],rgba[p*4+2]) > 15) return;
    seen[p] = 1; queue.push(p);
  };
  for (let x = 0; x < width; x++) { visit(x,0); visit(x,height-1); }
  for (let y = 0; y < height; y++) { visit(0,y); visit(width-1,y); }
  for (let i = 0; i < queue.length; i++) {
    const p = queue[i], x = p % width, y = Math.floor(p / width);
    rgba[p*4+3] = 0;
    visit(x-1,y); visit(x+1,y); visit(x,y-1); visit(x,y+1);
  }
  // Some enclosed black gaps between limbs are intentionally retained black;
  // no per-frame geometry guessing or cuts through the limb artwork.
  let tile;
  if(opponent) {
    let x0=width,y0=height,x1=0,y1=0;
    for(let y=0;y<height;y++)for(let x=0;x<width;x++)if(rgba[(y*width+x)*4+3]){
      x0=Math.min(x0,x);y0=Math.min(y0,y);x1=Math.max(x1,x+1);y1=Math.max(y1,y+1);
    }
    if(x1<=x0 || y1<=y0)throw Error('empty pose');
    const figure=await sharp(rgba,{raw:{width,height,channels:4}})
      .extract({left:x0,top:y0,width:x1-x0,height:y1-y0})
      .resize({width:176,height:166,fit:'inside'}).png().toBuffer();
    const fm=await sharp(figure).metadata();
    tile=await sharp({create:{width:size,height:size,channels:4,background:{r:0,g:0,b:0,alpha:0}}})
      .composite([{input:figure,left:Math.floor((size-fm.width)/2),top:177-fm.height}]).raw().toBuffer();
  } else {
    tile=await sharp(rgba,{raw:{width,height,channels:4}}).resize(size,size).raw().toBuffer();
  }
  tiles.push(zlib.deflateSync(tile,{level:9}));
}
let offset = 12 + tiles.length * 8;
const header = Buffer.alloc(offset);
header.write('H2RS'); header.writeUInt16LE(size,4); header.writeUInt16LE(size,6);
header.writeUInt16LE(tiles.length,8);
tiles.forEach((tile,i) => {
  header.writeUInt32LE(offset,12+i*8); header.writeUInt32LE(tile.length,16+i*8);
  offset += tile.length;
});
fs.writeFileSync(output,Buffer.concat([header,...tiles]));
console.log(`${output}: ${tiles.length} frames, ${size}x${size}, ${offset} bytes`);
