"""Bake fixed Chinese UI labels (font rasterization, not generated artwork).
Usage: python pack_ui_labels.py /path/to/CJK-font.ttc output.h2r8
"""
import sys, struct, zlib
from PIL import Image, ImageDraw, ImageFont

labels=("面对心魔","挑战虚空","选择模式","寻找对手","连接失败","返回")
image=Image.new("RGBA",(160,32*len(labels)))
draw=ImageDraw.Draw(image)
font=ImageFont.truetype(sys.argv[1],24)
for row,label in enumerate(labels):
    box=draw.textbbox((0,0),label,font=font)
    draw.text(((160-(box[2]-box[0]))/2,32*row+(32-(box[3]-box[1]))/2-box[1]),
              label,font=font,fill=(195,242,255,255))
with open(sys.argv[2],"wb") as stream:
    stream.write(b"H2R8"+struct.pack("<HH",*image.size)+zlib.compress(image.tobytes(),9))
