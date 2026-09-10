#!/usr/bin/env python3
"""Authored qi-duel vector artwork. No raster imports, tracing or embedded images.

Path geometry is authored in the reference's design coordinates. Gradients and
layered strokes express metal, glass and emission. SVG is a review/authoring
format; device path compilation and frame deadlines remain separate gates.
"""
from pathlib import Path
from math import sin, cos

OUT = Path(__file__).resolve().parents[2] / 'assets/vector'
DEFS = '''<defs>
<linearGradient id="metal" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#fcffff"/><stop offset=".22" stop-color="#8d98a8"/><stop offset=".37" stop-color="#131b28"/><stop offset=".52" stop-color="#323b48"/><stop offset=".73" stop-color="#dae4ee"/><stop offset="1" stop-color="#4a5362"/></linearGradient>
<linearGradient id="dark" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#718091"/><stop offset=".28" stop-color="#141b25"/><stop offset=".65" stop-color="#080d15"/><stop offset="1" stop-color="#657282"/></linearGradient>
<linearGradient id="glass" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#fff" stop-opacity=".95"/><stop offset=".28" stop-color="#d8e5f5" stop-opacity=".38"/><stop offset=".7" stop-color="#eff8ff" stop-opacity=".64"/><stop offset="1" stop-color="#fff" stop-opacity=".98"/></linearGradient>
<linearGradient id="light" x1="0" y1="0" x2="1" y2="0"><stop stop-color="#fff" stop-opacity="0"/><stop offset=".6" stop-color="#edf5ff" stop-opacity=".55"/><stop offset="1" stop-color="#fff"/></linearGradient>
<radialGradient id="halo"><stop stop-color="#edf5ff" stop-opacity=".72"/><stop offset=".35" stop-color="#dbe9ff" stop-opacity=".32"/><stop offset=".68" stop-color="#ccdfff" stop-opacity=".12"/><stop offset="1" stop-color="#d7e8ff" stop-opacity="0"/></radialGradient>
<radialGradient id="core"><stop stop-color="#fff"/><stop offset=".18" stop-color="#fff" stop-opacity=".95"/><stop offset=".48" stop-color="#eaf3ff" stop-opacity=".4"/><stop offset="1" stop-color="#dbe9ff" stop-opacity="0"/></radialGradient>
<linearGradient id="armor" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#303968"/><stop offset=".3" stop-color="#101b40"/><stop offset=".58" stop-color="#050a1d"/><stop offset="1" stop-color="#02040c"/></linearGradient>
<linearGradient id="purple" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#ffc9ff"/><stop offset=".25" stop-color="#b620ff"/><stop offset=".55" stop-color="#421791"/><stop offset="1" stop-color="#080a26"/></linearGradient>
<linearGradient id="plate" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#41497f"/><stop offset=".45" stop-color="#1c2b51"/><stop offset="1" stop-color="#040716"/></linearGradient>
</defs>'''

class Art:
    def __init__(self, name, w=160, h=160):
        self.name, self.w, self.h, self.items = name, w, h, []
    def raw(self, value): self.items.append(value)
    def path(self, d, fill='none', stroke=None, width=1, opacity=1):
        self.raw(f'<path d="{d}" fill="{fill}"'+(f' stroke="{stroke}" stroke-width="{width}"' if stroke else '')+f' opacity="{opacity}" stroke-linejoin="round" stroke-linecap="round"/>')
    def glow(self, d, width=1, color='#f4f9ff', strength=1):
        for extra, opacity in [(12,.025),(7,.055),(4,.1),(1.8,.23),(0,.92)]:
            self.path(d,stroke=color,width=width+extra,opacity=opacity*strength)
    def ellipse(self, x,y,rx,ry,fill,opacity=1):
        self.raw(f'<ellipse cx="{x}" cy="{y}" rx="{rx}" ry="{ry}" fill="{fill}" opacity="{opacity}"/>')
    def group(self, transform): self.raw(f'<g transform="{transform}">')
    def end(self): self.raw('</g>')
    def star(self,x,y,r=4,opacity=.8):
        self.path(f'M{x-r} {y} Q{x} {y-.4} {x} {y-r*1.7} Q{x+.5} {y} {x+r} {y} Q{x} {y+.5} {x} {y+r*1.7} Q{x-.4} {y} {x-r} {y}Z','#f8fcff',opacity=opacity)
    def save(self):
        OUT.mkdir(parents=True,exist_ok=True)
        (OUT/f'{self.name}.svg').write_text(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.w} {self.h}"><title>{self.name}: authored vector draft</title>{DEFS}'+''.join(self.items)+'</svg>\n')


