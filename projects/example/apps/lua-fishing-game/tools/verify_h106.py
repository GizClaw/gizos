#!/usr/bin/env python3
"""Capture the real H106 desktop output and make a layout review sheet."""
import argparse
import os
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
cases = [('Sea', 'idle', []), ('RODS', 'rods', []), ('REELS', 'reels', []),
         ('LURES', 'lures', []), ('RODS scrolled', 'rods', ['--scroll=144']),
         ('BAGS preview', 'fish-bag-preview', []),
         ('Long GAMAKATSU', 'rods', ['--rod=4']),
         ('Long MEGABASS', 'rods', ['--rod=8']),
         ('Settlement', 'deck-demo', ['--time-ms=2000'])]
sheet = Image.new('RGB', (760, ((len(cases) + 2) // 3) * 276), '#20242b')
labels = ImageDraw.Draw(sheet)
for i, (label, scene, extra) in enumerate(cases):
    target = a.out / (label.lower().replace(' ', '-') + '.ppm')
    result = subprocess.run(
        [str(a.binary.resolve()), '--scene=' + scene, '--time-ms=1000',
         '--capture=' + str(target.resolve())] + extra,
        env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
        capture_output=True, text=True, timeout=120)
    target.with_suffix('.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    frame = Image.open(target).convert('RGB')
    assert frame.size == (240, 240), frame.size
    frame.save(target.with_suffix('.png'))
    x, y = 10 + (i % 3) * 250, 8 + (i // 3) * 276
    labels.text((x, y), label, fill='white')
    sheet.paste(frame, (x, y + 20))
    print('PASS', label, flush=True)
sheet.save(a.out / 'h106-preview.png')
print(a.out / 'h106-preview.png')
