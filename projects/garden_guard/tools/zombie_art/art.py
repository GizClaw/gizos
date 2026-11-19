"""Original Garden Guard character drawings. Coordinates are in a 132×192 drawing space."""
from html import escape

INK = '#302633'
FAR_INK = '#373043'


def path(d, fill='none', stroke='none', width=3):
    return (f'<path d="{d}" fill="{fill}" stroke="{stroke}" stroke-width="{width}" '
            'stroke-linecap="round" stroke-linejoin="round"/>')


def ellipse(x,y,rx,ry,fill):
    return f'<ellipse cx="{x}" cy="{y}" rx="{rx}" ry="{ry}" fill="{fill}"/>'


def group(parts, transform=''):
    return f'<g transform="{escape(transform)}">' + ''.join(parts) + '</g>'


def svg(parts):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="132" height="192" '
            'viewBox="0 0 132 192">' + ''.join(parts) + '</svg>')


def leg(hip, knee, ankle, tilt, near, variant='gardener'):
    x,y = hip
    k,j = knee
    a,b = ankle
    ink = INK if near else FAR_INK
    trouser = '#4c566c' if near else '#34384f'
    shin = '#718098' if near else '#545d75'
    rubber = '#835356' if near else '#644354'
    sole, boot_light, boot_rim = ('#d19567','#be8177','#ca9381') if near else ('#8b6661','#936570','#8f6a78')
    if variant == 'armored':
        trouser, shin, rubber, sole, boot_light, boot_rim = (
            ('#434957','#60687b','#45666e','#c7915c','#81a9a4','#b08c68') if near else
            ('#333c4a','#4e5766','#34515c','#806a60','#538580','#826b5e'))
    elif variant == 'runner':
        trouser, shin, rubber, sole, boot_light, boot_rim = (
            ('#595379','#7f7293','#934848','#e5b875','#cf7960','#eaa37d') if near else
            ('#443f63','#635675','#713b46','#aa845b','#ad6559','#b48274'))
    boot = [
        path('M-6 -11 Q1 -13 7 -11 L8 -2 Q12 3 10 8 Q9 11 3 11 H-22 Q-29 10 -28 5 '
             'Q-28 -1 -19 -3 L-9 -4 Z',rubber,ink,3.5),
        path('M-26 5 Q-18 7 -9 6 H9 Q9 9 4 9 H-21 Q-26 9 -26 5Z',sole),
        path('M-23 2 Q-17 -1 -10 0','none',boot_light,2.4),
        path('M-6 -9 L6 -9','none',boot_rim,2.5),
    ]
    return group([
        path(f'M{x-11} {y-3} Q{x-2} {y-9} {x+12} {y-2} '
             f'Q{k+14} {j-6} {k+9} {j+2} L{a+8} {b-5} '
             f'Q{a+0} {b-1} {a-9} {b-5} Q{k-7} {j+8} {k-9} {j+1} '
             f'Q{k-11} {j-7} {x-11} {y-3}Z',trouser,ink,3.8),
        path(f'M{x-6} {y+1} Q{k-3} {j-7} {k-4} {j+1} L{a-5} {b-8} '
             f'L{a+3} {b-10} Q{k+4} {j+5} {k+3} {j-2} Q{k+5} {j-9} {x-6} {y+1}Z',shin),
        group(boot, f'translate({a} {b}) rotate({tilt})' +
              (' scale(1.13 1)' if variant == 'armored' else ' scale(0.86 1)' if variant == 'runner' else ''))
    ])


