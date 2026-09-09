// Pack the reviewed clash fade and split result words into H2RS atlases.
// Usage: NODE_PATH=... node pack_clash_result_atlas.mjs storyboard.png
//        fade-h106.h2rs fade-amoled.h2rs result-words.h2rs
//        result-bars-h106.h2rs result-bars-amoled.h2rs
import { createRequire } from 'node:module';
import fs from 'node:fs';
import zlib from 'node:zlib';

const require = createRequire(import.meta.url);
const sharp = require('sharp');
const [source, fadeH106, fadeAmoled, resultWords, barsH106, barsAmoled] = process.argv.slice(2);
if (!source || !fadeH106 || !fadeAmoled || !resultWords || !barsH106 || !barsAmoled) {
  throw new Error('storyboard.png fade-h106.h2rs fade-amoled.h2rs result-words.h2rs result-bars-h106.h2rs result-bars-amoled.h2rs required');
}

const metadata = await sharp(source).metadata();
if (!metadata.width || !metadata.height) throw new Error(`invalid image: ${source}`);

function panelRect(row, column) {
  const left = Math.round(column * metadata.width / 4);
  const top = Math.round(row * metadata.height / 3);
  const right = Math.round((column + 1) * metadata.width / 4);
  const bottom = Math.round((row + 1) * metadata.height / 3);
  return {left, top, width: right - left, height: bottom - top};
}

async function panel(row, column, width, height) {
  return sharp(source)
    .extract(panelRect(row, column))
    .resize(width, height, {fit: 'fill', kernel: sharp.kernel.lanczos3})
    .sharpen({sigma: .5})
    .ensureAlpha(1)
    .raw()
    .toBuffer();
}

function dilate(mask, width, height, radius) {
  const out = Buffer.alloc(mask.length);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    let value = 0;
    for (let yy = Math.max(0, y-radius); yy <= Math.min(height-1, y+radius); yy++) {
      for (let xx = Math.max(0, x-radius); xx <= Math.min(width-1, x+radius); xx++) {
        value = Math.max(value, mask[yy*width+xx]);
      }
    }
    out[y*width+x] = value;
  }
  return out;
}

function erode(mask, width, height, radius) {
  const out = Buffer.alloc(mask.length);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    let value = 255;
    for (let yy = y-radius; yy <= y+radius; yy++) for (let xx = x-radius; xx <= x+radius; xx++) {
      value = Math.min(value, xx < 0 || yy < 0 || xx >= width || yy >= height ? 0 : mask[yy*width+xx]);
    }
    out[y*width+x] = value;
  }
  return out;
}

function keyedPixels(rgb, width, height, seedThreshold=52, candidateThreshold=14) {
  const visited = Buffer.alloc(width*height);
  const connected = Buffer.alloc(width*height);
  const queue = new Int32Array(width*height);
  for(let start=0;start<width*height;start++) {
    const startPixel=start*3;
    if(visited[start] || Math.max(rgb[startPixel],rgb[startPixel+1],rgb[startPixel+2])<candidateThreshold)continue;
    let head=0,tail=0,minY=height,maxY=0,minX=width,maxX=0,hasCore=false;
    visited[start]=1;queue[tail++]=start;
    while(head<tail) {
      const i=queue[head++],x=i%width,y=Math.floor(i/width),p=i*3;
      minX=Math.min(minX,x);maxX=Math.max(maxX,x);minY=Math.min(minY,y);maxY=Math.max(maxY,y);
      hasCore=hasCore || Math.min(rgb[p],rgb[p+1],rgb[p+2])>=seedThreshold;
      for(let yy=Math.max(0,y-1);yy<=Math.min(height-1,y+1);yy++) {
        for(let xx=Math.max(0,x-1);xx<=Math.min(width-1,x+1);xx++) {
          const n=yy*width+xx,np=n*3;
          if(!visited[n] && Math.max(rgb[np],rgb[np+1],rgb[np+2])>=candidateThreshold) {
            visited[n]=1;queue[tail++]=n;
          }
        }
      }
    }
    const componentHeight=maxY-minY+1,componentWidth=maxX-minX+1;
    if(hasCore && tail>=18 && componentHeight>=7 && componentWidth/componentHeight<15) {
      for(let i=0;i<tail;i++)connected[queue[i]]=1;
    }
  }
  const alpha=Buffer.alloc(width*height);
  for(let i=0;i<alpha.length;i++)if(connected[i]) {
    const p=i*3,peak=Math.max(rgb[p],rgb[p+1],rgb[p+2]);
    alpha[i]=Math.max(0,Math.min(255,(peak-candidateThreshold)*2.2));
  }
  return alpha;
}

