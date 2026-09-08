#!/usr/bin/env python3
"""Capture original 03 ring-dust and 01+02+03, without modifying the reference.

Run then open http://127.0.0.1:4176/bake. No raster assets are generated: native
Lua computes continuous particle motion and the native renderer antialiases it.
"""
import http.server
from pathlib import Path

APP = Path(__file__).resolve().parents[2]
QA = APP / 'desktop-preview/comparison'
TIMES = (0, 1875, 4000, 9000, 17000)

BAKE = r"""
(async()=>{
  for(const layer of ['dust','arena-dust'])for(const time of [0,1875,4000,9000,17000]){
    ctx.lineCap='butt';ctx.lineJoin='miter';
    if(layer==='dust')drawArena({},time,'dust');else drawArena123({},time);
    const rgba=ctx.getImageData(0,0,W,H).data,rgb=new Uint8Array(W*H*3);
    for(let p=0;p<W*H;p++)rgb.set(rgba.subarray(p*4,p*4+3),p*3);
    const response=await fetch('/reference/'+layer+'/'+time,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:rgb});
    if(!response.ok)throw new Error(await response.text());
  }
  document.querySelector('#current-layer').textContent='Saved 03 dust and 01+02+03: ten reference frames';
})().catch(e=>document.querySelector('#current-layer').textContent=String(e));
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/bake':
            self.send_error(404)
            return
        source=(APP/'desktop-preview/index.html').read_text()
        script=source.split('<script>',1)[1].split('    Promise.all(',1)[0]
        # A second copy only gates off element 04; all 01–03 code is untouched.
        combined=script[script.index('    function drawArena(images,'):script.index('    function drawOpponent(')]
        combined=combined.replace('function drawArena(', 'function drawArena123(',1)
        combined=combined.replace("const drawFloorParticles = pass === 'all' || pass === 'particles';",'const drawFloorParticles = false;')
        combined=combined.replace("const drawWallParticles = pass === 'all' || pass === 'particles';",'const drawWallParticles = false;')
        page=('<!doctype html><meta charset="utf-8"><canvas id="game" width="368" height="448"></canvas>'
              '<div id="layers"></div><p id="current-layer">Capturing…</p><script>'+script+combined+BAKE+'</script>').encode()
        self.send_response(200)
        self.send_header('Content-Type','text/html; charset=utf-8')
        self.end_headers()
        self.wfile.write(page)

    def do_POST(self):
        valid={f'/reference/{layer}/{t}' for layer in ('dust','arena-dust') for t in TIMES}
        if self.path not in valid or self.headers.get('Origin')!='http://127.0.0.1:4176':
            self.send_error(403)
            return
        if self.headers.get('Content-Length')!=str(368*448*3):
            self.send_error(400)
            return
        data=self.rfile.read(368*448*3)
        assert len(data)==368*448*3
        _,_,layer,time=self.path.split('/')
        QA.mkdir(parents=True,exist_ok=True)
        (QA/f'browser-{layer}-{time}.ppm').write_bytes(b'P6\n368 448\n255\n'+data)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'OK')


if __name__=='__main__':
    http.server.HTTPServer(('127.0.0.1',4176),Handler).serve_forever()