def coat():
    # The shape from shoulder through rump is one piece. The apron follows its contour.
    return group([
        path('M60 83 Q68 72 81 75 Q99 76 106 93 Q112 103 108 120 L110 134 '
             'Q97 147 72 144 L59 137 Q59 126 54 113 Q49 97 60 83Z','#bd7770',INK,4),
        path('M77 78 Q104 76 106 100 Q110 115 105 126 L106 134 Q91 143 74 140 L72 127 '
             'Q90 118 89 96Z','#854f5a'),
        path('M60 91 Q63 83 73 82 Q69 93 70 106 L62 110 Q57 101 60 91Z','#df9782'),
        path('M73 87 Q77 84 80 87 L85 103 L92 106 Q96 120 104 136 Q94 143 72 142 L62 136 '
             'Q65 122 60 107 L68 106Z','#656282',INK,2.7),
        path('M67 112 Q73 111 82 110 Q87 125 91 137 Q81 140 67 136 Q70 128 67 112Z','#85819f'),
        path('M84 122 L96 119 L100 130 Q98 134 89 136Z','#5b516f'),
        path('M85 122 L95 120','none','#aaa0b2',2.2),
        ellipse(75,104,2.6,2.6,'#f0c584'),
        path('M66 78 Q76 77 86 84 L84 91 Q76 90 72 85 L68 92 L58 89Z','#db8f76',INK,2.5),
    ])


def hanger(mode):
    # A redrawn gesture from sleeve edge to fingertips at each state, not shape tweening.
    if mode == 'reach':
        sleeve = 'M77 87 Q88 83 95 91 Q101 99 92 108 Q80 115 69 111 L63 101 Q67 92 77 87Z'
        sleeve_light = 'M77 90 Q85 87 91 92 Q95 98 90 101 Q85 93 69 100 Q71 94 77 90Z'
        hand = ('M72 99 Q65 97 59 103 L42 104 L38 100 Q32 99 26 104 Q23 106 25 110 '
                'L29 109 Q29 116 32 116 L35 114 Q38 117 42 112 Q58 115 72 109 Q76 104 72 99Z')
        light = 'M70 101 Q65 100 59 105 Q52 108 43 106 Q35 103 29 106 Q32 106 34 111 Q38 108 42 109 Q55 111 69 107Z'
        cuffs = 'M68 96 L74 98 Q77 104 72 111 L65 112 Q70 105 68 96Z'
    elif mode == 'forward':
        sleeve = 'M77 88 Q89 85 96 94 Q100 102 94 109 Q88 116 76 114 L68 106 Q69 95 77 88Z'
        sleeve_light = 'M77 91 Q87 87 93 95 Q93 99 89 101 Q80 97 73 103 Q73 96 77 91Z'
        hand = ('M76 105 Q69 107 64 115 L54 122 L47 120 Q43 119 39 123 Q35 126 38 129 '
                'Q40 129 43 127 L42 133 Q43 137 46 134 L49 131 Q51 135 54 132 L60 128 '
                'Q75 124 82 113 Q83 108 76 105Z')
        light = 'M77 107 Q72 109 65 117 Q59 125 51 125 L45 123 L40 125 Q42 125 46 127 L45 132 L50 128 Q60 130 76 115Z'
        cuffs = 'M73 104 Q77 103 85 109 L82 115 Q75 115 69 110Z'
    elif mode == 'back':
        sleeve = 'M80 87 Q92 85 99 96 Q102 103 102 114 Q94 120 84 116 Q78 111 76 102 Q74 93 80 87Z'
        sleeve_light = 'M79 94 Q82 87 90 91 Q96 95 97 103 Q86 102 82 108 Q78 101 79 94Z'
        hand = ('M88 111 Q96 107 100 113 L101 128 Q102 134 97 142 Q93 146 92 142 '
                'L94 137 L89 141 Q86 142 86 139 L90 134 Q86 135 84 132 Q84 128 92 126 Q91 118 88 111Z')
        light = 'M92 113 L96 114 L98 128 Q97 135 94 137 L95 132 L88 132 L94 128Z'
        cuffs = 'M86 112 Q94 111 100 108 L102 115 Q97 119 88 118Z'
    else:
        sleeve = 'M78 87 Q89 84 98 94 Q102 103 95 117 Q88 121 79 116 Q75 111 75 103 Q73 93 78 87Z'
        sleeve_light = 'M78 94 Q80 89 87 89 Q95 93 96 101 Q87 99 80 107 Q77 101 78 94Z'
        hand = ('M82 112 Q90 109 96 115 Q94 123 88 132 L84 140 Q80 146 77 143 Q77 141 79 136 '
                'L75 141 Q71 143 70 140 L74 133 Q70 135 68 132 Q67 128 76 126 Q81 121 82 112Z')
        light = 'M86 114 Q90 113 92 116 Q89 126 82 132 L77 138 L79 131 L72 131 L78 128 Q82 122 86 114Z'
        cuffs = 'M79 109 Q88 112 96 109 L96 116 Q89 122 80 117Z'
    return group([
        path(hand,'#6b9b8b',INK,3.6), path(light,'#a5c5a4'),
        path(sleeve,'#c48479',INK,3.8), path(sleeve_light,'#e3a18a'),
        path(cuffs,'#a15e63',INK,2.4),
    ])


