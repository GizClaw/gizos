#!/usr/bin/env python3
"""Compile the authored SVG subset to bounded H2VG geometric commands (no pixels)."""
import argparse
import math
import re
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

TOK = re.compile(r'[a-zA-Z]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?')

def color(value):
    value=value.lstrip('#')
    if len(value)==3: value=''.join(c*2 for c in value)
    if len(value)!=6: raise ValueError('only RGB hex colors supported')
    return bytes.fromhex(value)

def compile_svg(source):
    root=ET.fromstring(source)
    defs={e.get('id'):e for e in root.iter() if e.get('id')}
    gradients=[e for e in root.iter() if e.tag.split('}')[-1] in ('linearGradient','radialGradient')]
    if len(gradients)>16: raise ValueError('too many gradients')
    ids={e.get('id'):i for i,e in enumerate(gradients)}
    view=list(map(float,root.get('viewBox').split()))
    if view[:2]!=[0,0] or any(v<=0 or v>4096 or v!=int(v) for v in view[2:]): raise ValueError('invalid viewbox')
    data=bytearray(b'H2VG'+struct.pack('<HHHH',int(view[2]),int(view[3]),len(gradients),1))
    for g in gradients:
        radial=g.tag.endswith('radialGradient')
        stops=list(g)
        if not 1<=len(stops)<=16: raise ValueError('invalid stops')
        coords=[float(g.get(n,str(d))) for n,d in (('cx',.5),('cy',.5),('r',.5),('unused',0))] if radial else [float(g.get(n,str(d))) for n,d in (('x1',0),('y1',0),('x2',1),('y2',0))]
        data.extend(struct.pack('<BB4f',int(radial),len(stops),*coords))
        for stop in stops:
            data.extend(struct.pack('<f',float(stop.get('offset','0')))+color(stop.get('stop-color'))+bytes([round(float(stop.get('stop-opacity','1'))*255)]))
    def op(code,*values):
        if any(not math.isfinite(v) or abs(v)>1e6 for v in values): raise ValueError('unbounded coordinate')
        data.append(code)
        data.extend(struct.pack('<'+'f'*len(values),*values))
    def arc(x,y,rx,ry,rotation,large,sweep,ex,ey):
        rx,ry=abs(rx),abs(ry)
        if not rx or not ry: op(6,ex,ey);return
        if x==ex and y==ey:return
        phi=math.radians(rotation);co,si=math.cos(phi),math.sin(phi)
        dx,dy=(x-ex)/2,(y-ey)/2;xp,yp=co*dx+si*dy,-si*dx+co*dy
        gain=xp*xp/(rx*rx)+yp*yp/(ry*ry)
        if gain>1:rx*=math.sqrt(gain);ry*=math.sqrt(gain)
        coeff=math.sqrt(max(0,(rx*rx*ry*ry-rx*rx*yp*yp-ry*ry*xp*xp)/(rx*rx*yp*yp+ry*ry*xp*xp)))
        if bool(large)==bool(sweep):coeff=-coeff
        cxp,cyp=coeff*rx*yp/ry,-coeff*ry*xp/rx
        cx,cy=co*cxp-si*cyp+(x+ex)/2,si*cxp+co*cyp+(y+ey)/2
        t=math.atan2((yp-cyp)/ry,(xp-cxp)/rx)
        end=math.atan2((-yp-cyp)/ry,(-xp-cxp)/rx);delta=(end-t)%(2*math.pi)
        if not sweep:delta-=2*math.pi
        count=max(1,math.ceil(abs(delta)/(math.pi/2)));step=delta/count
        def point(theta):return (cx+co*rx*math.cos(theta)-si*ry*math.sin(theta),cy+si*rx*math.cos(theta)+co*ry*math.sin(theta))
        def derivative(theta):return (-co*rx*math.sin(theta)-si*ry*math.cos(theta),-si*rx*math.sin(theta)+co*ry*math.cos(theta))
        for _ in range(count):
            a,b=point(t),point(t+step);da,db=derivative(t),derivative(t+step);k=4/3*math.tan(step/4)
            op(7,a[0]+k*da[0],a[1]+k*da[1],b[0]-k*db[0],b[1]-k*db[1],*b);t+=step
    def path(d):
        tokens=TOK.findall(d);i=0;cmd=None;x=y=sx=sy=0
        sizes={'M':2,'L':2,'H':1,'V':1,'C':6,'Q':4,'A':7}
        while i<len(tokens):
            if tokens[i].isalpha():cmd=tokens[i];i+=1
            if cmd in ('Z','z'):op(8);x,y=sx,sy;cmd=None;continue
            if cmd is None or cmd.upper() not in sizes:raise ValueError('unsupported path command')
            k=cmd.upper();n=sizes[k];v=list(map(float,tokens[i:i+n]));i+=n
            if len(v)!=n:raise ValueError('incomplete path')
            relative=cmd.islower()
            if k in ('M','L'):
                ex,ey=v;ex+=x if relative else 0;ey+=y if relative else 0
                op(5 if k=='M' else 6,ex,ey);x,y=ex,ey
                if k=='M':sx,sy=x,y;cmd='l' if relative else 'L'
            elif k=='H':x=v[0]+(x if relative else 0);op(6,x,y)
            elif k=='V':y=v[0]+(y if relative else 0);op(6,x,y)
            elif k in ('C','Q'):
                if relative:v=[a+(x if j%2==0 else y) for j,a in enumerate(v)]
                if k=='Q':qx,qy,ex,ey=v;v=[x+2/3*(qx-x),y+2/3*(qy-y),ex+2/3*(qx-ex),ey+2/3*(qy-ey),ex,ey]
                op(7,*v);x,y=v[-2:]
            else:
                rx,ry,rot,large,sweep,ex,ey=v;ex+=x if relative else 0;ey+=y if relative else 0
                arc(x,y,rx,ry,rot,large,sweep,ex,ey);x,y=ex,ey
    def transform(text):
        for name,args in re.findall(r'(\w+)\(([^)]*)\)',text):
            v=[float(t) for t in re.split(r'[ ,]+',args.strip())]
            if name=='translate':op(3,1,0,0,1,v[0],v[1] if len(v)>1 else 0)
            elif name=='rotate':
                angle=math.radians(v[0]);c,s=math.cos(angle),math.sin(angle);cx,cy=v[1:] if len(v)==3 else (0,0)
                op(3,c,s,-s,c,cx-c*cx+s*cy,cy-s*cx-c*cy)
            else:raise ValueError('unsupported transform')
    def geometry(e):
        op(4)
        if e.tag.endswith('path'):path(e.get('d'))
        elif e.tag.endswith('ellipse'):
            x,y,rx,ry=[float(e.get(k)) for k in ('cx','cy','rx','ry')];op(9,x-rx,y-ry,rx*2,ry*2)
        else:raise ValueError('unsupported geometry')
    def paint(value,stroke,width,opacity):
        if value=='none':return
        if value.startswith('url(#'):brush=ids[value[5:-1]];rgba=b'\0'*4
        else:brush=-1;rgba=color(value)+b'\xff'
        data.extend(bytes([10,int(stroke)])+struct.pack('<h',brush)+rgba+struct.pack('<ff',width,opacity))
    def visit(e):
        kind=e.tag.split('}')[-1]
        if kind in ('defs','title'):return
        op(1)
        if e.get('transform'):transform(e.get('transform'))
        if e.get('clip-path'):
            clip=defs[e.get('clip-path')[5:-1]]
            if len(clip)!=1:raise ValueError('unsupported clip')
            geometry(clip[0]);op(11)
        if kind in ('svg','g'):
            for child in e:visit(child)
        elif kind in ('path','ellipse'):
            geometry(e);opacity=float(e.get('opacity','1'));width=float(e.get('stroke-width','1'))
            paint(e.get('fill','#000'),False,width,opacity);paint(e.get('stroke','none'),True,width,opacity)
        else:raise ValueError('unsupported element '+kind)
        op(2)
    visit(root);op(0)
    return bytes(data)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.write_bytes(compile_svg(args.source.read_text()))
