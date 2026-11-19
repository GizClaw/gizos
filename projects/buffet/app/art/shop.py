"""Offline illustration plates. Coordinates are in logical game pixels."""
import math
import random


def path(d, fill='none', stroke='none', width=1, extra=''):
    return f'<path d="{d}" fill="{fill}" stroke="{stroke}" stroke-width="{width}" stroke-linejoin="round" stroke-linecap="round" {extra}/>'


def rect(x, y, w, h, color, radius=0):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{color}"/>'


def oval(x, y, a, b, color):
    return f'<ellipse cx="{x}" cy="{y}" rx="{a}" ry="{b}" fill="{color}"/>'


def group(art, transform='', opacity=1):
    return f'<g transform="{transform}" opacity="{opacity}">{art}</g>'


def linear(name, colors, vertical=True):
    stops = ''.join(f'<stop offset="{p}%" stop-color="{c}"/>' for p, c in colors)
    end = 'x2="0" y2="1"' if vertical else 'x2="1" y2=".13"'
    return f'<linearGradient id="{name}" x1="0" y1="0" {end}>{stops}</linearGradient>'


def crest(color):
    # Two cuts in a continuous brush fish. The tail curls into the next lap.
    body = 'M-25 2 C-18-12 -2-16 13-8 Q25-1 24 6 Q17 18 2 16 Q-12 15 -22 6 L-32 13 Q-32 6 -27 2 L-34-7 Z M10-3 a2.8 2.8 0 1 0 .1 0 Z'
    return path(body, color, extra='fill-rule="evenodd"') + path('M-17 3 Q-8-3 0-1 M-13 7 Q-5 2 2 4', 'none', '#275859', 1.6)


def lantern(x, y, scale=1):
    a = path('M0-35 L0-10', 'none', '#3e3734', 2)
    a += oval(2, 6, 13, 18, '#72523c') + oval(0, 4, 13, 18, 'url(#paperlamp)')
    for dy, dx in [(-9, 8), (-4, 11), (2, 12), (8, 12), (14, 10)]:
        a += path(f'M{-dx} {dy} Q0 {dy+4} {dx} {dy}', 'none', '#ce8d54', .8)
    a += rect(-7, -13, 14, 4, '#514238', 1.8) + rect(-6, 19, 12, 3, '#594638', 1.5)
    a += path('M0 22 v4', 'none', '#ae6147', 2)
    return group(a, f'translate({x} {y}) scale({scale})')


def cup(x, y, scale=1, soy=False):
    a = oval(2, 2, 8, 3.3, '#9c7954')
    if soy:
        a += path('M-5-2 L-5-9 Q-5-12 -2-13 L-2-17 L2-17 L2-13 Q6-12 6-8 L6-2 Q2 1 -5-2', '#503a30')
        a += rect(-3, -19, 6, 3, '#ad5035', 1) + rect(-4, -10, 8, 5, '#dcc28b', 1)
    else:
        a += path('M-6-9 L6-9 L4 0 Q0 2 -4 0 Z', '#75948a') + oval(0, -9, 6.1, 2.5, '#e4e8c9') + oval(0,-9,4.1,1.4,'#6b7959')
        a += path('M-3-6 L-2-2','none','#c5d6bc',1.4)
    return group(a, f'translate({x} {y}) scale({scale})')


def arcbox(xl, xr, cy, rx, ry, face=0):
    return f'M{xl} {cy-ry} H{xr} C{xr+rx*1.34} {cy-ry} {xr+rx*1.34} {cy+ry+face} {xr} {cy+ry+face} H{xl} C{xl-rx*1.34} {cy+ry+face} {xl-rx*1.34} {cy-ry} {xl} {cy-ry} Z'


def booth(x, y, mirrored):
    # A raised tatami bay at the cutaway edge. The inlaid table has a tray,
    # two cups, a sitting cushion and slats visible through the low partition.
    a = path('M-26-30 H57 L77-14 V36 L61 45 H-26 Z', '#a88a6b', extra='opacity=".56"')
    a += path('M-26-34 H55 L71-21 V29 L59 39 H-26 Z', '#87664d')
    a += path('M-26-35 H56 L70-22 V25 L57 34 H-26 Z', '#c7b783')
    for yy in range(-20,31,3): a += path(f'M-9 {yy} H56', 'none', '#a7a377', .7)
    a += path('M-16 16 Q8 13 29 16 L34 25 Q17 29 -8 26Z','#6d7765')
    a += path('M-16 14 Q8 11 29 14 L34 23 Q17 27 -8 24Z','#a5a17b')
    a += path('M-15-30 H52 L65-17 H-15Z','#cca278')
    for xx in (1,16,31,46,61): a += rect(xx,-35,2.4,26,'#916741')
    a += rect(-14,-37,77,3,'#ddb583')
    a += path('M-16-17 H50 L58-8 V8 L49 14 H-16Z','#b6865f')
    a += path('M-16-22 H48 L58-12 V3 L49 8 H-16Z','url(#topwood)')
    a += path('M-10 3 H47 L54 0','none','#f4dcaa',1)
    a += oval(29,-6,11,3,'#bd956e') + oval(28,-7,10,3.6,'#517970') + oval(28,-8.2,9.7,3.2,'#b9d2ba')
    a += cup(9,-4,.85)+cup(43,-13,.66,True)
    a += path('M19 1 l16 -1 m-16 2.4 l16 -1','none','#82543e',1)
    return group(a, f'translate({x} {y}) scale({-1 if mirrored else 1} 1)')


