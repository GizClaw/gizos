#!/usr/bin/env python3
"""Exercise the SDL event queue, H106 inverse mapping and real Lua input loop.

Unlike --check/device-bench this does not call game gesture handlers directly.
It does not claim to test macOS hardware mouse delivery or H106 firmware input.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from PIL import Image, ImageDraw

p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
run = subprocess.run([
    str(a.binary.resolve()), '--scene=desktop-input-check', '--weather=sunny',
    '--record-prefix=' + str(a.out.resolve() / 'frame-'), '--record-frames=100'],
    env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'}, capture_output=True, text=True, timeout=60)
log = run.stdout + run.stderr
(a.out / 'run.log').write_text(log)
expected = ['initial_rods', 'return_sea', 'click_cast', 'splash_wait', 'open_rods',
            'change_rod', 'return_without_reel', 'missing_reel_routes', 'equip_reel',
            'back_rods', 'back_sea', 'recast', 'second_splash']
observed = re.findall(r'DESKTOP_INPUT PASS (\w+)', log)
casts = re.findall(r'FISHING_CAST_START ([^\n]+)', log)
landed = re.findall(r'FISHING_LANDED ([\d.]+)m', log)
complete = (run.returncode == 0 and observed == expected and len(casts) == 2
            and len(landed) == 2 and 'DESKTOP_INPUT DONE PASS' in log
            and 'terminal state=4' not in log)
report = dict(complete=complete, source='SDL queue -> H106 PAL -> Lua touch loop',
              steps=observed, casts=casts, landed_m=[float(n) for n in landed])
(a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
assert complete, log
sheet = Image.new('RGB', (750, 556), '#20242b')
labels = ImageDraw.Draw(sheet)
for i, (frame, title) in enumerate([(4, 'Sea / reel visible'), (6, 'First cast'),
                                   (12, 'Waiting'), (20, 'Missing reel warning'),
                                   (30, 'Second cast'), (39, 'Second wait')]):
    image = Image.open(a.out / f'frame-{frame:05d}.ppm').convert('RGB')
    assert image.size == (240, 240)
    x, y = 5 + (i % 3) * 250, 5 + (i // 3) * 278
    labels.text((x, y), title, fill='white')
    sheet.paste(image, (x, y + 22))
sheet.save(a.out / 'desktop-input.png')
print('PASS 13 SDL checkpoints, two casts and two splashes:', a.out / 'report.json')
