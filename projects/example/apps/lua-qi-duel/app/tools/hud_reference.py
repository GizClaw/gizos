#!/usr/bin/env python3
"""Export static HUD state art and original-reference health transitions (4178)."""
import http.server
from pathlib import Path
import struct
import zlib

APP = Path(__file__).resolve().parents[2]
QA = APP / 'desktop-preview/comparison'
TIMES = (0, 50, 120, 240, 360, 480)
CASES = ('hud', 'scene8', 'player-down', 'player-up', 'enemy-down', 'enemy-up')
BAKE = r"""
(async()=>{
 const images={mock:await load('/assets/source/approved-mockup.png'),opponent:await load('/assets/source/opponent-full.png'),
 left:await load('/assets/source/player-hand-left.png'),right:await load('/assets/source/player-hand-right.png')};
 images.leftFaded=makeArmEndFade(images.left,'left');images.rightFaded=makeArmEndFade(images.right,'right');
 const sheet=document.createElement('canvas');sheet.width=190;sheet.height=56*12;
 const g=sheet.getContext('2d');
 for(const [side,sx,row] of [['player',12,0],['enemy',726,6]])for(let hp=0;hp<=5;hp++){
   ctx.clearRect(0,0,W,H);previewMeters[side]=hp;
   const scale=images.mock.width/1136;
   ctx.drawImage(images.mock,sx*scale,24*scale,398*scale,116*scale,0,0,190,hudLayout.height);
   drawHealthState(side,0,0,0,0);
   g.drawImage(frameCanvas,0,0,190,56,0,(row+hp)*56,190,56);
 }
 let response=await fetch('/asset',{method:'POST',body:g.getImageData(0,0,190,672).data});
 if(!response.ok)throw new Error('asset export failed');
 for(const mode of ['hud','scene8','player-down','player-up','enemy-down','enemy-up'])for(const age of [0,50,120,240,360,480]){
   ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);ctx.lineCap='butt';ctx.lineJoin='miter';
   previewMeters.player=4;previewMeters.enemy=4;meterFx.player=null;meterFx.enemy=null;
   if(mode.includes('-')){
     const [side,action]=mode.split('-'),up=action==='up';previewMeters[side]=up?5:3;
     meterFx[side]={index:side==='player'?(up?4:3):(up?0:1),direction:up?1:-1,start:0};
   }
   if(mode==='scene8'){drawArena(images,age);drawOpponent(images.opponent,age);drawHands(images,age);}
   drawHud(images.mock,age);
   const rgba=ctx.getImageData(0,0,W,H).data,rgb=new Uint8Array(W*H*3);
   for(let p=0;p<W*H;p++)rgb.set(rgba.subarray(p*4,p*4+3),p*3);
   response=await fetch('/reference/'+mode+'/'+age,{method:'POST',body:rgb});
   if(!response.ok)throw new Error('reference export failed');
 }
 document.querySelector('#current-layer').textContent='Saved HUD state atlas and 36 original reference frames';
})().catch(e=>document.querySelector('#current-layer').textContent=String(e));
"""

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in {f'/assets/source/{n}.png' for n in
                         ('approved-mockup','opponent-full','player-hand-left','player-hand-right')}:
            data=(APP/self.path.lstrip('/')).read_bytes()
            self.send_response(200);self.send_header('Content-Type','image/png');self.end_headers();self.wfile.write(data)
            return
        if self.path!='/bake':self.send_error(404);return
        source=(APP/'desktop-preview/index.html').read_text()
        script=source.split('<script>',1)[1].split('    Promise.all(',1)[0]
        # Preserve transparent space outside the original panel rectangle.
        # Explicit readback policy prevents Chrome switching from GPU to CPU
        # halfway through the export and changing image minification filtering.
        script=script.replace("frameCanvas.getContext('2d', { alpha: false })", "frameCanvas.getContext('2d', { alpha: true, willReadFrequently: false })")
        page=('<meta charset="utf-8"><canvas id="game" width="368" height="448"></canvas>'
              '<div id="layers"></div><p id="current-layer">Capturing…</p><script>'+script+BAKE+'</script>').encode()
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.end_headers();self.wfile.write(page)

    def do_POST(self):
        if self.headers.get('Origin')!='http://127.0.0.1:4178':self.send_error(403);return
        references={f'/reference/{case}/{age}' for case in CASES for age in TIMES}
        if self.path=='/asset':expected=190*672*4
        elif self.path in references:expected=368*448*3
        else:self.send_error(404);return
        if self.headers.get('Content-Length')!=str(expected):self.send_error(400);return
        data=self.rfile.read(expected)
        if len(data)!=expected:self.send_error(400);return
        if self.path=='/asset':
            (APP/'assets/generated/hud-states.h2r8').write_bytes(b'H2R8'+struct.pack('<HH',190,672)+zlib.compress(data,9))
        else:
            _,_,case,age=self.path.split('/')
            (QA/f'browser-{case}-{age}.ppm').write_bytes(b'P6\n368 448\n255\n'+data)
        self.send_response(200);self.end_headers();self.wfile.write(b'OK')

if __name__=='__main__':http.server.HTTPServer(('127.0.0.1',4178),Handler).serve_forever()