def room():
    rng = random.Random(709)
    art = rect(0,0,800,480,'url(#woodfloor)')
    # Cypress floorboards, staggered with sparse heartwood lines at knots.
    for i, y in enumerate(range(122, 395, 20)):
        art += path(f'M0 {y} H800', 'none','#bc986e',1.4) + path(f'M0 {y+1.4} H800','none','#f8dba5',1)
        for x in range((i*87)%183-183,800,183):
            art += path(f'M{x} {y+1} l-6 18','none','#bb9369',1.15)
            a, yy = x+rng.randrange(12,75), y+rng.randrange(5,14)
            art += path(f'M{a} {yy} q20 -2 38 0 t44 0', 'none', '#d0aa79', .65)
            if rng.random()<.45:
                art += path(f'M{a+4} {yy+2} q13 -3 23 0 q-9 1 -15 0', 'none','#be996f',.7)
    art += group(path('M-36 102 H82 L-24 351 H-88 Z M98 116 H127 L45 302 H16 Z M548 103 H658 L564 326 H455 Z','#fff1be'),opacity=.19)
    # Shoji windows frame a distant river bank. Counter height and the two
    # side benches leave a continuous lane behind the conveyor.
    art += rect(0,35,800,98,'#544c40')+rect(7,40,786,80,'url(#window)')
    art += path('M0 92 Q40 69 83 83 T160 76 Q198 81 233 66 Q291 52 325 72 T392 74 Q438 62 466 70 Q532 51 583 72 Q648 91 705 64 Q743 54 805 65 V115 H0Z','#9bb9a5')
    art += path('M0 101 Q70 79 135 100 T287 87 T447 86 T593 91 T800 81 V116 H0Z','#799b88')
    for x in range(12,800,32): art += rect(x,46,2,69,'#b68e64')
    art += rect(0,78,800,2,'#c2976c')+rect(0,102,800,3,'#9d795b')
    for x in (4,157,320,480,640,791): art += rect(x,43,8,80,'#77523d')+rect(x,43,2,79,'#dab07c')
    art += rect(0,108,800,8,'#754e39')+rect(0,115,800,13,'url(#facewood)')+rect(0,128,800,4,'#916647')
    # A three-part indigo noren hangs in the central service bay.
    art += rect(290,48,222,60,'#423e38')
    for i in range(5):
        x=290+i*45
        art+=path(f'M{x} 46 h44 v42 q-8 7 -20 4 q-13 0 -24 3Z', 'url(#cloth)')
        art+=path(f'M{x+5} 49 v39 m33-39 v38','none','#4e7672',.9)
    art+=group(crest('#e4d6a6'),'translate(400 70) scale(.91)')
    for x in (316,479): art+=path(f'M{x-9} 74 q6 -4 12 0 t12 0 m-20 5 q5 -3 10 0 t10 0','none','#87a89a',1)
    art += rect(278,44,244,4,'#473f36')+rect(280,42,240,3,'#d4aa77')
    for x in (30,146,272,530,654,770): art+=lantern(x,69 if x in (272,530) else 60, .80 if x in (30,770) else .92)
    # Small timber booths and woven mats, clipped at either wall.
    for x in (-29,680):
        art+=path(f'M{x+2} 125 h154 l-5 22 h-158Z','#ae8b65')
        art+=rect(x,109,149,25,'#a77853',4)+rect(x+3,110,144,18,'#ddd1a0',3)
        for y in range(113,126,3): art+=rect(x+3,y,144,.7,'#b7aa7c')
        art+=rect(x+2,128,144,6,'#84614d')
        art+=path(f'M{x+15} 119 h100 l12 13 h-124Z','#efd1a1')+rect(x+3,132,124,4,'#a47555')
        art+=cup(x+74,127,.75)+cup(x+60,125,.68,True)
        for xx in (x+4,x+127): art+=rect(xx,111,6,41,'#ab805b',2)+rect(xx,110,7,3,'#efd2a0',1)
    # Knife rail and stacks of blue dishes at the pass.
    art+=rect(301,104,200,7,'#d7af7f')+rect(301,110,200,3,'#8e6849')
    for x in (316,483):
        for i in range(3): art+=oval(x,104-i*2.8,11,3.7,'#5d847c')+oval(x,103-i*2.8,10.7,2.8,'#d5dfc6')
    art+=cup(339,106,.80,True)+cup(460,107,.9)
    for x in (15,782):
        art+=rect(x-8,157,16,12,'#776953',3)+oval(x,157,8.2,2.6,'#526958')
        for j in range(9):
            dx=(j-4)*3.9;dy=13+rng.randrange(14)
            art+=path(f'M{x} 157 q{dx*.8} {-dy*.4} {dx} {-dy} q{-dx*.7-3} 2 {-dx} {dy}', ['#426f56','#67916e','#86a475'][j%3])
    for x, mirror in ((0,False),(800,True)):
        for y in (213,322): art+=booth(x,y,mirror)
    # A deep valance and a narrow strip of end-grain frame the room.
    art+=rect(0,45,800,4,'#584a3c')+rect(0,49,800,1.2,'#e2b886')
    return art


