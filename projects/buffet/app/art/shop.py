"""Hand-dyed menu cloth, pottery marks and collector print mounts."""
import random


def path(d, fill='none', stroke='none', width=1, extra=''):
    return f'<path d="{d}" fill="{fill}" stroke="{stroke}" stroke-width="{width}" stroke-linejoin="round" stroke-linecap="round" {extra}/>'


def rect(x, y, w, h, color, radius=0):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{color}"/>'


def oval(x, y, a, b, color):
    return f'<ellipse cx="{x}" cy="{y}" rx="{a}" ry="{b}" fill="{color}"/>'


def group(art, transform='', opacity=1):
    return f'<g transform="{transform}" opacity="{opacity}">{art}</g>'


def crest(color):
    return (path('M-24 5 C-27-8 -14-17 0-16 C10-16 19-11 23-3 C14-10 5-11-6-9 C-15-7-20 0-21 7Z',color)
            +path('M24-4 C28 8 15 17 1 17 C-10 17-19 12-23 5 C-14 10-2 12 8 9 C17 7 21 2 21-5Z',color)
            +group(oval(-4,0,6.5,2.4,color)+oval(6,1,5.2,2.1,color),'rotate(-26)'))


def meter(filled):
    # The fifth well ends at the capacity notch. The last two taper to points.
    a=rect(6,5,147,24,'#423f37',10)+rect(5,3,147,23,'#b79a6c',10)+rect(6,4,145,21,'#364c48',9)
    for i in range(8):
        x=36+14*i
        color=('#a4c8a8' if i<5 else '#e5b76f' if i==5 else '#ea8564') if filled else ('#59716a' if i<5 else '#806c4d' if i==5 else '#7f574d')
        shape=(f'M{x+2} 10 h9 l2 4 -2 6 h-9 q-2-4 0-10Z' if i<6 else f'M{x+2} 10 h6 l4 5 -4 5 h-7 l3-5Z')
        a+=path(shape,color)
        if filled:a+=path(f'M{x+3} 11.5 h5','none','#f2e6ba' if i<6 else '#ffd1a2',1)
    a+=path('M104 7 v14','none','#ebe1bb',1.4)+path('M117 7 l2-2 2 2 -2 2Z','#f4b17d')
    a+=path('M2 11 Q3 8 7 8 Q8 4 12 6 Q15 2 18 5 Q22 3 24 7 Q28 5 29 10Z','#ebe1c7','#504b40',1)
    a+=path('M1 10 Q12 13 31 9 Q29 18 20 21 L20 23 Q15 25 11 23 V21 Q4 20 1 10Z','#97bcb2','#413e39',1.2)
    a+=path('M3 12 Q14 15 28 12 M7 16 Q11 19 14 19','none','#e0dfbb',1.3)
    return a+path('M17 8 L26 0 M20 8 L31 2','none','#e3b078',1.15)


def mat():
    rng=random.Random(53)
    a=rect(0,0,800,480,'url(#titlecloth)')
    for x0,y0 in ((-65,113),(400,-80),(-40,445),(400,464),(748,60)):
        for row in range(4):
            for col in range(4):
                x=x0+col*25+(row%2)*12.5;y=y0+row*9
                for r in (5,9,13): a+=path(f'M{x-r} {y} a{r} {r*.7} 0 0 1 {r*2} 0','none','#3e7271',1)
    for _ in range(1500):
        x,y=rng.randrange(800),rng.randrange(480);a+=rect(x,y,rng.choice((1,2,3)),.8,rng.choice(('#346665','#2c5c5b','#2f6260')))
    for x in (0,13):
        a+=path(f'M{36+x} 286 L{142+x} 99 q3-4 6 0 L{39+x} 287Z','#183f42',extra='opacity=".42"')
        a+=path(f'M{32+x} 282 L{138+x} 93 q3-4 6 0 L{34+x} 283Z','#bc8557')
        a+=path(f'M{137+x} 96 l4 2 l-8 17 l-4-2Z','#daab70')
    a+=rect(0,465,800,15,'#1c4549')
    for x in range(0,800,10):a+=rect(x,468,5,1,'#809b88')
    return a


DEFS = '<linearGradient id="titlecloth" x1="0" y1="0" x2="1" y2=".13"><stop offset="0%" stop-color="#1e4a50"/><stop offset="50%" stop-color="#2c6563"/><stop offset="100%" stop-color="#204b50"/></linearGradient>'