def hat():
    return group([
        path('M30 32 Q29 23 37 15 Q41 9 50 10 L55 7 Q70 4 81 12 Q91 18 92 29 L102 36 '
             'Q61 45 30 32Z','#d9a558',INK,3.8),
        path('M34 26 Q38 14 49 14 Q54 8 66 10 Q80 10 86 21 Q68 13 61 19 '
             'Q48 16 34 26Z','#f4ce7d'),
        path('M31 28 Q54 34 91 24 L95 33 Q61 46 30 37Z','#8f4e49'),
        path('M37 30 Q56 35 86 27','none','#c77259',2.5),
        path('M15 34 Q24 34 31 34 Q62 40 91 30 Q101 27 109 31 Q115 34 110 38 '
             'Q103 44 86 48 Q51 54 24 47 Q13 44 10 40 Q8 36 15 34Z','#dfad5f',INK,3.6),
        path('M14 37 Q23 38 30 39 Q60 44 91 34 Q102 30 108 34 Q102 39 84 41 Q49 49 21 42Z','#f4d083'),
        path('M26 46 Q52 52 86 45 Q101 41 105 39','none','#a96848',2.3),
        path('M37 18 L42 17 M71 15 Q76 14 80 18 M45 42 L52 43','none','#b87b48',2.2),
    ])