def charge():
    a=Art('skill-charge');a.ellipse(81,81,76,76,'url(#halo)',.65)
    for angle in [0,90,180,270]:
        a.group(f'rotate({angle} 81 81)')
        a.glow('M91 25 A57 57 0 0 1 137 72',.6,'#f4f9ff',.8)
        a.glow('M91 19 A63 63 0 0 1 143 74',.4,strength=.6)
        a.path('M82 23 Q112 31 127 60 L121 64 Q106 41 91 37Z','url(#glass)','#e9f5ff',.8)
        a.path('M78 14 Q77 11 80 14 L85 22 86 32 Q95 42 99 47 L94 53 88 46 82 42Z','url(#metal)','#eaf2ff',1)
        a.path('M79 15 L82 40 94 51 96 48 85 33 83 21Z','url(#dark)')
        a.glow('M83 23 Q105 26 114 43 L108 45 Q103 34 87 31',2.4)
        a.path('M108 31 L116 36 124 51 128 53 119 58 117 45Z','url(#metal)','#d1dce9',.5)
        a.glow('M81 39 L82 65',1.4)
        a.path('M127 20 L119 34 121 28Z','#fff',opacity=.8)
        a.path('M109 16 L110 8 111 12Z','#fff',opacity=.8)
        a.end()
    a.glow('M81 51 A31 31 0 1 1 80.9 51',1.6)
    a.glow('M81 42 A40 40 0 1 1 80.9 42',.6,strength=.7)
    a.glow('M81 57 L101 84 81 109 62 84Z',1.8)
    a.path('M81 57 L101 84 81 109 62 84Z','url(#glass)','#f4faff',1)
    for d in ['M81 57 L70 84 81 109 92 84Z','M62 84 L81 71 101 84 81 95Z','M70 84 L81 71 92 84 81 95Z']:
        a.path(d,stroke='#fff',width=.6,opacity=.6)
    a.ellipse(81,81,18,18,'url(#core)',.4)
    for x,y,r in [(81,57,6),(101,84,2),(81,109,7),(62,84,2)]:a.star(x,y,r,1);a.ellipse(x,y,12,12,'url(#core)')
    for i in range(26):
        t=i*2.399;x=81+cos(t)*(29+(i%4)*4);y=81+sin(t)*(29+(i%4)*4)
        a.star(round(x,2),round(y,2),.6+(i%3)*.35,.5)
    a.save()


def wave():
    a=Art('skill-wave');a.ellipse(96,80,65,74,'url(#halo)',.78)
    for d in ['M46 20 C102 20 145 50 142 81 C141 113 103 136 58 140 C93 124 127 109 127 80 C127 54 88 31 46 20Z',
              'M76 21 C122 40 149 64 147 87 C145 108 127 127 106 134 C127 109 137 94 136 80 C134 54 110 36 76 21Z',
              'M40 37 C87 42 124 62 126 80 C126 103 82 125 41 126 C87 110 112 93 111 81 C111 67 83 51 40 37Z']:
        a.path(d,'url(#glass)');a.glow(d,.7)
    for d,w in [('M46 20 C103 23 142 49 142 80 C140 110 109 134 58 140',2.3),('M69 35 C112 43 125 63 126 80 C125 103 108 121 67 131',2.3),('M24 39 Q109 46 132 80 Q112 109 23 123',1.3)]:a.glow(d,w)
    for d in ['M8 70 L62 64 108 79 60 74Z','M6 85 L64 78 111 81 61 89Z','M21 97 L65 91 111 83 65 100Z',
              'M22 52 L65 53 114 77 72 63Z','M37 111 L65 98 111 84 77 108Z']:
        a.path(d,'url(#light)');a.glow(d,.55,strength=.6)
    for i in range(12):
        y=43+i*6.5;end=77+(y-80)*.18
        a.glow(f'M{10+(i%4)*8} {y:.1f} Q62 {(y+80)/2:.1f} 107 {end:.1f}',.35,strength=.28+(i%3)*.15)
    a.glow('M6 79 L112 79 M10 83 L112 81',1.5)
    for d in ['M32 51 L65 61 98 74 72 72Z','M52 56 L90 61 116 77 102 77 84 69Z','M31 109 L71 87 98 85 65 103Z','M55 104 L89 99 116 83 102 84 85 92Z']:
        a.path(d,'url(#metal)','#f4f8ff',.6)
    a.path('M32 51 L72 72 93 73 62 63Z','url(#dark)')
    a.path('M32 108 L72 88 94 86 62 99Z','url(#dark)')
    a.ellipse(104,80,13,13,'url(#core)');a.ellipse(104,80,7.5,7.5,'url(#glass)');a.glow('M104 71 A9 9 0 1 1 103.9 71',.8)
    for x,y,r in [(58,47,1.3),(59,103,1.3),(36,91,1.3),(82,114,1)]:a.star(x,y,r)
    a.save()


