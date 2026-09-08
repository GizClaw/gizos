// Mechanical runtime packing of the approved 3/2/1 sheet, not artwork generation.
// Usage: NODE_PATH=... node pack_countdown.mjs source.png output.h2r8
import { createRequire } from 'node:module';
import fs from 'node:fs';
import zlib from 'node:zlib';
const sharp = createRequire(import.meta.url)('sharp');
const [source, output] = process.argv.slice(2);
if (!source || !output) throw Error('source.png output.h2r8 required');
const {data, info}=await sharp(source).removeAlpha().raw().toBuffer({resolveWithObject:true});
const boxes=[];
for(let n=0;n<3;n++) {
  let x0=info.width,y0=info.height,x1=0,y1=0;
  for(let y=0;y<info.height;y++)for(let x=Math.floor(n*info.width/3);x<Math.floor((n+1)*info.width/3);x++) {
    const p=(y*info.width+x)*3;
    if(Math.max(data[p],data[p+1],data[p+2])<80)continue;
    x0=Math.min(x0,x);y0=Math.min(y0,y);x1=Math.max(x1,x+1);y1=Math.max(y1,y+1);
  }
  if(x1<=x0)throw Error('missing digit');
  boxes.push({x0,y0,x1,y1});
}
const top=Math.min(...boxes.map(b=>b.y0))-40;
const bottom=Math.max(...boxes.map(b=>b.y1))+40;
const span=bottom-top, size=64, atlas=Buffer.alloc(size*size*3*4);
for(let n=0;n<3;n++) {
  const b=boxes[n], left=Math.round((b.x0+b.x1-span)/2);
  const rgb=await sharp(source).extract({left,top,width:span,height:span})
    .resize(size,size,{kernel:'lanczos3'}).removeAlpha().raw().toBuffer();
  for(let p=0;p<size*size;p++) {
    const alpha=Math.max(rgb[p*3],rgb[p*3+1],rgb[p*3+2]);
    const dst=(n*size*size+p)*4;
    // Unmatte luminous RGB from black. Over black the result preserves the
    // approved colors, while gaps/glow no longer mask the arena underneath.
    if(alpha<=3)continue;
    for(let c=0;c<3;c++)atlas[dst+c]=Math.round(rgb[p*3+c]*255/alpha);
    atlas[dst+3]=alpha;
  }
}
const header=Buffer.alloc(8);header.write('H2R8');
header.writeUInt16LE(size,4);header.writeUInt16LE(size*3,6);
fs.writeFileSync(output,Buffer.concat([header,zlib.deflateSync(atlas,{level:9})]));
console.log(`${output}: 3,2,1 rows; ${size}x${size*3} RGBA`);
