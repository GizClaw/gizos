#!/usr/bin/env python3
"""Export original 03–07 references and lossless RGBA art; localhost:4177/bake."""
import http.server
from pathlib import Path
import struct
import zlib

APP=Path(__file__).resolve().parents[2]
QA=APP/'desktop-preview/comparison'
OUT=APP/'assets/generated'
TIMES=(0,1875,4000,9000,17000)
LAYERS=('dust','particles','opponent','hand-left','hand-right','scene7','arena-dust')
ART={'opponent':(1224,1285),'hand-left':(145,177),'hand-right':(145,177)}
BAKE=r"""
(async()=>{
  const images={opponent:await load('/assets/source/opponent-full.png'),
    left:await load('/assets/source/player-hand-left.png'),right:await load('/assets/source/player-hand-right.png')};
  images.leftFaded=makeArmEndFade(images.left,'left');images.rightFaded=makeArmEndFade(images.right,'right');
  for(const [name,source] of [['opponent',images.opponent],['hand-left',images.leftFaded],['hand-right',images.rightFaded]]){
    const art=document.createElement('canvas');art.width=source.width;art.height=source.height;
    const g=art.getContext('2d');g.drawImage(source,0,0);
    const response=await fetch('/asset/'+name,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:g.getImageData(0,0,art.width,art.height).data});
    if(!response.ok)throw new Error(await response.text());
  }
  for(const layer of ['dust','particles','opponent','hand-left','hand-right','scene7','arena-dust'])
  for(const time of [0,1875,4000,9000,17000]){
    ctx.fillStyle='#000';ctx.fillRect(0,0,W,H);ctx.lineCap='butt';ctx.lineJoin='miter';
    if(layer==='dust'||layer==='particles')drawArena(images,time,layer);
    else if(layer==='opponent')drawOpponent(images.opponent,time);
    else if(layer==='hand-left')drawHands(images,time,'left');
    else if(layer==='hand-right')drawHands(images,time,'right');
    else if(layer==='arena-dust')drawArena123(images,time);
    else{drawArena(images,time);drawOpponent(images.opponent,time);drawHands(images,time);}
    const rgba=ctx.getImageData(0,0,W,H).data,rgb=new Uint8Array(W*H*3);
    for(let p=0;p<W*H;p++)rgb.set(rgba.subarray(p*4,p*4+3),p*3);
    const response=await fetch('/reference/'+layer+'/'+time,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:rgb});
    if(!response.ok)throw new Error(await response.text());
  }
  document.querySelector('#current-layer').textContent='Saved three lossless RGBA assets and 35 reference frames (03–07 + combined)';
})().catch(e=>document.querySelector('#current-layer').textContent=String(e));
"""

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        allowed={f'/assets/source/{name}.png' for name in ('opponent-full','player-hand-left','player-hand-right')}
        if self.path in allowed:
            data=(APP/self.path.lstrip('/')).read_bytes()
            self.send_response(200);self.send_header('Content-Type','image/png');self.end_headers();self.wfile.write(data)
            return
        if self.path!='/bake':self.send_error(404);return
        source=(APP/'desktop-preview/index.html').read_text()
        script=source.split('<script>',1)[1].split('    Promise.all(',1)[0]
        combined=script[script.index('    function drawArena(images,'):script.index('    function drawOpponent(')]
        combined=combined.replace('function drawArena(','function drawArena123(',1)
        combined=combined.replace("const drawFloorParticles = pass === 'all' || pass === 'particles';",'const drawFloorParticles = false;')
        combined=combined.replace("const drawWallParticles = pass === 'all' || pass === 'particles';",'const drawWallParticles = false;')
        page=('<!doctype html><meta charset="utf-8"><canvas id="game" width="368" height="448"></canvas>'
              '<div id="layers"></div><p id="current-layer">Capturing…</p><script>'+script+combined+BAKE+'</script>').encode()
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.end_headers();self.wfile.write(page)

    def do_POST(self):
        if self.headers.get('Origin')!='http://127.0.0.1:4177':self.send_error(403);return
        references={f'/reference/{layer}/{time}' for layer in LAYERS for time in TIMES}
        if self.path in references:expected=368*448*3
        elif self.path in {f'/asset/{name}' for name in ART}:
            name=self.path.split('/')[-1];w,h=ART[name];expected=w*h*4
        else:self.send_error(404);return
        if self.headers.get('Content-Length')!=str(expected):self.send_error(400);return
        data=self.rfile.read(expected)
        assert len(data)==expected
        if self.path in references:
            _,_,layer,time=self.path.split('/')
            QA.mkdir(parents=True,exist_ok=True)
            (QA/f'browser-{layer}-{time}.ppm').write_bytes(b'P6\n368 448\n255\n'+data)
        else:
            OUT.mkdir(parents=True,exist_ok=True)
            (OUT/f'{name}.h2r8').write_bytes(b'H2R8'+struct.pack('<HH',w,h)+zlib.compress(data,9))
        self.send_response(200);self.end_headers();self.wfile.write(b'OK')

if __name__=='__main__':http.server.HTTPServer(('127.0.0.1',4177),Handler).serve_forever()