def absorb():
    a=Art('skill-absorb');a.ellipse(80,80,78,78,'url(#halo)',.62)
    for angle in range(0,360,45):
        a.group(f'rotate({angle} 80 80)')
        a.path('M79 81 C103 77 125 43 116 17 C131 45 126 64 107 76 C95 84 87 86 79 81Z','url(#glass)',opacity=.65)
        a.glow('M79 81 C103 77 125 43 116 17',1.15)
        a.glow('M83 84 C105 84 128 65 130 40',.65)
        a.glow('M82 84 C111 91 137 64 133 41',.5,strength=.5)
        a.path('M112 33 L117 27 119 36 116 48 111 54 113 42Z','url(#metal)','#e5eff9',.6)
        a.path('M122 55 L125 51 127 55 123 61 120 62Z','url(#dark)','#d3dfec',.4)
        a.end()
    for angle in [0,90,180,270]:
        a.group(f'rotate({angle} 80 80)')
        a.path('M73 1 L80 7 87 1 87 26 94 23 91 34 80 46 69 34 66 23 73 26Z','url(#metal)','#ecf4ff',.8)
        a.path('M75 5 L80 10 85 5 85 28 80 37 75 28Z','url(#glass)')
        a.glow('M80 8 L80 42 M70 29 L80 42 90 29',1.4)
        a.path('M68 25 L73 30 73 34 68 30Z','url(#dark)')
        a.path('M87 30 L92 25 91 31 87 35Z','url(#dark)')
        a.end()
    a.ellipse(80,82,13,13,'url(#core)',.7)
    a.glow('M79 81 C72 71 82 65 91 71 C106 82 98 96 85 97 C65 98 60 75 73 62',1.2)
    for x,y in [(51,58),(98,45),(123,107),(45,115),(23,32)]:a.star(x,y,1.5,.7)
    a.save()


def guard():
    a=Art('skill-guard');a.ellipse(80,83,73,78,'url(#halo)',.7)
    a.group('translate(0 -8)')
    shell='M80 12 L137 42 131 92 Q125 126 80 153 Q36 126 29 92 L23 42Z'
    a.path(shell,'url(#glass)','#eef5ff',1)
    a.glow('M80 12 L137 42 131 92 Q125 126 80 153 Q36 126 29 92 L23 42Z',1)
    a.path('M80 20 L129 46 121 91 Q115 123 80 143 Q44 121 38 91 L31 46Z','url(#dark)','#dae4ef',.7)
    a.path('M80 28 L120 50 113 92 Q106 119 80 135 Q53 118 47 92 L40 50Z','url(#glass)','#b6c4d7',.7)
    # Finite honeycomb cells are authored geometry, clipped inside the shield.
    a.raw('<defs><clipPath id="shieldclip"><path d="M80 29 L119 50 112 93 Q105 118 80 134 Q54 118 48 93 L41 50Z"/></clipPath></defs><g clip-path="url(#shieldclip)">')
    for row in range(18):
        for col in range(13):
            x=37+col*7+(row%2)*3.5;y=40+row*6
            a.path(f'M{x} {y-4} l3.5 2 v4 l-3.5 2 -3.5 -2 v-4Z',stroke='#e5f1ff',width=.32,opacity=.2)
    a.end()
    a.path('M47 24 L35 28 28 41 38 49 44 45 39 36 57 27Z','url(#metal)','#e8f2ff',.7)
    a.path('M113 24 L125 29 132 41 122 49 116 45 121 36 103 27Z','url(#metal)','#e8f2ff',.7)
    a.path('M80 23 L113 46 112 65 80 91 48 65 47 46Z','url(#glass)','#e5efff',1)
    a.path('M80 25 L84 32 83 81 109 47 108 63 80 89 51 62 51 50 77 80 77 32Z','url(#glass)')
    a.path('M47 70 L80 96 113 70 109 91 80 116 51 91Z','url(#glass)','#e5efff',1)
    a.path('M53 97 L80 121 107 97 99 115 80 132 62 115Z','url(#metal)','#e5efff',.65)
    a.glow('M80 19 L80 144 M48 66 L80 92 112 66 M51 92 L80 117 109 92',1.5)
    for x,y in [(49,104),(111,104)]:
        a.ellipse(x,y,3.5,3.5,'url(#metal)');a.ellipse(x,y,1.9,1.9,'#1b2431');a.ellipse(x-.4,y-.7,1.1,1.1,'#dae8f6')
    for d in ['M21 29 Q8 40 12 76 Q16 103 32 126 Q19 97 24 53 L28 41 25 29Z','M139 29 Q152 40 148 76 Q144 103 128 126 Q141 97 136 53 L132 41 135 29Z']:
        a.path(d,'url(#glass)');a.glow(d,1.3)
    a.star(80,15,4);a.end();a.save()

if __name__=='__main__':
    for draw in [charge,wave,absorb,guard]:draw()