function keyedTextPixels(rgb,width,height) {
  // Result lettering has a white-hot core; the storyboard speed bars do not.
  // Grow only from that neutral core so a coloured bar touching the crop can
  // never become part of the word sprite, while retaining three pixels of the
  // authored cyan/magenta edge glow.
  const core=Buffer.alloc(width*height);
  for(let i=0;i<core.length;i++) {
    const p=i*3,low=Math.min(rgb[p],rgb[p+1],rgb[p+2]);
    const high=Math.max(rgb[p],rgb[p+1],rgb[p+2]);
    if(low>=72 && high>=118)core[i]=255;
  }
  const envelope=dilate(core,width,height,3);
  const alpha=Buffer.alloc(width*height);
  for(let i=0;i<alpha.length;i++)if(envelope[i]) {
    const p=i*3,peak=Math.max(rgb[p],rgb[p+1],rgb[p+2]);
    alpha[i]=Math.max(0,Math.min(255,(peak-10)*2.35));
  }
  return alpha;
}

function transparentComposite(rgb, alpha, width, height) {
  const rgba=Buffer.alloc(width*height*4);
  for(let i=0;i<alpha.length;i++) {
    const p=i*3,q=i*4,a=alpha[i];
    rgba[q]=a?Math.min(255,Math.round(rgb[p]*255/a)):0;
    rgba[q+1]=a?Math.min(255,Math.round(rgb[p+1]*255/a)):0;
    rgba[q+2]=a?Math.min(255,Math.round(rgb[p+2]*255/a)):0;
    rgba[q+3]=a;
  }
  return rgba;
}

async function wordPair(row, x, y, cropWidth, cropHeight, targetWidth, targetHeight, color,
                        clearLeft=0) {
  const rect = panelRect(row, 3);
  const resized = await sharp(source)
    .extract({left: rect.left+x, top: rect.top+y, width: cropWidth, height: cropHeight})
    .resize(targetWidth, targetHeight, {fit: 'fill', kernel: sharp.kernel.lanczos3})
    .removeAlpha().raw().toBuffer();
  const width = 224, tileHeight = 64, innerWidth = targetWidth, innerHeight = targetHeight;
  const innerMask=keyedTextPixels(resized,innerWidth,innerHeight);
  const mask = Buffer.alloc(width*tileHeight),rgb=Buffer.alloc(width*tileHeight*3);
  const offsetX=Math.floor((width-innerWidth)/2),offsetY=Math.floor((tileHeight-innerHeight)/2);
  for (let iy = 0; iy < innerHeight; iy++) for (let ix = 0; ix < innerWidth; ix++) {
    const src = (iy*innerWidth+ix)*3,dst=(iy+offsetY)*width+ix+offsetX;
    mask[dst]=innerMask[iy*innerWidth+ix];
    rgb[dst*3]=resized[src];rgb[dst*3+1]=resized[src+1];rgb[dst*3+2]=resized[src+2];
  }
  const solid=Buffer.from(mask);for(let i=0;i<solid.length;i++)solid[i]=solid[i]>72?255:0;
  const edge = dilate(solid,width,tileHeight,1);
  const glow = dilate(solid,width,tileHeight,3);
  const inside = erode(solid,width,tileHeight,1);
  const outline = Buffer.alloc(width*tileHeight*4);
  const full = transparentComposite(rgb,mask,width,tileHeight);
  for (let i = 0; i < mask.length; i++) {
    const border = Math.max(0, edge[i]-inside[i]);
    const glowAlpha = Math.max(0, glow[i]-solid[i]);
    const p = i*4;
    outline[p]=color[0];outline[p+1]=color[1];outline[p+2]=color[2];
    outline[p+3]=Math.min(255,border+glowAlpha*.28);
  }
  // The reviewed WIN panel has a disconnected cyan slash just before the Y.
  // It belongs to the background speed-line layer, not to the word artwork.
  // Remove only that cropped margin from both word states.
  if (clearLeft > 0) {
    for (let y = 0; y < tileHeight; y++) for (let x = 0; x < clearLeft; x++) {
      outline[(y*width+x)*4+3]=0;
      full[(y*width+x)*4+3]=0;
    }
  }
  return [outline,full];
}

