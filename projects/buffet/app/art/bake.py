"""Bake the original restaurant and character plates. The game ships the
indexed PNGs, so running or building it requires no art-authoring packages.
"""
import argparse
import io
from pathlib import Path

import cairosvg
from PIL import Image, PngImagePlugin

from service_layers import service_layers
from cutouts import Visitor
from shop import DEFS, crest, mat, meter, path, group, rect

HERE = Path(__file__).resolve().parent


def paint(body, box, size):
    x, y, w, h = box
    doc = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x} {y} {w} {h}" '
           f'width="{size[0] * 2}" height="{size[1] * 2}"><defs>{DEFS}</defs>{body}</svg>')
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
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    service_layers(HERE/'service', args.out)
    for name, body, box in [('gauge',meter(False),(0,0,154,29)), ('gauge_full',meter(True),(0,0,154,29)),
                            ('cloth',mat(),(0,0,800,480)), ('mark',crest('#f3ddb0'),(-36,-20,66,40))]:
        save(paint(body, box, (box[2]*2,box[3]*2)), name, 'shop.py / '+name, args.out)
    visitor=Visitor(HERE/'service'/'stations'/'player')
    sheet = Image.new('RGBA',(9*128,4*144))
    for face in range(4):
        for pose in range(9):
            tile=visitor.frame(face,pose).resize((128,144),Image.Resampling.LANCZOS)
            sheet.paste(tile,(pose*128,face*144))
    save(sheet,'cast','stations/player, four directions and a common sole anchor',args.out)
    save(visitor.frame(0,0),'guest','stations/player/front.png',args.out)
    save(visitor.frame(1,7),'rest','stations/player/savor.png',args.out)
    triptych=Image.open(HERE/'painting'/'collectibles.png').convert('RGBA')
    # Ema-size collector prints, each with an illustrated subject and its
    # own plate glaze. Text remains live in both languages outside the art.
    for i,(key,bg,ink) in enumerate([('card_bite','#397772','#a9c4a1'),('card_close','#b26f4f','#f0c58b'),('card_rest','#62748b','#c4c8c1'),('card_back','#385660','#8aa594')]):
        image=rect(1,1,110,142,'#453f37',7)+rect(2,2,108,138,'#e7d3a5',6)+rect(6,6,100,130,bg,4)
        image+=path('M13 20 v-6 h10 M88 14 h10 v7 M98 121 v7 h-10 M23 128 h-10 v-7','none',ink,1.4)
        for row in range(4):
            for col in range(3):
                xx=17+col*14+(row%2)*7; yy=99+row*6
                image+=path(f'M{xx} {yy} q5-5 10 0','none',ink,.7)
        image+=group(crest(ink),'translate(54 25) scale(.25)')
        if i==3:
            image+=group(crest('#e3c68f'),'translate(56 74) rotate(-18) scale(.93)')
            image+=path('M30 96 q14 7 36 2 M39 101 q9 3 21 1','none',ink,1)
        card=paint(image,(0,0,112,144),(224,288))
        if i<3:
            # Preserve the gilded edges and all the subject art. Trim the
            # joining gutter, then set the print into the collection mount.
            pane=triptych.crop((i*512+8,8,(i+1)*512-8,1016)).resize((202,260),Image.Resampling.LANCZOS)
            card.alpha_composite(pane,(11,13))
        save(card,key,'painting/collectibles.png and shop print mount',args.out)



if __name__ == '__main__':
    main()