def head(bite, snooze=False, variant='gardener'):
    jaw_inner = {
        'closed': 'M19 71 Q34 75 50 69 Q47 80 36 82 Q23 84 19 76Z',
        'part': 'M18 69 Q31 74 49 68 Q48 81 39 87 Q22 94 17 80 Q15 76 18 69Z',
        'wide': 'M16 65 Q29 71 51 64 Q52 76 44 87 Q32 98 21 91 Q10 85 13 74Z',
    }[bite]
    jaw_outline = {
        'closed': 'Q46 96 38 91 Q28 90 24 87 Q15 83 18 72',
        'part': 'Q43 98 33 96 Q22 95 17 89 Q11 83 15 71',
        'wide': 'Q45 102 34 101 Q17 101 11 91 Q6 84 11 73',
    }[bite]
    eye = ([path('M31 56 Q39 59 48 54','none',INK,3)] if snooze else [
        path('M30 53 Q37 48 47 51 Q51 53 50 58 Q47 64 39 64 Q32 64 30 59Z','#f5e9c1',INK,2.6),
        path('M31 53 Q39 47 48 51 L49 54 Q40 52 30 57Z','#a9c59e'),
        path('M30 56 Q38 51 49 53','none',INK,2.8),
        ellipse(37,58.5,2.9,3.6,INK),ellipse(36.3,57.5,1.0,1.15,'#fff2cf'),
    ])
    if variant == 'armored' and not snooze:
        eye = [path('M29 53 Q40 51 51 53 Q50 65 39 65 Q31 64 29 59Z','#f2e0b4',INK,2.7),
               path('M29 53 Q39 49 51 52 L51 55 L29 57Z','#9cb790'),
               path('M28 56 Q38 54 51 54','none',INK,3),ellipse(39,59.4,3.4,3.45,INK)]
    elif variant == 'runner' and not snooze:
        eye = [path('M30 54 Q38 46 49 50 Q53 55 47 62 Q42 67 34 63 Q29 60 30 54Z','#fff0c3',INK,2.7),
               path('M29 55 Q38 47 49 50','none',INK,2.8),ellipse(35.5,57,3.35,4.3,INK),
               ellipse(34.6,55.7,1.05,1.1,'#fff2cf')]
    teeth = ('M22 73 L31 74 L30 79 Q27 81 23 78Z' if bite == 'closed'
             else 'M19 71 L32 72 L29 80 Q24 81 21 78Z')
    tongue = [] if bite == 'closed' else [path('M20 85 Q27 80 38 84 Q34 91 27 90Z','#bc7469')]
    lower_light = {
        'closed': 'M25 84 Q36 87 49 81 Q45 90 37 89 Q29 88 25 84Z',
        'part': 'M21 90 Q32 95 44 87 Q40 95 33 94 Q26 94 21 90Z',
        'wide': 'M16 91 Q31 100 43 90 Q40 98 31 99 Q21 98 16 91Z',
    }[bite]
    return group([
        path('M28 44 Q35 31 57 32 Q78 32 89 46 Q101 60 90 77 Q82 89 69 90 '
             'L65 94 Q59 97 51 94 ' + jaw_outline +
             ' Q7 70 9 62 Q10 56 17 55 '
             'Q21 54 26 55 Q25 49 28 44Z' , '#79a58c', INK, 4),
        path('M31 45 Q48 38 66 41 Q77 46 74 54 Q64 57 60 66 Q58 76 49 81 Q39 77 27 73 '
             'Q19 73 14 68 Q10 63 18 60 L28 59 Q28 49 31 45Z','#acca9f'),
        path('M50 86 Q66 82 70 72 Q67 65 74 58 Q81 52 90 59 Q92 73 80 82 Q74 88 62 90 '
             'L60 94 L52 94Z','#5c8a78'),
        path(jaw_inner,'#513544'), *tongue, path(teeth,'#f7e7b8'), path(lower_light,'#a4c39c'),
        path('M15 62 Q20 57 26 59','none','#d5dda6',2.9),
        path('M33 48 Q39 45 46 47','none','#4d695e',2.7),
        *eye,
        path('M72 62 Q82 55 86 63 Q89 69 81 74 Q75 76 71 70','none','#416354',2.8),
        path('M76 66 Q81 62 82 67 L79 70','none','#9dbb93',2.4),
        path('M45 69 Q53 65 57 69 Q59 74 51 76 Q46 75 45 69Z','#c2947d'),
        path('M53 73 L55 72','none','#8c615f',1.8),
        hat() if variant == 'gardener' else helmet() if variant == 'armored' else forelock(),
    ])


# Travel is in world space. Only the planted sole, heel roll and lift change inside the cel.
# Tuple fields are (knee x/y, ankle x/y, sole roll angle). Legs are far then near.
WALK = [
    ('walk_0_contact', 155, (0, 1),(-1, 1,-5),'back',     ((90,151),(102,169),-4), ((65,151),(59,171),0)),
    ('walk_1_weight',  125,(-2, 3),(-2, 3,-3),'hang',     ((89,154),(101,168),-6), ((64,155),(67,171),0)),
    ('walk_2_pass',    110, (0,-1),(-1, 0,-1),'forward',  ((65,149),(85,162),-18),((76,152),(79,171),0)),
    ('walk_3_reach',   100, (0,-1),(-2, 0,-5),'forward',  ((58,149),(56,166),8),  ((81,153),(91,171),0)),
    ('walk_4_contact', 155,(-1, 0),(-2, 1,-5),'forward',  ((61,151),(60,170),0),  ((88,153),(101,168),-6)),
    ('walk_5_drag',    190, (0, 2),( 0, 2, 0),'hang',     ((70,153),(73,170),0),  ((89,155),(98,169),-4)),
    ('walk_6_pass',    120, (0,-1),( 0,-1,-3),'back',     ((80,153),(86,170),0),  ((65,149),(82,160),-12)),
    ('walk_7_reach',    95, (0,-1),(-3, 0,-5),'back',     ((86,151),(95,170),0),  ((56,149),(52,165),8)),
]

