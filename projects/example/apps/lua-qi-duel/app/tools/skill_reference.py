#!/usr/bin/env python3
"""Export approved icon focus styles, not animation frames; Lua owns motion."""
import http.server
from pathlib import Path
import struct
import zlib

APP = Path(__file__).resolve().parents[2]
QA = APP / 'desktop-preview/comparison'
PARTS = ('skill-charge', 'skill-wave', 'skill-absorb', 'skill-guard')
CASES = (*PARTS, 'carousel', 'carousel-wave', 'carousel-absorb', 'carousel-guard',
         'carousel-left', 'carousel-right', 'full')
TILES = []
BAKE = r"""
(async()=>{
 const images={mock:await load('/assets/source/approved-mockup.png'),opponent:await load('/assets/source/opponent-full.png'),
 left:await load('/assets/source/player-hand-left.png'),right:await load('/assets/source/player-hand-right.png'),
 carouselBase:await load('/assets/source/carousel-base-v1.png'),chargeMeter:await load('/assets/source/charge-meter-v1.png'),
 skills:await load('/assets/source/skill-icons-v1.png')};
 images.leftFaded=makeArmEndFade(images.left,'left');images.rightFaded=makeArmEndFade(images.right,'right');
 skillIconVariants=Array.from({length:4},(_,i)=>{const full=makeSpriteSkillIcon(images.skills,i);return{full,left:makeDirectionalFade(full,-1),right:makeDirectionalFade(full,1)};});
 const art=document.createElement('canvas');art.width=art.height=160;
 const g=art.getContext('2d',{willReadFrequently:false});
 const focuses=Array.from({length:33},(_,i)=>i/32);focuses.splice(17,0,.500001);
 let tile=0;
 for(let i=0;i<4;i++)for(const direction of ['full','left','right'])for(const focus of focuses){
   g.clearRect(0,0,160,160);g.save();g.globalAlpha=.34+focus*.66;
   if(focus>.5){g.shadowColor=`rgba(255,255,255,${.38+focus*.46})`;g.shadowBlur=9+focus*8;g.filter=`brightness(${1.05+focus*.32})`;}
   const size=52+focus*36;g.drawImage(skillIconVariants[i][direction],80-size/2,80-size/2,size,size);g.restore();
   if(!(await fetch('/tile/'+tile++,{method:'POST',body:g.getImageData(0,0,160,160).data})).ok)throw Error('tile export');
 }
 for(const mode of ['skill-charge','skill-wave','skill-absorb','skill-guard','carousel','carousel-wave','carousel-absorb','carousel-guard','carousel-left','carousel-right','full']){
   for(const time of [0,1875,4000,9000,17000]){
     selected=mode==='carousel-wave'?1:mode==='carousel-absorb'?2:mode==='carousel-guard'?3:0;
     dragX=mode==='carousel-left'?-50:mode==='carousel-right'?50:0;
     ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);ctx.lineCap='butt';ctx.lineJoin='miter';
     if(mode==='full'){drawArena(images,time);drawOpponent(images.opponent,time);drawHands(images,time);drawHud(images.mock,time);}
     drawCarousel(images,time,mode.startsWith('skill-')?mode:'all');
     const rgba=ctx.getImageData(0,0,W,H).data,rgb=new Uint8Array(W*H*3);
     for(let p=0;p<W*H;p++)rgb.set(rgba.subarray(p*4,p*4+3),p*3);
     if(!(await fetch('/reference/'+mode+'/'+time,{method:'POST',body:rgb})).ok)throw Error('reference export');
   }
 }
 document.querySelector('#current-layer').textContent='Saved 408 focus styles and 55 reference frames';
})().catch(e=>document.querySelector('#current-layer').textContent=String(e));
"""

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        names = ('approved-mockup','opponent-full','player-hand-left','player-hand-right',
                 'carousel-base-v1','charge-meter-v1','skill-icons-v1')
        if self.path in {f'/assets/source/{n}.png' for n in names}:
            data=(APP/self.path.lstrip('/')).read_bytes();kind='image/png'
        elif self.path == '/bake':
            source=(APP/'desktop-preview/index.html').read_text()
            script=source.split('<script>',1)[1].split('    Promise.all(',1)[0]
            script=script.replace("frameCanvas.getContext('2d', { alpha: false })","frameCanvas.getContext('2d', { alpha: true, willReadFrequently: false })")
            data=('<meta charset="utf-8"><canvas id="game" width="368" height="448"></canvas><div id="layers"></div><p id="current-layer">Capturing…</p><script>'+script+BAKE+'</script>').encode();kind='text/html; charset=utf-8'
            TILES.clear()
        else:self.send_error(404);return
        self.send_response(200);self.send_header('Content-Type',kind);self.end_headers();self.wfile.write(data)

    def do_POST(self):
        if self.headers.get('Origin')!='http://127.0.0.1:4180':self.send_error(403);return
        tile=self.path==f'/tile/{len(TILES)}' and len(TILES)<408
        refs={f'/reference/{c}/{t}' for c in CASES for t in (0,1875,4000,9000,17000)}
        if not tile and self.path not in refs:self.send_error(404);return
        expected=160*160*4 if tile else 368*448*3
        if self.headers.get('Content-Length')!=str(expected):self.send_error(400);return
        data=self.rfile.read(expected)
        if len(data)!=expected:self.send_error(400);return
        if tile:
            TILES.append(zlib.compress(data,9))
            if len(TILES)==408:
                offset=12+8*len(TILES);directory=b''
                for blob in TILES:
                    directory+=struct.pack('<II',offset,len(blob));offset+=len(blob)
                (APP/'assets/generated/skill-styles.h2rs').write_bytes(b'H2RS'+struct.pack('<HHHH',160,160,len(TILES),0)+directory+b''.join(TILES))
        else:
            _,_,case,time=self.path.split('/')
            (QA/f'browser-{case}-{time}.ppm').write_bytes(b'P6\n368 448\n255\n'+data)
        self.send_response(200);self.end_headers();self.wfile.write(b'OK')

if __name__=='__main__':http.server.HTTPServer(('127.0.0.1',4180),Handler).serve_forever()
