#!/usr/bin/env python3
"""Side-by-side review only. Reference pixels are never used by the game."""
from pathlib import Path
import argparse
from PIL import Image, ImageDraw
parser=argparse.ArgumentParser()
parser.add_argument('--captures',type=Path,required=True)
a=parser.parse_args()
root=Path(__file__).resolve().parents[1]
rows=[
 ('approved-storyboard.png',(29,82,505,432),'overhead.png','OVERHEAD'),
 ('approved-storyboard.png',(1045,518,1521,858),'fight.png','FIGHT'),
 ('approved-inventory.png',(97,88,654,738),'rods.png','RODS'),
 ('approved-inventory.png',(706,88,1263,738),'reels.png','REELS'),
 ('approved-inventory.png',(1310,88,1867,738),'lures.png','LURES'),
]
for group,indices in [('sea',[0,1]),('gear',[2,3,4])]:
    image=Image.new('RGB',(776,len(indices)*488+8),'#0c2034')
    draw=ImageDraw.Draw(image)
    for row,index in enumerate(indices):
        source,box,native,label=rows[index]
        ref=Image.open(root/'design'/source).convert('RGB').crop(box).resize((368,448),Image.Resampling.NEAREST)
        frame=Image.open(a.captures/native).convert('RGB')
        y=30+row*488
        image.paste(ref,(10,y));image.paste(frame,(398,y))
        draw.text((10,y-20),'REFERENCE / '+label,fill='#d4edf1')
        draw.text((398,y-20),'LUA + SDL / '+label,fill='#d4edf1')
    image.save(a.captures/('comparison-'+group+'.png'))
print('Reference comparisons saved. Left: accepted concept scaled to native aspect. Right: actual framebuffer.')