CHEW = [
    ('chew_0_brace',  135, (0,0),(-1, 0,-2), 'forward', 'closed'),
    ('chew_1_open',   110,(-2,1),(-4,-1,-7), 'reach',   'part'),
    ('chew_2_wide',   105,(-4,2),(-7,2,-14), 'reach',   'wide'),
    ('chew_3_bite',   135,(-2,2),(-7,5,-10), 'reach',   'closed'),
    ('chew_4_release',235,(-1,0),(-2,3,-4), 'forward', 'closed'),
]


def gardener(shift, head_pose, arm, far, near, bite='closed', snooze=False, pose='', variant='gardener'):
    x,y = shift
    hx,hy,a = head_pose
    far_hand = group([
        path('M58 92 Q63 89 71 94 L67 105 Q60 114 53 119 Q48 126 42 123 Q39 122 43 116 '
             'Q40 118 38 115 Q36 110 45 109 Q51 105 54 96Z','#557669',FAR_INK,3.4),
        path('M55 91 Q64 85 71 94 L69 102 Q65 108 54 106 Q51 98 55 91Z','#9b625f',FAR_INK,3.1),
    ], 'translate(-7 -8) rotate(7 58 95)' if arm == 'reach' else 'translate(7 5)')
    if variant == 'armored':
        body = [far_hand, coat(), hanger(arm), cuirass(arm)]
        head_art = group([head(bite,snooze,variant)],'translate(2 1) scale(1.02 1)')
        hip_far,hip_near = (80+x,136+y), (91+x,136+y)
    elif variant == 'runner':
        body = [group([scarf(arm),far_hand,runner_coat(),hanger(arm)],'rotate(-14 82 140) translate(3 -5)')]
        head_art = group([head(bite,snooze,variant)],'translate(-2 -4) scale(0.91 0.99)')
        hip_far,hip_near = (77+x,128+y), (84+x,128+y)
    else:
        body = [far_hand, coat(), hanger(arm)]
        head_art = head(bite,snooze)
        hip_far,hip_near = (79+x,132+y),(89+x,132+y)
    drawing = [
        leg(hip_far,*far,False,variant), leg(hip_near,*near,True,variant),
        group(body, f'translate({x} {y})'),
        group([head_art],f'translate({hx+x} {hy+y}) rotate({a} 64 79)'),
    ]
    if pose == 'defeat':
        drawing = [group(drawing, 'rotate(22 86 180)')]
    return svg(drawing)


def helmet():
    # A dented, blue enamel watering vessel with a rolled rim and copper repair.
    return group([
        path('M32 36 L33 20 Q40 10 58 12 L65 8 Q73 5 80 12 L90 16 Q99 24 99 34 '
             'L105 47 Q92 57 77 60 L67 55 L66 45 Q52 48 31 43 Q20 42 15 37 Q14 33 20 32Z','#5a8394',INK,4),
        path('M37 29 Q42 16 58 17 L66 13 Q75 10 81 17 Q93 19 95 31 Q79 36 70 34 '
             'L66 27 Q57 32 37 29Z','#a8c9c5'),
        path('M67 35 Q79 36 98 30 L102 46 Q92 53 78 57 L71 53 L70 42Z','#38586d'),
        path('M19 35 Q45 42 67 34 Q75 40 95 33 L98 39 Q80 45 71 41 L66 41 '
             'Q41 49 20 39Z','#c98f5e',INK,2.5),
        path('M26 37 Q46 42 63 37 M79 40 Q85 40 92 37','none','#f4cd88',2.7),
        path('M80 51 Q87 50 92 47','none','#83a2aa',2.2),
        ellipse(75.5,46,2.5,2.6,'#e1b676'),
    ])


