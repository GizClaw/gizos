#!/usr/bin/env python3
"""Bake static arena-lamp coverage from the unchanged, approved Canvas code.

Open http://127.0.0.1:4175/bake. Isolates original draw calls and overrides only
their gain; projection, paths, colors and styles stay verbatim. No scene movie.
Two cap variants preserve the original wheel-only and full-scene Canvas state.
"""
import base64
import http.server
import json
from pathlib import Path
import struct
import zlib

APP = Path(__file__).resolve().parents[2]
QA = APP / 'desktop-preview/comparison'
OUT = APP / 'assets/generated'
TIMES = (0, 1875, 4000, 9000, 17000)

BAKE = r"""
(async () => {
  const keys=['base'];
  for(let ring=0;ring<7;ring++) for(let s=0;s<16+ring*2;s++)
    if(hash01(ring*401+s*97)>=.45)keys.push('ring-'+ring+'-'+s);
  for(let i=0;i<32;i++)if(hash01(i*173)>=.42)keys.push('radial-'+i);
  for(let i=0;i<12;i++)keys.push('rect-'+i);
  const encode = rgb => {
    let binary='';
    for(let i=0;i<rgb.length;i+=4096)binary+=String.fromCharCode(...rgb.subarray(i,i+4096));
    return btoa(binary);
  };
  const rgbAt=(x,y,w,h)=>{
    const rgba=ctx.getImageData(x,y,w,h).data,rgb=new Uint8Array(w*h*3);
    for(let p=0;p<w*h;p++)rgb.set(rgba.subarray(p*4,p*4+3),p*3);
    return rgb;
  };
  for(const cap of ['butt','square']){
    const lamps=[];
    for(const key of keys){
      bakeArenaLamp(key,1,cap);
      const all=rgbAt(0,0,W,H);
      let left=W,top=H,right=0,bottom=0;
      for(let y=0;y<H;y++)for(let x=0;x<W;x++){
        const p=(y*W+x)*3;
        if(all[p]||all[p+1]||all[p+2]){
          left=Math.min(left,x);right=Math.max(right,x);
          top=Math.min(top,y);bottom=Math.max(bottom,y);
        }
      }
      if(left===W)throw new Error('Empty lamp '+key);
      left=Math.max(0,left-2);top=Math.max(0,top-2);
      const w=Math.min(W-1,right+2)-left+1,h=Math.min(H-1,bottom+2)-top+1;
      const levels=[];
      for(let level=0;level<=64;level++){
        bakeArenaLamp(key,level/64,cap);
        levels.push(encode(rgbAt(left,top,w,h)));
      }
      lamps.push({key,x:left,y:top,w,h,levels});
    }
    const response=await fetch('/atlas/'+cap,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(lamps)});
    if(!response.ok)throw new Error(await response.text());
  }
  for(const time of [0,1875,4000,9000,17000]){
    // The isolated preview starts with default butt caps. The full scene sets
    // square caps in its preceding wall pass; bake both instead of changing it.
    ctx.lineCap='butt';ctx.lineJoin='miter';
    drawArenaStructure(time,'wheel');
    let response=await fetch('/reference/'+time,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:rgbAt(0,0,W,H)});
    if(!response.ok)throw new Error(await response.text());
    ctx.lineCap='butt';ctx.lineJoin='miter';
    drawArenaStructure(time,'all');
    response=await fetch('/combined/'+time,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:rgbAt(0,0,W,H)});
    if(!response.ok)throw new Error(await response.text());
  }
  document.querySelector('#current-layer').textContent=`Saved ${keys.length} arena lamps, two cap variants, ten reference frames`;
})().catch(e=>document.querySelector('#current-layer').textContent=String(e));
"""


