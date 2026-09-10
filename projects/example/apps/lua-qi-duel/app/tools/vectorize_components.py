#!/usr/bin/env python3
"""Extract simplified, filled vector contours from the approved component art.

Build/review tool only (Pillow + numpy); the runtime receives path commands,
not pixels, PNGs, texture atlases, or a raster decoder. Palette regions share
boundaries and preserve hole winding. RDP removes pixel staircases. The SVGs
are editable source geometry and use the original component coordinate space.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib
import math
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SHEETS = {'hud-states':56, 'charge-cells':45, 'countdown':64,
          'impact-labels':96, 'result-streaks':48}
NAMES = ['hud-states','carousel-base','charge-cells','countdown','impact-labels',
         'result-streaks','result-words','action-hands','action-opponent',
         'beam-clash-h106','beam-clash-amoled','beam-clash-fade-h106','beam-clash-fade-amoled']

def frames(name):
    folder=ROOT/'assets/generated'
    p=folder/(name+'.h2r8')
    if p.exists():
        data=p.read_bytes(); w,h=struct.unpack_from('<HH',data,4)
        im=Image.frombytes('RGBA',(w,h),zlib.decompress(data[8:]))
        step=SHEETS.get(name,h)
        return [im.crop((0,y,w,y+step)) for y in range(0,h,step)],p
    p=folder/(name+'.h2rs'); data=p.read_bytes();w,h,n=struct.unpack_from('<HHH',data,4)
    result=[]
    for i in range(n):
        offset,size=struct.unpack_from('<II',data,12+8*i)
        result.append(Image.frombytes('RGBA',(w,h),zlib.decompress(data[offset:offset+size])))
    return result,p

def simplify(p,epsilon):
    if len(p)<3:return p
    ax,ay=p[0]; bx,by=p[-1];dx,dy=bx-ax,by-ay;den=dx*dx+dy*dy
    best,index=0,0
    for i,(x,y) in enumerate(p[1:-1],1):
        t=max(0,min(1,((x-ax)*dx+(y-ay)*dy)/den)) if den else 0
        d=(x-ax-t*dx)**2+(y-ay-t*dy)**2
        if d>best:best,index=d,i
    if best>epsilon*epsilon:
        return simplify(p[:index+1],epsilon)[:-1]+simplify(p[index:],epsilon)
    return [p[0],p[-1]]

def contours(mask,epsilon):
    # Clockwise outer contours, counterclockwise holes (nonzero fill rule).
    h,w=mask.shape; pad=np.pad(mask,1)
    edges={}
    def edge(a,b):edges.setdefault(a,[]).append(b)
    for y,x in zip(*np.nonzero(mask & ~pad[:-2,1:-1])):edge((int(x),int(y)),(int(x+1),int(y)))
    for y,x in zip(*np.nonzero(mask & ~pad[1:-1,2:])):edge((int(x+1),int(y)),(int(x+1),int(y+1)))
    for y,x in zip(*np.nonzero(mask & ~pad[2:,1:-1])):edge((int(x+1),int(y+1)),(int(x),int(y+1)))
    for y,x in zip(*np.nonzero(mask & ~pad[1:-1,:-2])):edge((int(x),int(y+1)),(int(x),int(y)))
    result=[]
    while edges:
        first=next(iter(edges));p=[first];at=first
        while True:
            nxt=edges[at].pop()
            if not edges[at]:del edges[at]
            p.append(nxt);at=nxt
            if at==first:break
        # Split at a distant point so a closed contour isn't simplified away.
        mid=max(range(len(p)-1),key=lambda i:(p[i][0]-first[0])**2+(p[i][1]-first[1])**2)
        q=simplify(p[:mid+1],epsilon)[:-1]+simplify(p[mid:],epsilon)[:-1]
        if len(q)>=3:result.append(q)
    return result

def clip_plane(loop,normal,offset,positive):
    result=[]
    for a,b in zip(loop,loop[1:]+loop[:1]):
        da=a[0]*normal[0]+a[1]*normal[1]-offset
        db=b[0]*normal[0]+b[1]*normal[1]-offset
        ain=da>=0 if positive else da<=0;bin=db>=0 if positive else db<=0
        if ain:result.append(a)
        if ain!=bin:
            t=da/(da-db);result.append((a[0]+t*(b[0]-a[0]),a[1]+t*(b[1]-a[1])))
    return result

def absorb_pose(loops,frame,right):
    # Corrected user direction: ABSORB, not charge. Upper left arm and lower
    # right arm are horizontal and parallel; each palm and forearm share an
    # axis. The shared transverse wrist basis keeps both pieces joined.
    anchors=[
        [((32,118),(18,-54),(34,-66)),((162,120),(-24,-58),(-32,-60))],
        [((56,69),(41,-88),(50,-34)),((147,152),(-45,-33),(-47,-4))],
        [((52,69),(35,-91),(53,-24)),((150,164),(-34,-26),(-48,-3))],
    ][frame-1]
    def unit(v):
        length=math.hypot(*v);return (v[0]/length,v[1]/length)
    def cross(a,b):return a[0]*b[1]-a[1]*b[0]
    result=[]
    for loop in loops:
        wrist,forearm,palm=anchors[int(right)]
        forearm,palm=unit(forearm),unit(palm)
        axis=unit((forearm[0]+palm[0],forearm[1]+palm[1]));normal=(-axis[1],axis[0])
        offset=wrist[0]*axis[0]+wrist[1]*axis[1]
        for hand in (False,True):
            part=clip_plane(loop,axis,offset,hand)
            if len(part)<3:continue
            direction=palm if hand else forearm;den=cross(direction,normal)
            transformed=[];sign=-1 if right else 1
            for x,y in part:
                relative=(x-wrist[0],y-wrist[1])
                along=cross(relative,normal)/den;across=cross(direction,relative)/den
                transformed.append(((162 if right else 55)+sign*along,(118 if right else 72)+sign*across))
            result.append(transformed)
    return result

def hand_masks(rgba):
    # Assign whole connected silhouettes before extracting palette contours.
    # A diagonal cut through the image would cut off a forearm into the other hand.
    opaque=rgba[:,:,3]>4;seen=np.zeros_like(opaque);components=[]
    height,width=opaque.shape
    for y,x in zip(*np.where(opaque)):
        if seen[y,x]:continue
        points=[(int(x),int(y))];seen[y,x]=True
        for px,py in points:
            for nx,ny in ((px-1,py),(px+1,py),(px,py-1),(px,py+1)):
                if 0<=nx<width and 0<=ny<height and opaque[ny,nx] and not seen[ny,nx]:
                    seen[ny,nx]=True;points.append((nx,ny))
        components.append(np.array(points))
    components.sort(key=len,reverse=True)
    hands=sorted(components[:2],key=lambda points:points[:,0].mean())
    masks=[np.zeros_like(opaque),np.zeros_like(opaque)]
    for points in components:
        side=min(range(2),key=lambda side:np.min(np.sum((points.mean(axis=0)-hands[side])**2,axis=1)))
        masks[side][points[:,1],points[:,0]]=True
    assert all(mask.sum()>3000 for mask in masks), 'Expected two complete hand silhouettes'
    return masks

def vectorize(im,colors,epsilon,pose=None):
    rgba=np.array(im)
    rgba[rgba[:,:,3]<5]=0
    im=Image.fromarray(rgba)
    quant=im.quantize(colors=colors,method=Image.Quantize.FASTOCTREE,dither=Image.Dither.NONE)
    indexed=np.array(quant);palette=quant.getpalette('RGBA')
    paths=[]
    masks=hand_masks(rgba) if pose is not None else None
    for index in sorted(np.unique(indexed),key=lambda k:sum(palette[k*4:k*4+3])):
        color=palette[index*4:index*4+4]
        if not color[3]:continue
        if pose is None:loops=contours(indexed==index,epsilon)
        else:
            loops=[]
            for right,mask in enumerate(masks):
                loops.extend(absorb_pose(contours((indexed==index)&mask,epsilon),pose,bool(right)))
        if loops:paths.append((color,loops))
    w,h=im.size; binary=bytearray(b'H2VG'+struct.pack('<HHHH',w,h,0,1))
    svg=[f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}">']
    vertices=0
    for color,loops in paths:
        binary.append(4);parts=[]
        for loop in loops:
            vertices+=len(loop)
            binary.extend(struct.pack('<BH',13,len(loop)))
            for x,y in loop:binary.extend(struct.pack('<hh',round(x*4),round(y*4)))
            parts.append('M'+' L'.join(f'{x:.2f},{y:.2f}' for x,y in loop)+'Z')
        # A very small same-color seam seal avoids transparent cracks where
        # independently antialiased adjoining palette regions meet.
        for stroke,width in [(1,.45),(0,0)]:
            binary.extend(struct.pack('<BBhBBBBff',10,stroke,-1,*color,width,1))
        col='#%02x%02x%02x'%tuple(color[:3]);alpha=color[3]/255
        svg.append(f'<path d="{" ".join(parts)}" fill="{col}" fill-opacity="{alpha:.5f}" stroke="{col}" stroke-opacity="{alpha:.5f}" stroke-width=".45" stroke-linejoin="round"/>')
    binary.append(0);svg.append('</svg>')
    return bytes(binary),'\n'.join(svg)+'\n',vertices,len(paths)

def analytic_streak(row):
    # Port of pack_result_streaks.mjs: preserve its six polygon layers,
    # palette, bevels and dimensions instead of tracing blurred color pixels.
    palette=['20e8ff','328fff','626eff','b13cff','18c8ff','ffb126','ff7a18','ff481c','ff2638','ff5c18']
    body,width,slant,trail=[(42,2.2,2.5,82),(54,4,3.2,94),(47,7,4.2,105),(63,10.5,5.5,116),(38,5.5,4,76)][row%5]
    color=tuple(bytes.fromhex(palette[row]));start=154-body
    binary=bytearray(b'H2VG'+struct.pack('<HHHH',160,48,0,1));svg=['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 48">'];count=0
    def polygon(x0,x1,w,s,alpha,col=color):
        nonlocal count
        if w<=0 or x1<=x0 or alpha<.0001:return
        points=[(x0+s,24-w/2),(x1,24-w/2),(x1-s,24+w/2),(x0,24+w/2)]
        binary.append(4);binary.extend(struct.pack('<BH',13,4))
        for x,y in points:binary.extend(struct.pack('<hh',round(x*4),round(y*4)))
        binary.extend(struct.pack('<BBhBBBBff',10,0,-1,*col,255,0,alpha))
        path='M'+' L'.join(f'{x:.3f},{y:.3f}' for x,y in points)+'Z'
        svg.append(f'<path d="{path}" fill="#{bytes(col).hex()}" opacity="{alpha:.6f}"/>');count+=1
    def blur(x0,x1,w,s,alpha,sigma):
        radius=math.ceil(sigma*2);weights=[math.exp(-k*k/(2*sigma*sigma)) for k in range(-radius,radius+1)];total=sum(weights)
        for k,weight in zip(range(-radius,radius+1),weights):
            polygon(x0-k,x1+k,w+2*k,s,1-(1-alpha)**(weight/total))
    blur(max(1,start-trail*1.12),start+body*.18,width*2.7,slant*1.6,.055,5.2)
    polygon(max(2,start-trail),start+body*.28,width*1.75,slant*1.25,.12)
    polygon(max(4,start-trail*.68),start+body*.52,width*1.18,slant,.22)
    blur(start-8,154,width*1.75,slant*1.35,.52,2.8)
    polygon(start,154,width,slant,.98)
    polygon(start+body*.20,151,width*.24,max(1,slant*.32),.82,(244,253,255))
    binary.append(0);svg.append('</svg>')
    return bytes(binary),'\n'.join(svg)+'\n',count*4,count

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--colors',type=int,default=32)
    parser.add_argument('--epsilon',type=float,default=.45);parser.add_argument('--only',nargs='*')
    opt=parser.parse_args(); out=ROOT/'assets/vector/components';out.mkdir(parents=True,exist_ok=True)
    pack=bytearray();meta={};report={}
    for name in opt.only or NAMES:
        images,source=frames(name);entries=[];counts=[]
        for i,im in enumerate(images):
            pose=i-8 if name=='action-hands' and 9<=i<=11 else None
            commands,svg,vertices,regions=analytic_streak(i) if name=='result-streaks' else vectorize(im,opt.colors,opt.epsilon,pose)
            (out/f'{name}-{i+1:02}.svg').write_text(svg)
            compressed=struct.pack('<I',len(commands))+zlib.compress(commands,9)
            entries.append([len(pack),len(compressed)]);pack.extend(compressed)
            counts.append({'vertices':vertices,'colors':regions,'bytes':len(commands)})
        meta[name]={'width':images[0].width,'height':images[0].height,'frames':entries,'sheet':name in SHEETS}
        report[name]={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'source_bytes':source.stat().st_size,'frames':counts}
        print(name,len(images),'frames',sum(c['bytes'] for c in counts),'path bytes',flush=True)
    (out/'components.h2vp').write_bytes(pack)
    lines=['-- Generated vector directory: byte ranges address H2VG path streams, never pixels.','return {']
    for name,m in meta.items():
        entries=','.join('{%d,%d}'%tuple(v) for v in m['frames'])
        lines.append('["%s"]={w=%d,h=%d,sheet=%s,frames={%s}},'%(name,m['width'],m['height'],str(m['sheet']).lower(),entries))
    lines.append('}')
    (ROOT/'data/skills/qi-duel/scripts/component_paths.lua').write_text('\n'.join(lines)+'\n')
    report={'method':'palette-region contour geometry with RDP simplification; no embedded raster data','palette':opt.colors,'simplification_px':opt.epsilon,'pack_bytes':len(pack),'components':report}
    (out/'generation.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