async function keyedPanel(row,width,height) {
  const rgb=await sharp(source).extract(panelRect(row,0))
    .resize(width,height,{fit:'fill',kernel:sharp.kernel.lanczos3})
    .removeAlpha().raw().toBuffer();
  const alpha=Buffer.alloc(width*height);
  for(let i=0;i<alpha.length;i++) {
    const p=i*3,peak=Math.max(rgb[p],rgb[p+1],rgb[p+2]);
    alpha[i]=Math.max(0,Math.min(255,(peak-4)*1.7));
  }
  const rgba=transparentComposite(rgb,alpha,width,height);
  const palette=row===1
    ? [[14,145,255],[166,42,255]]   // WIN: electric blue into violet
    : [[255,126,24],[255,32,18]];   // LOSE: warning orange into hot red
  for(let y=0;y<height;y++)for(let x=0;x<width;x++) {
    const i=y*width+x,p=i*4;
    if(!rgba[p+3])continue;
    const diagonal=Math.max(0,Math.min(1,x/width*.55+(1-y/height)*.45));
    const sourcePeak=Math.max(rgb[i*3],rgb[i*3+1],rgb[i*3+2])/255;
    const whiteCore=Math.max(0,(sourcePeak-.72)/.28)*.32;
    for(let c=0;c<3;c++) {
      const tint=palette[0][c]+(palette[1][c]-palette[0][c])*diagonal;
      rgba[p+c]=Math.round(tint+(255-tint)*whiteCore);
    }
  }
  return rgba;
}

function shiftClipped(frame,width,height,dx,dy) {
  const shifted=Buffer.alloc(frame.length);
  for(let y=0;y<height;y++)for(let x=0;x<width;x++) {
    const sx=x-dx,sy=y-dy;
    if(sx<0 || sy<0 || sx>=width || sy>=height)continue;
    const source=(sy*width+sx)*4,target=(y*width+x)*4;
    shifted[target]=frame[source];
    shifted[target+1]=frame[source+1];
    shifted[target+2]=frame[source+2];
    shifted[target+3]=frame[source+3];
  }
  return shifted;
}

async function movingBarFrames(row,width,height) {
  const sourceFrame=await keyedPanel(row,width,height);
  const frames=[];
  const frameCount=12;
  // The authored bars run lower-left to upper-right. Each atlas cycle starts
  // mostly off the lower-left edge and exits at the upper-right edge. Both end
  // frames are sparse, so runtime cross-fading can wrap without a tile seam.
  for(let frame=0;frame<frameCount;frame++) {
    const progress=frame/(frameCount-1);
    frames.push(shiftClipped(sourceFrame,width,height,
      Math.round(width*(-.72+1.44*progress)),
      Math.round(height*(.36-.72*progress))));
  }
  return frames;
}

function pack(output, width, height, frames) {
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
  process.stdout.write(`${output}: ${frames.length} frames, ${width}x${height}, ${offset} bytes\n`);
}

const fadeFor = async (width, height) => Promise.all([0,1,2,3].map(column => panel(0,column,width,height)));
pack(fadeH106,240,240,await fadeFor(240,240));
pack(fadeAmoled,184,224,await fadeFor(184,224));
const words = [];
for (const spec of [
  [1,35,110,245,68,176,42,[40,220,255],31], // WIN: YOU; strip pre-Y speed-line fragment
  [1,20,184,280,82,196,45,[40,220,255]],  // WIN: WIN
  [2,35,86,250,72,180,42,[232,70,255],32], // LOSE: YOU; strip pre-Y speed-line fragment
  [2,15,164,285,80,206,47,[232,70,255]],  // LOSE: LOSE
]) words.push(...await wordPair(...spec));
pack(resultWords,224,64,words);
pack(barsH106,240,240,[...await movingBarFrames(1,240,240),
                       ...await movingBarFrames(2,240,240)]);
pack(barsAmoled,184,224,[...await movingBarFrames(1,184,224),
                         ...await movingBarFrames(2,184,224)]);
