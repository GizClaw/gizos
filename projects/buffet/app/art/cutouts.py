"""Register native-alpha cel art to a heel anchor and pose it off-line.
The torso leads a stride, the nearer shoe lifts and swings, curls and hems
follow by less than a head-width. The three bite poses are separately drawn.
"""
import math
from PIL import Image, ImageOps


def cell(sheet, column, row):
    width, height = sheet.size
    tile = sheet.crop((column*width//3, row*height//2, (column+1)*width//3, (row+1)*height//2)).convert('RGBA')
    alpha = tile.getchannel('A')
    # Some exported clear pixels carry the painting's RGB. Only alpha
    # determines the contour, including the pale cuffs and shoe highlights.
    alpha = alpha.point(lambda a: 0 if a < 16 else a)
    tile.putalpha(alpha)
    box = alpha.point(lambda a: 255 if a > 120 else 0).getbbox()
    if box is None:
        raise ValueError('empty character pose')
    box=(max(0,box[0]-2),max(0,box[1]-2),min(tile.width,box[2]+2),min(tile.height,box[3]+2))
    return tile.crop(box)


def anchored(source, size=(272,384), tall=340):
    scale=min(tall/source.height,(size[0]-12)/source.width)
    art=source.resize((round(source.width*scale),round(source.height*scale)),Image.Resampling.LANCZOS)
    dest=Image.new('RGBA',size)
    dest.alpha_composite(art,((size[0]-art.width)//2,356-art.height))
    return dest


def posed(source, face, pose, registered=False):
    base=source.copy() if registered else anchored(source)
    phase=(0,-1,-.18,1,.22,0,0,0,0)[pose]
    if phase:
        def inverse(x,y):
            t=max(0,min(1,(y-18)/338));side=max(-1,min(1,(x-136)/29));feet=max(0,(t-.66)/.34)
            bob=3*(1-abs(phase))*(1-t)
            head=max(0,1-abs(t-.26)/.28)
            hair=(max(0,(abs(x-136)-45)/65))*head
            dx=phase*((4.7*head + 5.2*hair)+(21 if face in (1,3) else 4)*side*feet)
            dy=bob+feet*max(0,-phase*side)*12+2*side*head*phase
            return x-dx,y+dy
        mesh=[]
        for y in range(0,384,16):
            for x in range(0,272,16):
                points=[inverse(x,y),inverse(x,y+16),inverse(x+16,y+16),inverse(x+16,y)]
                mesh.append(((x,y,x+16,y+16),tuple(a for pair in points for a in pair)))
        base=base.transform(base.size, Image.Transform.MESH, mesh,Image.Resampling.BICUBIC)
    if face==3:base=ImageOps.mirror(base)
    return base


class Cast:
    def __init__(self, paintings):
        first=Image.open(paintings/'regulars.png')
        self.people=[[cell(first,col,row) for col in range(3)] for row in (0,1)]
        self.visitor={p.stem:Image.open(p).convert('RGBA') for p in (paintings/'visitor').glob('*.png')}

    def frame(self, kind, face, pose):
        if kind==0:
            key='back' if face==2 else 'front' if face==0 else 'quarter-right'
            if 1<=pose<=4 and face in (1,3):key='profile-right'
            if pose==5:key='quarter-right'
            if pose==6:key='bite-right'
            if pose==7:key='savor-right'
            if pose==8 and face in (1,3):key='step-right'
            if pose==8 and face==0:key='front-blink'
            return posed(self.visitor[key],face,pose,True)
        image=self.people[kind-1][2 if face==2 else 0 if face==0 else 1]
        return posed(image,face,pose)
