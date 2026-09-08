// Reuse approved glass-icon focus sprites; bake tint and alpha-masked sheen.
// node pack_skill_tints.mjs skill-styles.h2rs skill-colors.h2rs
import fs from 'node:fs';
import zlib from 'node:zlib';
const [source,output]=process.argv.slice(2), input=fs.readFileSync(source);
const w=input.readUInt16LE(4),h=input.readUInt16LE(6),count=input.readUInt16LE(8);
if(input.toString('ascii',0,4)!=='H2RS'||count!==408)throw Error('expected 408 glass focus styles');
const palette=[[.2,.82,1],[1,.53,.13],[.73,.3,1],[.2,1,.58]];
const originals=Array.from({length:count},(_,i)=>zlib.inflateSync(input.subarray(
  input.readUInt32LE(12+i*8),input.readUInt32LE(12+i*8)+input.readUInt32LE(16+i*8))));
function tint(source,index,sheen=-1,locked=false) {
  const pixels=Buffer.from(source),color=palette[index];
  for(let p=0;p<pixels.length;p+=4) {
    const light=Math.max(source[p],source[p+1],source[p+2])/255;
    const highlight=Math.pow(light,5)*.4;
    const x=(p/4)%w,y=Math.floor(p/4/w);
    const stripe=sheen<0?0:Math.max(0,1-Math.abs((x+y*.3)/w-(-.15+sheen*1.6))/.09);
    for(let c=0;c<3;c++) {
      const colored=source[p+c]*(color[c]+(1-color[c])*highlight);
      pixels[p+c]=Math.min(255,Math.round(colored*(locked?1.15:1)+(255-colored)*stripe*.85));
    }
  }
  return zlib.deflateSync(pixels,{level:9});
}
const tiles=originals.map((p,i)=>tint(p,Math.floor(i/102)));
// Per skill: 20 confirmation-sheen frames, then a steady bright locked frame.
for(let i=0;i<4;i++)for(let frame=0;frame<21;frame++)
  tiles.push(tint(originals[i*102+33],i,frame===20?-1:frame/19,true));
let offset=12+tiles.length*8;
const header=Buffer.alloc(offset);header.write('H2RS');header.writeUInt16LE(w,4);
header.writeUInt16LE(h,6);header.writeUInt16LE(tiles.length,8);
tiles.forEach((tile,i)=>{header.writeUInt32LE(offset,12+i*8);header.writeUInt32LE(tile.length,16+i*8);offset+=tile.length;});
fs.writeFileSync(output,Buffer.concat([header,...tiles]));
console.log(`${tiles.length} tinted/confirmation frames, ${offset} bytes`);