def cuirass(arm):
    # The shoulder is oversized on purpose. Its one clean curve is the armor cue.
    plate = [
        path('M73 81 Q95 72 108 86 Q117 94 118 110 Q110 122 94 123 L79 120 Q69 110 69 99 Q70 89 73 81Z',
             '#648f9b',INK,4),
        path('M74 97 Q76 83 89 83 Q103 81 110 94 Q100 89 89 98 L74 106Z','#b2d1c8'),
        path('M92 98 Q101 90 110 96 Q112 101 114 110 Q106 117 94 118 L87 115Z','#38667a'),
        path('M75 107 Q94 117 115 106 L117 112 Q108 124 83 120 L78 117Z','#cc996b',INK,2.8),
        path('M84 116 Q100 119 109 113','none','#f2cc89',2.35),
        ellipse(90,106,2.8,3,'#f3cf8d'),
        path('M97 86 Q101 87 104 90','none','#6a999f',2.6),
    ]
    armor_waist = [
        path('M64 110 Q86 114 107 108 L111 132 Q106 140 94 144 Q77 145 66 138 L60 122Z',
             '#587982',INK,3.4),
        path('M64 118 Q83 126 107 115 M67 132 Q87 140 107 127','none','#91adab',2.6),
        path('M65 126 Q83 133 109 121','none','#345967',3),
        path('M83 126 L96 125 L98 134 L87 137Z','#c68d5d',INK,2.6),
    ]
    if arm == 'reach':
        brace = path('M42 104 Q53 104 61 99 Q66 96 70 99 Q73 104 70 109 Q60 113 44 112Z','#557f8f',INK,2.4)
        transform = 'rotate(12 86 99) translate(-5 -4)'
    elif arm == 'forward':
        brace = path('M55 121 Q62 114 69 112 L75 116 Q70 124 60 129Z','#557f8f',INK,2.4)
        transform = 'rotate(5 86 99) translate(-3 -2)'
    elif arm == 'back':
        brace = path('M90 121 L100 121 L102 128 L91 132Z','#557f8f',INK,2.4)
        transform = 'rotate(-5 86 99)'
    else:
        brace = path('M76 127 L82 117 L93 118 L87 131Z','#557f8f',INK,2.4)
        transform = ''
    return group([*armor_waist,brace,group(plate,transform)])


def forelock():
    return group([
        path('M26 51 Q21 44 24 34 Q28 25 39 23 Q33 20 27 23 Q32 15 44 15 Q48 8 60 7 '
             'Q66 8 67 13 Q76 8 84 13 Q89 18 86 23 Q101 24 100 31 Q96 37 88 36 '
             'Q96 40 98 45 Q87 53 75 47 Q70 52 64 51 L59 42 Q52 52 44 49 Q44 43 46 39 '
             'Q38 47 26 51Z','#536452',INK,3.7),
        path('M28 34 Q35 25 49 25 Q44 21 39 21 Q46 20 51 16 Q60 10 64 15 '
             'L61 22 Q73 17 83 19 L82 26 Q92 25 96 29 Q89 33 85 32 Q75 28 63 33 '
             'Q50 28 38 36 L30 44Z','#82956e'),
        path('M28 43 Q52 47 88 34 L91 40 Q65 53 31 50Z','#cd764f',INK,2.8),
        path('M31 44 Q57 46 80 39','none','#f4ac63',2.5),
    ])


def scarf(arm):
    if arm in ('forward','reach'):
        tail = 'M72 81 Q88 75 99 79 Q118 85 126 71 Q129 83 119 90 L126 91 Q110 104 96 93 Q85 90 75 95Z'
        seam = 'M85 84 Q109 92 121 82'
    elif arm == 'back':
        tail = 'M72 82 Q90 75 105 86 Q115 96 126 87 Q125 97 117 101 L122 105 Q111 110 101 104 Q90 94 75 96Z'
        seam = 'M87 86 Q106 101 120 94'
    else:
        tail = 'M72 82 Q91 72 106 77 Q118 82 125 78 Q125 88 117 91 L125 94 Q111 101 98 93 Q88 89 75 95Z'
        seam = 'M85 83 Q110 92 119 85'
    return group([
        path(tail,'#de8551',INK,3.7),path(seam,'none','#f6b671',3),
        path('M62 86 Q77 80 84 85 L86 93 Q78 101 66 96 L60 92Z','#e9a25e',INK,3.1),
        path('M66 90 Q74 90 82 87','none','#f8c67b',2.4),
    ])


