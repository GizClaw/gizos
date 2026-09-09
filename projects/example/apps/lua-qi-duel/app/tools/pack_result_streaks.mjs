// Build ten storyboard-style speed-light archetypes as one transparent H2R8
// sheet: five blue/violet WIN rows, then five orange/red LOSE rows.
// Usage: NODE_PATH=... node pack_result_streaks.mjs output.h2r8
import { createRequire } from 'node:module';
import fs from 'node:fs';
import zlib from 'node:zlib';

const sharp=createRequire(import.meta.url)('sharp');
const [output,preview]=process.argv.slice(2);
if(!output)throw new Error('output.h2r8 required');

const WIDTH=160,HEIGHT=48;
const palettes=[
  '#20e8ff','#328fff','#626eff','#b13cff','#18c8ff',
  '#ffb126','#ff7a18','#ff481c','#ff2638','#ff5c18',
];
const variants=[
  {body:42,width:2.2,slant:2.5,trail:82},
  {body:54,width:4.0,slant:3.2,trail:94},
  {body:47,width:7.0,slant:4.2,trail:105},
  {body:63,width:10.5,slant:5.5,trail:116},
  {body:38,width:5.5,slant:4.0,trail:76},
];

function polygon(x0,x1,center,width,slant) {
  const top=center-width/2,bottom=center+width/2;
  return `${x0+slant},${top} ${x1},${top} ${x1-slant},${bottom} ${x0},${bottom}`;
}

function tileSvg(color,index) {
  const v=variants[index%variants.length],head=154,start=head-v.body,center=24;
  const trailStart=Math.max(2,start-v.trail);
  const middleStart=Math.max(4,start-v.trail*.68);
  const faintStart=Math.max(1,start-v.trail*1.12);
  return Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${WIDTH}" height="${HEIGHT}" viewBox="0 0 ${WIDTH} ${HEIGHT}">
    <defs>
      <filter id="wide" x="-30%" y="-100%" width="170%" height="300%"><feGaussianBlur stdDeviation="5.2"/></filter>
      <filter id="body" x="-25%" y="-100%" width="160%" height="300%"><feGaussianBlur stdDeviation="2.8"/></filter>
    </defs>
    <polygon points="${polygon(faintStart,start+v.body*.18,center,v.width*2.7,v.slant*1.6)}" fill="${color}" opacity=".055" filter="url(#wide)"/>
    <polygon points="${polygon(trailStart,start+v.body*.28,center,v.width*1.75,v.slant*1.25)}" fill="${color}" opacity=".12"/>
    <polygon points="${polygon(middleStart,start+v.body*.52,center,v.width*1.18,v.slant)}" fill="${color}" opacity=".22"/>
    <polygon points="${polygon(start-8,head,center,v.width*1.75,v.slant*1.35)}" fill="${color}" opacity=".52" filter="url(#body)"/>
    <polygon points="${polygon(start,head,center,v.width,v.slant)}" fill="${color}" opacity=".98"/>
    <polygon points="${polygon(start+v.body*.20,head-3,center,v.width*.24,Math.max(1,v.slant*.32))}" fill="#f4fdff" opacity=".82"/>
  </svg>`);
}

const composites=palettes.map((color,row)=>({
  input:tileSvg(color,row%5),left:0,top:row*HEIGHT,
}));
const rgba=await sharp({create:{width:WIDTH,height:HEIGHT*palettes.length,
  channels:4,background:{r:0,g:0,b:0,alpha:0}}})
  .composite(composites).raw().toBuffer();
const header=Buffer.alloc(8);
header.write('H2R8');header.writeUInt16LE(WIDTH,4);header.writeUInt16LE(HEIGHT*palettes.length,6);
fs.writeFileSync(output,Buffer.concat([header,zlib.deflateSync(rgba,{level:9})]));
if(preview)await sharp(rgba,{raw:{width:WIDTH,height:HEIGHT*palettes.length,channels:4}})
  .png().toFile(preview);
console.log(`${output}: 10 independent speed-light styles; ${WIDTH}x${HEIGHT*palettes.length} RGBA`);
