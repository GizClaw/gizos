"""Bake the original restaurant and character plates. The game ships the
indexed PNGs, so running or building it requires no art-authoring packages.
"""
import argparse
import io
from pathlib import Path
import subprocess
import tempfile

import cairosvg
from PIL import Image, PngImagePlugin

from diners import CAST_DEFS, actor
from shop import DEFS, crest, island, mat, path, room

HERE = Path(__file__).resolve().parent
NAMES = ('salmon', 'tamago', 'tuna', 'shrimp', 'maki', 'inari', 'empty')


def paint(body, box, size):
    x, y, w, h = box
    doc = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x} {y} {w} {h}" '
           f'width="{size[0] * 2}" height="{size[1] * 2}"><defs>{DEFS}{CAST_DEFS}</defs>{body}</svg>')
    png = cairosvg.svg2png(bytestring=doc.encode())
    return Image.open(io.BytesIO(png)).convert('RGBA').resize(size, Image.Resampling.LANCZOS)


def save(image, name, origin, out):
    # Build the colour palette from visible pigment. The 32 last entries are
    # a half-coverage edge palette. This keeps transparent sprites from using
    # most of a tiny hardware palette on invisible RGB neighbours.
    image = image.convert('RGBA')
    rgb, alpha = image.convert('RGB'), image.getchannel('A')
    main = rgb.quantize(224, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    edge = rgb.quantize(30, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    data = bytes(255 if a < 52 else 224 + e if a < 195 else p
                 for a, p, e in zip(alpha.tobytes(), main.tobytes(), edge.tobytes()))
    pal = Image.frombytes('P', image.size, data)
    pal.putpalette((main.getpalette()+[0]*672)[:672] + (edge.getpalette()+[0]*90)[:90] + [0]*6)
    info = PngImagePlugin.PngInfo()
    info.add_text('Source', 'Original Tidewheel artwork. ' + origin)
    pal.save(out / f'{name}.png', bits=8, transparency=bytes([255]*224+[128]*30+[0,0]), pnginfo=info, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=HERE.parent / 'plates')
    parser.add_argument('--raw', type=Path, help='Reuse 640 by 416 RGBA sculpts named 0.rgba through 12.rgba')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for name, body, box in [('room', room(),(0,45,800,330)), ('counter',island(),(136,172,528,125)),
                            ('cloth',mat(),(0,0,800,480)), ('mark',crest('#f3ddb0'),(-36,-20,66,40))]:
        save(paint(body, box, box[2:]), name, 'shop.py / '+name, args.out)
    sheet = Image.new('RGBA',(9*72,12*80))
    for kind in range(3):
        for face in range(4):
            for pose in range(9):
                tile=paint(actor(kind,face,pose),(-36,-74,72,80),(72,80))
                sheet.paste(tile,(pose*72,(kind*4+face)*80))
    save(sheet,'cast','diners.py, four facings and nine poses per diner',args.out)
    save(paint(actor(0,0,0),(-36,-64,70,69),(196,193)),'otter','diners.py, ticket portrait',args.out)
    compress = path('M-13-51 Q-7-61 4-59 L11-55 L9-50 Q-1-54 -13-49Z', '#bed9ce', '#526f69', 1)
    compress += path('M-9-52 Q-3-55 7-54 M0-58 L3-55', 'none', '#80aaa0', 1)
    save(paint(actor(0,0,8)+compress,(-36,-64,70,69),(196,193)),'rest','diners.py, resting with a cooling cloth',args.out)
    with tempfile.TemporaryDirectory() as work:
        sculpt = str(Path(work)/'sculpt')
        if args.raw is None:
            subprocess.run(['cc','-O3','-std=c11',str(HERE/'sculpt.c'),'-lm','-o',sculpt],check=True)
        for i,name in enumerate(NAMES + tuple(n+'_cleared' for n in NAMES[:6])):
            raw = ((args.raw/f'{i}.rgba').read_bytes() if args.raw else
                   subprocess.check_output([sculpt,str(i),'640','416']))
            plate=Image.frombytes('RGBA',(640,416),raw)
            if i>6:
                save(plate.resize((67,44),Image.Resampling.LANCZOS),name,'sculpt.c, returned glaze',args.out)
                continue
            save(plate.resize((360,234),Image.Resampling.LANCZOS),name,'sculpt.c, '+name,args.out)
            save(plate.resize((67,44),Image.Resampling.LANCZOS),name+'_belt','sculpt.c, dining-room mip',args.out)


if __name__ == '__main__':
    main()
