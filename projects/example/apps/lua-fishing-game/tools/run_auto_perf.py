#!/usr/bin/env python3
"""Collect bounded game-driven performance runs; never infer hardware FPS from host runs."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--mode', choices=('desktop', 'monitor', 'upgrade'), required=True)
p.add_argument('--port', default='/dev/tty.usbmodem1101')
p.add_argument('--timeout', type=int, default=540)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
command = [str(a.binary.resolve())]
if a.mode == 'desktop':
    command += ['--scene=device-bench', '--profile=amoled']
else:
    command += ['--port', a.port, '--no-ble', '--wait-timeout', '12', '--read-timeout', '12']
    command += ['reboot', 'upgrade', '--monitor'] if a.mode == 'upgrade' else ['monitor']
environment = {**os.environ}
if a.mode == 'desktop':
    environment['SDL_VIDEODRIVER'] = 'dummy'
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment)
selector = selectors.DefaultSelector()
selector.register(process.stdout, selectors.EVENT_READ)
started = time.monotonic()
buffer = b''
results = []
done = False
caught = False
failure = None
ansi = re.compile(r'\x1b\[[0-9;]*m')
try:
    with (a.out / 'serial.log').open('xb') as log:
        while time.monotonic() - started < a.timeout and not done:
            if not selector.select(1):
                if process.poll() is not None:
                    break
                continue
            data = os.read(process.stdout.fileno(), 65536)
            if not data:
                break
            log.write(data)
            log.flush()
            buffer += data
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                line = ansi.sub('', line.decode('utf-8', errors='replace')).strip()
                if 'FISHING_AUTO_' in line:
                    print(line, flush=True)
                if 'FISHING_AUTO_RESULT ' in line:
                    values = dict(re.findall(r'(\w+)=([^\s]+)', line.split('FISHING_AUTO_RESULT ', 1)[1]))
                    for key in values.keys() - {'name', 'mode'}:
                        values[key] = float(values[key])
                    results.append(values)
                if 'FISHING_AUTO_DONE PASS' in line:
                    done = True
                if 'FISHING_AUTO_CATCH PASS actual_hook_reel_lift_record count=1' in line:
                    caught = True
                if 'terminal state=4' in line or 'Guru Meditation' in line or 'automatic fight timed out' in line:
                    failure = line
                    break
            if failure:
                break
finally:
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
    selector.close()
expected = {(name, mode) for name in (
    'horizontal_rods_reels', 'horizontal_reels_lures', 'horizontal_lures_bags',
    'vertical_rods', 'vertical_reels', 'vertical_lures', 'vertical_bags')
    for mode in ('reference', 'optimized')}
expected |= {(name, 'optimized') for name in ('cast', 'waiting', 'fight', 'lifting',
    'settlement_animation', 'settlement_static', 'actual_catch_bag', 'settlement_repeat_optimized')}
expected.add(('settlement_repeat_reference', 'reference'))
observed = {(r['name'], r['mode']) for r in results}
if done and (not caught or observed != expected or len(results) != len(expected)):
    failure = 'Missing/duplicate phases or no verified unique actual catch'
    done = False
for r in results:
    if (not all(math.isfinite(v) and v >= 0 for k, v in r.items() if k not in ('name', 'mode'))
            or r.get('frames', 0) <= 0 or r.get('changed', 0) > r.get('frames', 0)):
        failure = 'Invalid performance sample'
        done = False
    if (r['name'].startswith(('horizontal_', 'vertical_'))
            and r.get('changed', 0) < max(2, r.get('frames', 0) * .1)):
        failure = 'Gesture did not animate the viewport: ' + r['name']
        done = False
report = {'source': 'desktop' if a.mode == 'desktop' else 'amoled_hardware',
          'port': None if a.mode == 'desktop' else a.port,
          'fixture': 'deterministic 1.2 kg bite; actual hook/reel/lift/unique catch path',
          'complete': done, 'error': failure, 'elapsed_s': time.monotonic() - started,
          'phases': results}
(a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f"{'PASS' if done else 'INCOMPLETE'}: {a.out / 'report.json'}", flush=True)
raise SystemExit(0 if done else 1)
