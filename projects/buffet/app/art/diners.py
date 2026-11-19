"""Posed cut-paper character rigs. Origin at heel, positive Y down."""
from shop import group, linear, oval, path

CAST_DEFS = ''.join((
    linear('fur0', [(0,'#dfa76b'),(50,'#c88953'),(100,'#a26846')],False),
    linear('fur1', [(0,'#a5a8a0'),(54,'#90988c'),(100,'#6b7973')],False),
    linear('fur2', [(0,'#f2e4c6'),(54,'#dfd2b7'),(100,'#b5ad99')],False),
    linear('jacket0',[(0,'#527d80'),(50,'#436a70'),(100,'#2e505d')],False),
    linear('jacket1',[(0,'#a77c83'),(50,'#8c6877'),(100,'#654e65')],False),
    linear('jacket2',[(0,'#b5a154'),(50,'#998c54'),(100,'#76764f')],False),
))


def actor(kind, direction, pose):
    side = direction in (1, 3)
    back = direction == 2
    ink = '#493e36' if kind == 0 else '#464941'
    fur = f'url(#fur{kind})'
    coat = f'url(#jacket{kind})'
    scarf = ('#dc6948','#d69f65','#87a89d')[kind]
    hi = ('#ffb47c','#f4c88b','#caddb6')[kind]
    belly = ('#f6d7a1','#dfd9be','#f7e9cc')[kind]
    walk = (0,-1,0,1,0,0,0,0,0)[pose]
    bob = (0,-.9,-2,-.9,0,2,-1,.7,0)[pose]
    anticipating, lifting, chewing = pose == 5, pose == 6, pose == 7
    a = ''
    if kind == 0 or kind == 1:
        tail = path('M-8-18 Q-17-18 -23-23 Q-28-28 -30-27 Q-31-22 -27-17 Q-20-8 -10-10 Z',fur,ink,1.3)
        if kind == 1:
            tail+=path('M-24-23 q-1 3 -4 3 m7 1 q-1 4 -4 4','none','#595e58',3)
        else:
            tail+=path('M-28-25 Q-22-17 -14-17','none','#e0ab73',1.4)
        a+=group(tail, f'rotate({walk*6+(8 if back else 0)} -10 -15)')
    else:
        a += oval(-11,-15,6,5,'#d6cab3')+oval(-13,-17,4,3,'#f3e5c9')
    for j in (-1,1):
        dx=j*(5.5 if not side else 3.3)+(walk*4.5*j if side else 0)
        dy=walk*2.9*j if not side else min(0,walk*j)*2.6
        leg=path('M-3-11 Q0-13 4-11 L4-3 Q3 0 -2 0 L-4-3 Z','#645348',ink,1)
        leg+=path('M-4-4 Q0-6 4-5 Q9-5 8-1 Q5 2 -3 1 Q-5 0 -4-4Z','#795338',ink,1)
        leg+=path('M-2 0 q5 1 8-1','none','#c69667',1.2)
        a+=group(leg, f'translate({dx} {dy})')
    body=path('M-9-32 Q-15-28 -14-13 Q-9-8 2-9 Q9-9 15-14 Q14-29 8-32Z',coat,ink,1.15)
    body+=path('M-12-13 Q1-8 13-14','none','#9aac99',1.4)
    if not back:
        body+=path('M-7-32 L2-16 L9-31','none','#c2bc96',1.5)
        body+=path('M-10-17 Q1-13 12-18','none','#494943',2.2)
        body+=path('M-10-20 Q-8-17 -6-20 M5-15 q1 2 3 0','none','#8ba7a0',1)
    else:
        body+=path('M-9-26 Q0-21 9-26','none','#6b8986',1.2)
        body+=path('M-10-29 L8-12','none','#ae8057',2.6)+oval(8,-13,6,3.7,'#b3895d')
    body+=path('M-10-31 Q-1-28 11-33 L13-29 Q2-24 -10-28Z',scarf,ink,.75)
    body+=path(f'M-7-29 Q-13-28 -17 {-24+walk*2} L-11-26 L-11-23 Q-7-25 -6-28Z',scarf)
    # One sleeve swings behind the cheek. At bite contact the other reaches
    # the mouth. The held morsel follows the paw through the settle pose.
    swing= -5*walk if side else -2*walk
    sleeve=path('M-10-29 Q-14-30 -16-20 L-12-17 Q-9-19 -7-26Z',coat,ink,1)+oval(-13,-18,3.3,3.2,belly)
    body+=group(sleeve, f'rotate({swing} -11 -28)')
    head=''
    ex=7 if side else 0
    if kind==2:
        for off,angle in ((-7,-14),(5,12)):
            ear=path('M-3 3 Q-7-8 -4-17 Q-2-22 1-19 Q5-15 4 1Z',fur,ink,1.2)
            ear+=path('M-2-3 Q-4-12 -2-15 Q1-16 1-3Z','#c89787')
            head+=group(ear, f'translate({off} -51) rotate({angle+walk*3})')
    else:
        for xx,yy,ss in ((-13,-48,1),(10 if side else 13,-48,.92)):
            head+=group(oval(0,0,5.6,5.3,ink)+oval(0,-.3,4.5,4.4,fur)+oval(.3,.15,2.75,2.5,'#94674e' if kind==0 else '#67736b'),f'translate({xx} {yy}) scale({ss})')
    head+=path('M-17-43 C-16-52 -6-55 3-54 C13-53 18-48 18-42 Q22-37 16-33 Q9-27 -2-28 Q-14-29 -18-35 Q-20-39 -17-43Z',fur,ink,1.4)
    head+=path('M-13-46 Q-10-52 -3-51 M-8-48 q3-4 6-4','none',hi,1.1)
    if back:
        head+=path('M-12-37 q2 3 6 3 m10-1 q4-1 6-4','none', '#9d775e' if kind==0 else '#949b87',1)
    else:
        if kind==1:
            for off in ((10,) if side else (-8,8)):
                head+=group(path('M-7-44 Q0-47 7-42 L4-36 Q-4-36 -7-40Z','#4d5854'),f'translate({off} 0)')
        face=path('M-13-36 Q-10-42 -4-39 Q0-42 4-40 Q11-41 15-37 Q16-31 6-29 Q-3-27 -11-31Z', belly)
        face+=path('M-2-38 Q0-40 4-39 Q6-38 2-35 Q0-35 -2-38Z','#42392f')
        face+=path('M2-35 v2 q2 2 4 0 m-4 .3 q-2 2-4 .2','none',ink,1)
        for x in (-7,8):
            face+=oval(x,-34, .7,.65,'#9d795e')+oval(x+2,-35,.7,.65,'#9d795e')
        head+=group(face, f'translate({ex} 0) scale({.77 if side else 1} 1)')
        for x in ((11,) if side else (-7.3,8.8)):
            if chewing or anticipating or pose == 8:
                head+=path(f'M{x-2} -41 q2 -2 4 0','none',ink,1.5)
            else:
                head+=oval(x,-41.8,1.8,2.55,'#3a3930')+oval(x-.48,-42.85,.6,.74,'#fff1d4')
        if kind==0: head+=oval(-9 if not side else 3, -37, 2.6,1.4,'#d6946c')
    head_y = 1.8 if anticipating else -1 if lifting else 0
    body+=group(head,f'translate({1 if lifting and side else 0} {head_y}) rotate({3 if chewing else 0} 0 -37)')
    # The ochre seam and fluttering scarf identify the player even facing
    # away. Place the knot over the lapel so it survives the waist-high view.
    if kind == 0:
        body+=path('M-12-31 Q-3-27 10-31 L7-27 Q-1-23 -11-28Z',scarf,ink,.65)
        body+=path(f'M-9-29 Q-17-27 -19 {-20+walk*2} Q-15-22 -12-22 L-13-19 Q-8-23 -7-28Z',scarf,ink,.65)
        body+=path('M-11-29 Q-3-26 5-28','none',hi,.9)
    if lifting or chewing:
        body+=path('M9-25 Q12-30 9-34 Q7-36 5-33 L3-29 Q5-24 9-25Z',belly,ink,.9)
        body+=path('M9-32 l-9-2 m8 .3 l-10-1.8','none','#9f6d43',1)
        if lifting: body+=oval(0,-33.5,3.5,2.1,'#f1d9a6')+oval(-.4,-35,3.7,1.5,'#e88155')
    elif side:
        body+=group(path('M2-27 q5-3 7 3 l-1 7 q-2 2-5 0Z',coat,ink,1)+oval(5,-17,3,2.4,belly),f'rotate({walk*24} 6 -27)')
    else:
        body+=group(oval(12,-21,3.7,5,coat)+oval(12,-18,3.3,2.4,belly), f'translate(0 {walk*2})')
    a+=group(body,f'translate(0 {bob})')
    return group(a,'scale(-1 1)') if direction==3 else a