def isolated_function(script):
    original = script[script.index('    function drawArenaStructure('):
                      script.index('    function drawArena(images,')]
    result = original.replace("function drawArenaStructure(time, pass = 'all') {",
                              "function bakeArenaLamp(wheelKey, gain, cap) {\n"
                              "const time=0,pass='wheel';ctx.lineCap=cap;ctx.lineJoin='miter';")
    result = result.replace("ctx.fillStyle = 'rgba(0,12,25,.64)';",
                            "if(wheelKey==='base' && gain>0){\nctx.fillStyle = 'rgba(0,12,25,.64)';")
    result = result.replace('const wheelRings =', '}\nconst wheelRings =')
    result = result.replace('const end = start + slotAngle * span;',
                            "const end = start + slotAngle * span;\nif(wheelKey!=='ring-'+ring+'-'+s)continue;")
    result = result.replace('const a = i / 32 * TAU;',
                            "if(wheelKey!=='radial-'+i)continue;\nconst a = i / 32 * TAU;")
    result = result.replace('const near = .16 + .84 * depth;',
                            'brightness=gain;\nconst near = .16 + .84 * depth;')
    result = result.replace('const brightness = Math.pow(pulse, 5);',
                            "if(wheelKey!=='rect-'+i)return;\nconst brightness=gain;")
    assert result != original and result.count('brightness=gain;') == 3
    return result


def pack_atlas(lamps):
    assert 1 <= len(lamps) <= 256
    header = bytearray(b'H2LF' + struct.pack('<HHHH',368,448,len(lamps),65))
    payload, offsets = bytearray(), {}
    offset = 12 + len(lamps)*(8+65*8)
    for lamp in lamps:
        header.extend(struct.pack('<HHHH',lamp['x'],lamp['y'],lamp['w'],lamp['h']))
        assert len(lamp['levels']) == 65
        for level in lamp['levels']:
            raw = base64.b64decode(level, validate=True)
            assert len(raw) == lamp['w']*lamp['h']*3
            packed = zlib.compress(raw,9)
            if packed not in offsets:
                offsets[packed] = offset
                offset += len(packed)
                payload.extend(packed)
            header.extend(struct.pack('<II',offsets[packed],len(packed)))
    return header + payload


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/bake':
            self.send_error(404)
            return
        source = (APP/'desktop-preview/index.html').read_text()
        script = source.split('<script>',1)[1].split('    Promise.all(',1)[0]
        page = ('<!doctype html><meta charset="utf-8"><canvas id="game" width="368" '
                'height="448"></canvas><div id="layers"></div><p id="current-layer">Baking…</p>'
                '<script>'+script+isolated_function(script)+BAKE+'</script>').encode()
        self.send_response(200)
        self.send_header('Content-Type','text/html; charset=utf-8')
        self.end_headers()
        self.wfile.write(page)

    def do_POST(self):
        if self.headers.get('Origin') != 'http://127.0.0.1:4175':
            self.send_error(403)
            return
        count = int(self.headers.get('Content-Length','0'))
        if not 0 < count < 100_000_000:
            self.send_error(400)
            return
        data = self.rfile.read(count)
        if self.path in ('/atlas/butt','/atlas/square'):
            lamps = json.loads(data)
            OUT.mkdir(parents=True,exist_ok=True)
            name = 'arena-lights-'+self.path.rsplit('/',1)[1]+'.h2lf'
            (OUT/name).write_bytes(pack_atlas(lamps))
            (OUT/'arena-light-order.json').write_text(json.dumps([lamp['key'] for lamp in lamps],indent=2)+'\n')
        elif self.path in {f'/{mode}/{t}' for mode in ('reference','combined') for t in TIMES}:
            assert len(data) == 368*448*3
            QA.mkdir(parents=True,exist_ok=True)
            layer = 'wheel' if self.path.startswith('/reference/') else 'arena'
            (QA/f'browser-{layer}-{self.path.rsplit("/",1)[1]}.ppm').write_bytes(b'P6\n368 448\n255\n'+data)
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'OK')


if __name__ == '__main__':
    http.server.HTTPServer(('127.0.0.1',4175),Handler).serve_forever()
