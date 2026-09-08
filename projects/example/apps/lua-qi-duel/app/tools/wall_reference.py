#!/usr/bin/env python3
"""Bake lamp rasterization (not animation) from the approved Canvas renderer.

Run this server, then open /bake on localhost:4174. All animation timing stays
in Lua; the atlas records subpixel coverage and Canvas neon glow at 65 gains.
The server accepts only named local build artifacts, never arbitrary paths.
"""
import base64
import http.server
import json
from pathlib import Path
import struct
import zlib

APP = Path(__file__).resolve().parents[2]
OUT = APP / "assets" / "generated"
QA = APP / "desktop-preview" / "comparison"

BAKE = r"""
(async () => {
  const lamps = [];
  for (const lamp of wallLights) {
    const perspective = .25 + lamp.depth * .75;
    const x = 184 + lamp.side * (6 + lamp.depth * 181);
    const y = 164 - lamp.depth * 124 + (6 + lamp.depth * 168) * lamp.lift;
    const length = (3.1 + perspective * 7.1) * lamp.length;
    const width = .38 + perspective * .78;
    const left = Math.max(0, Math.floor(x - 24));
    const top = Math.max(0, Math.floor(y - length / 2 - 24));
    const w = Math.min(368 - left, Math.ceil(x + 24) - left);
    const h = Math.min(448 - top, Math.ceil(y + length / 2 + 24) - top);
    const levels = [];
    for (let level = 0; level <= 64; level++) {
      ctx.clearRect(0,0,W,H); ctx.fillStyle = '#000'; ctx.fillRect(0,0,W,H);
      ctx.save(); ctx.globalCompositeOperation = 'lighter';
      drawWallNeonSegment(x,y-length/2,x,y+length/2,width,lamp.hue,level/64);
      ctx.restore();
      const rgba = ctx.getImageData(left,top,w,h).data;
      const rgb = new Uint8Array(w*h*3);
      for(let p=0;p<w*h;p++) rgb.set(rgba.subarray(p*4,p*4+3),p*3);
      levels.push(btoa(String.fromCharCode(...rgb)));
    }
    lamps.push({x:left,y:top,w,h,levels});
  }
  let response = await fetch('/atlas', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(lamps)});
  if(!response.ok) throw new Error(await response.text());
  for(const time of [0,1875,4000,9000,17000]) {
    drawArenaStructure(time,'walls');
    const rgba = ctx.getImageData(0,0,W,H).data;
    const rgb = new Uint8Array(W*H*3);
    for(let p=0;p<W*H;p++) rgb.set(rgba.subarray(p*4,p*4+3),p*3);
    response = await fetch('/reference/'+time, {method:'POST',headers:{'Content-Type':'application/octet-stream'},body:rgb});
    if(!response.ok) throw new Error(await response.text());
  }
  document.querySelector('#current-layer').textContent='Atlas and five reference frames saved';
})().catch(e => {document.querySelector('#current-layer').textContent=String(e);});
"""

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/bake":
            self.send_error(404)
            return
        source = (APP / "desktop-preview/index.html").read_text()
        script = source.split("<script>", 1)[1].split("    Promise.all(", 1)[0]
        page = ('<!doctype html><meta charset="utf-8"><canvas id="game" width="368" '
                'height="448"></canvas><div id="layers"></div><p id="current-layer">Baking…</p>'
                '<script>' + script + BAKE + '</script>').encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(page)

    def do_POST(self):
        if self.headers.get('Origin') != 'http://127.0.0.1:4174':
            self.send_error(403)
            return
        count = int(self.headers.get("Content-Length", "0"))
        if not 0 < count < 100_000_000:
            self.send_error(400)
            return
        data = self.rfile.read(count)
        if self.path == "/atlas":
            lamps = json.loads(data)
            assert len(lamps) == 72
            header = bytearray(b"H2LF" + struct.pack("<HHHH",368,448,72,65))
            payload = bytearray()
            offset = 12 + 72 * (8 + 65 * 8)
            for lamp in lamps:
                header.extend(struct.pack("<HHHH",lamp['x'],lamp['y'],lamp['w'],lamp['h']))
                assert len(lamp['levels']) == 65
                for level in lamp['levels']:
                    raw = base64.b64decode(level)
                    assert len(raw) == lamp['w'] * lamp['h'] * 3
                    packed = zlib.compress(raw, 9)
                    header.extend(struct.pack("<II",offset,len(packed)))
                    payload.extend(packed)
                    offset += len(packed)
            OUT.mkdir(parents=True,exist_ok=True)
            (OUT / "wall-lights.h2lf").write_bytes(header + payload)
        elif self.path in {f'/reference/{t}' for t in (0,1875,4000,9000,17000)}:
            assert len(data) == 368*448*3
            QA.mkdir(parents=True,exist_ok=True)
            (QA / ('browser-walls-'+self.path.rsplit('/',1)[1]+'.ppm')).write_bytes(
                b'P6\n368 448\n255\n'+data)
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"OK")

if __name__ == '__main__':
    http.server.HTTPServer(('127.0.0.1',4174),Handler).serve_forever()
