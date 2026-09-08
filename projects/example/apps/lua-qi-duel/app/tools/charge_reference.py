#!/usr/bin/env python3
"""Original 10–11 static sprites and charge transition references (4179)."""
import http.server
from pathlib import Path
import struct
import zlib

APP=Path(__file__).resolve().parents[2]
QA=APP/'desktop-preview/comparison'
TIMES=(0,65,130,260,390,520)
CASES=('carousel-frame','charge-cells','charge-up','charge-down','charge-base','scene11')
ART={'carousel-base':(380,152),'charge-cells':(55,3150)}
BAKE=r"""
(async()=>{
 const images={mock:await load('/assets/source/approved-mockup.png'),opponent:await load('/assets/source/opponent-full.png'),
 left:await load('/assets/source/player-hand-left.png'),right:await load('/assets/source/player-hand-right.png'),
 carouselBase:await load('/assets/source/carousel-base-v1.png'),chargeMeter:await load('/assets/source/charge-meter-v1.png')};
 images.leftFaded=makeArmEndFade(images.left,'left');images.rightFaded=makeArmEndFade(images.right,'right');
 const base=document.createElement('canvas');base.width=380;base.height=152;
 const b=base.getContext('2d');b.globalAlpha=.9;b.drawImage(images.carouselBase,0,0,380,152);
 const cells=document.createElement('canvas');cells.width=55;cells.height=3150;
 const g=cells.getContext('2d');
 for(let lit=0;lit<=1;lit++)for(let i=0;i<10;i++){
   ctx.clearRect(0,0,W,H);drawChargeCell(images.chargeMeter,i,!!lit);
   const p=chargeCellPoint(i),x=Math.floor(p.x)-27,y=Math.floor(p.y)-22;
   g.drawImage(frameCanvas,x,y,55,45,0,(lit*10+i)*45,55,45);
 }
 // Keep the five source-over operations separate for animated opacity. Fading
 // one pre-flattened sprite is not equivalent to the original per-pass alpha.
 const fill=ctx.fill.bind(ctx),stroke=ctx.stroke.bind(ctx),drawImage=ctx.drawImage.bind(ctx),fillRect=ctx.fillRect.bind(ctx);
 for(let part=0;part<5;part++)for(let i=0;i<10;i++){
   let strokeIndex=0;
   ctx.fill=(...args)=>{if(part===0)fill(...args);};
   ctx.drawImage=(...args)=>{if(part===1)drawImage(...args);};
   ctx.fillRect=(...args)=>{if(part===2)fillRect(...args);};
   ctx.stroke=(...args)=>{if(part===3+strokeIndex)stroke(...args);strokeIndex++;};
   ctx.clearRect(0,0,W,H);drawChargeCell(images.chargeMeter,i,true);
   const p=chargeCellPoint(i),x=Math.floor(p.x)-27,y=Math.floor(p.y)-22;
   g.drawImage(frameCanvas,x,y,55,45,0,(20+part*10+i)*45,55,45);
 }
 ctx.fill=fill;ctx.stroke=stroke;ctx.drawImage=drawImage;ctx.fillRect=fillRect;
 for(const [name,art] of [['carousel-base',base],['charge-cells',cells]]){
   const response=await fetch('/asset/'+name,{method:'POST',body:art.getContext('2d').getImageData(0,0,art.width,art.height).data});
   if(!response.ok)throw new Error('asset export failed');
 }
 for(const mode of ['carousel-frame','charge-cells','charge-up','charge-down','charge-base','scene11'])for(const age of [0,65,130,260,390,520]){
   ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);ctx.lineCap='butt';ctx.lineJoin='miter';
   previewMeters.charge=3;meterFx.charge=null;
   if(mode==='charge-up'||mode==='charge-down'){
     const up=mode==='charge-up';previewMeters.charge=up?4:2;
     meterFx.charge={index:up?3:2,direction:up?1:-1,start:0};
   }
   if(mode==='scene11'){drawArena(images,age);drawOpponent(images.opponent,age);drawHands(images,age);drawHud(images.mock,age);}
   if(['carousel-frame','charge-base','scene11'].includes(mode))drawCarousel(images,age,'frame');
   if(mode!=='carousel-frame')drawChargeState(images.chargeMeter,age);
   const rgba=ctx.getImageData(0,0,W,H).data,rgb=new Uint8Array(W*H*3);
   for(let p=0;p<W*H;p++)rgb.set(rgba.subarray(p*4,p*4+3),p*3);
   const response=await fetch('/reference/'+mode+'/'+age,{method:'POST',body:rgb});
   if(!response.ok)throw new Error('reference export failed');
 }
 document.querySelector('#current-layer').textContent='Saved transparent base, 20 static cells, 50 alpha components and 36 reference frames';
})().catch(e=>document.querySelector('#current-layer').textContent=String(e));
"""

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        names=('approved-mockup','opponent-full','player-hand-left','player-hand-right','carousel-base-v1','charge-meter-v1')
        if self.path in {f'/assets/source/{n}.png' for n in names}:
            data=(APP/self.path.lstrip('/')).read_bytes()
            self.send_response(200);self.send_header('Content-Type','image/png');self.end_headers();self.wfile.write(data);return
        if self.path!='/bake':self.send_error(404);return
        source=(APP/'desktop-preview/index.html').read_text()
        script=source.split('<script>',1)[1].split('    Promise.all(',1)[0]
        script=script.replace("frameCanvas.getContext('2d', { alpha: false })","frameCanvas.getContext('2d', { alpha: true, willReadFrequently: false })")
        page=('<meta charset="utf-8"><canvas id="game" width="368" height="448"></canvas>'
              '<div id="layers"></div><p id="current-layer">Capturing…</p><script>'+script+BAKE+'</script>').encode()
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.end_headers();self.wfile.write(page)

    def do_POST(self):
        if self.headers.get('Origin')!='http://127.0.0.1:4179':self.send_error(403);return
        refs={f'/reference/{case}/{age}' for case in CASES for age in TIMES}
        assets={f'/asset/{name}' for name in ART}
        if self.path in assets:
            name=self.path.split('/')[-1];w,h=ART[name];expected=w*h*4
        elif self.path in refs:expected=368*448*3
        else:self.send_error(404);return
        if self.headers.get('Content-Length')!=str(expected):self.send_error(400);return
        data=self.rfile.read(expected)
        if len(data)!=expected:self.send_error(400);return
        if self.path in assets:
            (APP/f'assets/generated/{name}.h2r8').write_bytes(b'H2R8'+struct.pack('<HH',w,h)+zlib.compress(data,9))
        else:
            _,_,case,age=self.path.split('/')
            (QA/f'browser-{case}-{age}.ppm').write_bytes(b'P6\n368 448\n255\n'+data)
        self.send_response(200);self.end_headers();self.wfile.write(b'OK')

if __name__=='__main__':http.server.HTTPServer(('127.0.0.1',4179),Handler).serve_forever()