def runner_coat():
    return group([
        path('M59 89 Q66 80 78 81 Q95 82 104 92 Q107 102 100 115 Q94 125 98 135 '
             'L78 144 L63 135 Q65 123 57 117 Q50 107 59 89Z','#616e84',INK,3.8),
        path('M63 91 Q71 83 82 87 Q69 98 70 112 L74 133 L64 131 Q67 121 60 114 Q56 107 63 91Z','#879a9f'),
        path('M70 101 Q76 94 83 96 L87 100 Q80 108 78 128 L70 133 Q67 118 70 101Z','#e9a25e',INK,2.8),
        path('M89 121 L97 118 L101 129 L95 139 L81 142 L79 132Z','#5b4e73',INK,3),
        path('M85 133 Q92 134 97 127','none','#b58b98',2.7),
    ])


# Different weight and stride lengths, while the shared planted pixel stays at the lane floor.
ARMOR_WALK = [
    ('walk_0_contact',185,(0,2),(0,2,-2),'back',    ((95,154),(106,168),-6),((61,156),(53,171),0)),
    ('walk_1_weight', 190,(-1,5),(0,4,-3),'hang',   ((95,156),(105,169),-1),((67,158),(63,171),0)),
    ('walk_2_pass',   130,(0,1),(0,1,-3),'hang',    ((74,152),(90,166),-10),((74,157),(77,171),0)),
    ('walk_3_reach',  122,(1,0),(1,1,-4),'forward',((64,152),(60,166),8),   ((83,157),(94,171),0)),
    ('walk_4_contact',190,(0,2),(0,2,-3),'forward', ((60,155),(52,170),0),   ((93,156),(104,168),-5)),
    ('walk_5_drag',   235,(0,4),(0,3,0),'hang',     ((67,158),(65,170),0),   ((93,157),(103,169),-3)),
    ('walk_6_pass',   145,(0,0),(0,2,-3),'back',    ((78,157),(80,170),0),   ((71,152),(86,163),-10)),
    ('walk_7_reach',  123,(0,1),(-1,0,-4),'back',   ((87,156),(97,170),0),   ((62,153),(54,166),8)),
]

RUNNER_WALK = [
    ('walk_0_contact',80,(-2,3),(-1,1,-11),'back',    ((91,151),(108,155),-18),((55,148),(46,171),0)),
    ('walk_1_weight', 72,(-3,6),(-3,0,-9),'hang',    ((86,148),(108,150),-25),((64,153),(59,171),0)),
    ('walk_2_pass',   70,(-2,0),(-3,-1,-13),'forward',((58,142),(76,151),-22),((72,148),(80,171),0)),
    ('walk_3_reach',  66,(-3,-1),(-2,0,-9),'reach',  ((51,145),(48,168),7),   ((84,149),(100,169),-4)),
    ('walk_4_contact',83,(-4,2),(-2,1,-12),'forward',((53,149),(45,170),0),   ((92,148),(110,154),-17)),
    ('walk_5_weight', 70,(-2,6),(-2,0,-9),'hang',    ((63,152),(60,170),0),   ((88,148),(109,148),-25)),
    ('walk_6_pass',   72,(-3,0),(-1,-1,-13),'back',  ((73,150),(80,170),0),   ((60,142),(76,150),-23)),
    ('walk_7_reach',  67,(-3,-1),(-2,-1,-9),'back',  ((88,149),(103,167),-6), ((49,144),(47,167),8)),
]