def island():
    a=path(arcbox(217,583,238,69,46,18),'#ac805a',extra='opacity=".43"')
    a+=path(arcbox(217,583,235,66,43,13),'url(#facewood)')
    for x in range(213,590,13):
        a+=path(f'M{x} 264 v22','none','#ba8657',1)+path(f'M{x+2} 264 v22','none','#e4bc83',1)
    a+=path('M183 279 Q199 289 221 289 H579 Q609 289 630 275','none','#77533d',3)
    a+=path(arcbox(217,583,225,68,45,3),'#765846')+path(arcbox(217,583,224,67,44),'url(#topwood)')
    a+=path(arcbox(216,584,224,60,38),'#7d6b59')+path(arcbox(216,584,223,59,37),'#e8d5ab')
    a+=path(arcbox(216,584,224,56,34),'#526967')+path(arcbox(216,584,224,53,31),'#8ca097')
    a+=path(arcbox(216,584,223,44,21),'#d8bf97')+path(arcbox(216,584,221,42,19),'url(#topwood)')
    # An inset leaf motif, lacquer tea service and stacked saucers share the island.
    a+=group(crest('#777d66'),'translate(406 220) scale(.74 .54)')
    for x in (273,514):
        a+=path(f'M{x-24} 222 q20 5 48 1 l4 3 q-35 5 -54-1Z','#b78c64')
        for i in range(3):
            a+=oval(x-12,221-i*2.1,11,3.5,'#66867b')+oval(x-12,220-i*2.1,10.7,2.6,'#d4dac0')
        a+=cup(x+5,223,.89,True)+cup(x+18,225,.90)
    a+=rect(351,255,97,10,'#836249',4)+rect(355,256,89,4,'#403c33',2)
    a+=path('M351 266 H448','none','#eed0a3',1)
    return a


def mat():
    rng=random.Random(53)
    a=rect(0,0,800,480,'url(#titlecloth)')
    # The print is tucked into the corners so the plate and type have air.
    for x0,y0 in ((-65,113),(400,-80),(-40,445),(400,464),(748,60)):
        for row in range(4):
            for col in range(4):
                x=x0+col*25+(row%2)*12.5;y=y0+row*9
                for r in (5,9,13): a+=path(f'M{x-r} {y} a{r} {r*.7} 0 0 1 {r*2} 0','none','#3e7271',1)
    for _ in range(1500):
        x,y=rng.randrange(800),rng.randrange(480);a+=rect(x,y,rng.choice((1,2,3)),.8,rng.choice(('#346665','#2c5c5b','#2f6260')))
    # Two tapered lacquer chopsticks beyond the rim, a folded selvage at the table edge.
    for x in (0,13):
        a+=path(f'M{36+x} 286 L{142+x} 99 q3-4 6 0 L{39+x} 287Z','#183f42',extra='opacity=".42"')
        a+=path(f'M{32+x} 282 L{138+x} 93 q3-4 6 0 L{34+x} 283Z','#bc8557')
        a+=path(f'M{137+x} 96 l4 2 l-8 17 l-4-2Z','#daab70')
    a+=rect(0,465,800,15,'#1c4549')
    for x in range(0,800,10): a+=rect(x,468,5,1,'#809b88')
    return a


DEFS = ''.join((
    linear('woodfloor',[(0,'#d6b389'),(29,'#dfc398'),(51,'#f2d7aa'),(79,'#dbb689'),(100,'#bc9671')]),
    linear('facewood',[(0,'#e9c08a'),(35,'#d7a973'),(86,'#c18f62'),(100,'#aa7d59')]),
    linear('topwood',[(0,'#f6e0ae'),(52,'#e6c593'),(100,'#d9b182')]),
    linear('window',[(0,'#8ab8ba'),(47,'#dae1c1'),(100,'#f6dbab')]),
    linear('cloth',[(0,'#315d61'),(52,'#345f64'),(94,'#244b50'),(100,'#406567')]),
    linear('titlecloth',[(0,'#1e4a50'),(50,'#2c6563'),(100,'#204b50')],False),
    linear('paperlamp',[(0,'#bc7049'),(18,'#f2b871'),(39,'#ffe3a1'),(73,'#f3c078'),(100,'#c88550')],False),
))
