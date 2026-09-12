#!/usr/bin/env python3
"""Bounded Runtime-event integration runner; desktop results are not device results."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--mode', choices=('desktop', 'monitor', 'upgrade', 'restart'), required=True)
parser.add_argument('--port')
parser.add_argument('--timeout', type=int, default=600)
parser.add_argument('--target-fps', type=float, default=25, help='H106 real-device active-frame target')
parser.add_argument('--vm-kib', type=int, default=4096, help='desktop diagnostic Lua budget')
args = parser.parse_args()
if not 1 <= args.target_fps <= 120:
    parser.error('--target-fps must be between 1 and 120')
if args.mode != 'desktop' and not args.port:
    parser.error('device mode requires an explicit --port')
args.out.mkdir(parents=True, exist_ok=False)
command = [str(args.binary.resolve())]
environment = dict(os.environ)
if args.mode == 'desktop':
    command += ['--button-check', '--profile=amoled', f'--vm-kib={args.vm_kib}']
    environment['SDL_VIDEODRIVER'] = 'dummy'
else:
    command += ['--port', args.port, '--no-ble', '--wait-timeout', '12', '--read-timeout', '12']
    if args.mode in ('upgrade', 'restart'):
        command += ['reboot', 'upgrade' if args.mode == 'upgrade' else 'app', '--monitor']
    else:
        command += ['monitor']
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment)
selector = selectors.DefaultSelector()
selector.register(process.stdout, selectors.EVENT_READ)
started = time.monotonic()
last_output = started
app_ready = args.mode == 'desktop'
buffer = b''
checkpoints = []
performance = []
animations = []
tab_animations = []
phases = []
quality_failures = []
failure = None
done = False
ansi = re.compile(r'\x1b\[[0-9;]*m')
try:
    with (args.out / 'run.log').open('xb') as log:
        while time.monotonic() - started < args.timeout and not done and not failure:
            if not selector.select(1):
                if process.poll() is not None:
                    break
                if app_ready and time.monotonic() - last_output > 45:
                    failure = 'No diagnostic output for 45 seconds after App startup; run incomplete'
                    break
                continue
            chunk = os.read(process.stdout.fileno(), 65536)
            if not chunk:
                break
            last_output = time.monotonic()
            log.write(chunk)
            log.flush()
            buffer += chunk
            while b'\n' in buffer:
                raw, buffer = buffer.split(b'\n', 1)
                line = ansi.sub('', raw.decode('utf-8', errors='replace')).strip()
                if 'FISHING_H106_READY rc=0' in line:
                    app_ready = True
                if any(token in line for token in ('H106_AUTO', 'FISHING_H106_', 'H106_UI_ANIMATION', 'H106_TAB_ANIMATION', 'H106_PHASE_PERF')):
                    print(line, flush=True)
                if 'H106_AUTO PASS ' in line:
                    checkpoints.append(line.split('H106_AUTO PASS ', 1)[1])
                if 'H106_AUTO CHECK_FAIL ' in line:
                    quality_failures.append(line.split('H106_AUTO CHECK_FAIL ', 1)[1])
                if any(marker in line for marker in ('H106_AUTO DONE PASS physical_input_restored',
                                                     'H106_AUTO DONE FAIL physical_input_restored')):
                    done = True
                    if 'H106_AUTO DONE FAIL' in line:
                        failure = 'Device reported nonterminal check failures'
                if 'FISHING_PERF ' in line:
                    performance.append(line.split('FISHING_PERF ', 1)[1])
                if 'H106_UI_ANIMATION ' in line:
                    fields = dict(re.findall(r'(\w+)=([\d.]+)', line.split('H106_UI_ANIMATION ', 1)[1]))
                    animations.append({key: float(value) for key, value in fields.items()})
                if 'H106_TAB_ANIMATION ' in line:
                    fields = dict(re.findall(r'(\w+)=([\d.]+)', line.split('H106_TAB_ANIMATION ', 1)[1]))
                    tab_animations.append({key: float(value) for key, value in fields.items()})
                if 'H106_PHASE_PERF ' in line:
                    fields = dict(re.findall(r'(\w+)=([\w.]+)', line.split('H106_PHASE_PERF ', 1)[1]))
                    phases.append({key: value if key == 'phase' else float(value) for key, value in fields.items()})
                if any(token in line for token in ('terminal state=4', 'Guru Meditation', 'H106_AUTO timeout:', 'FISHING_H106_EXIT')):
                    failure = line
                    print(line, flush=True)
                    break
finally:
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
    selector.close()
required = {'rods_selection_auto_scroll', 'page_1', 'page_2', 'page_3', 'page_4',
            'bidirectional_page_cycle', 'record_tap_retrieve', 'charge_changes_actual_distance',
            'record_hook', 'left_right_rod_angle', 'fight_lift_settlement',
            'record_settlement_exit', 'bag_actual_catch'}
if not required.issubset(checkpoints) or len(checkpoints) != 15:
    failure = failure or 'Missing or duplicated checkpoints'
if not done:
    failure = failure or 'Process ended or timed out before completion'
if quality_failures:
    failure = failure or 'Nonterminal checks failed'
distances = {}
for checkpoint in checkpoints:
    match = re.fullmatch(r'cast_([12])_distance_([\d.]+)', checkpoint)
    if match:
        distances[match[1]] = float(match[2])
if not (distances.keys() == {'1', '2'} and distances['2'] > distances['1'] + 1):
    failure = failure or 'Missing or invalid charged cast distances'
expected_phases = {'aiming', 'casting', 'flight', 'waiting', 'fight', 'lifting', 'settlement',
                   'select_1', 'select_2', 'select_3', 'scroll_1', 'scroll_2', 'scroll_3',
                   'tabs_1_2', 'tabs_2_1', 'tabs_2_3', 'tabs_3_2', 'tabs_3_4', 'tabs_4_3',
                   'tabs_3_4_caught', 'tabs_4_3_caught'}
missing_phases = sorted(expected_phases - {p['phase'] for p in phases})
if done and missing_phases:
    failure = failure or 'Missing required phase measurements'
report = {'passed': done and failure is None, 'mode': args.mode, 'port': args.port,
          'target_fps': args.target_fps,
          'tab_animation_target_met': len(tab_animations) == 8 and all(a.get('fps', 0) >= args.target_fps for a in tab_animations),
          'selection_target_met': len(animations) == 24 and all(a.get('fps', 0) >= args.target_fps for a in animations),
          'all_active_phases_target_met': done and required.issubset(checkpoints) and len(checkpoints) == 15
              and not missing_phases and all(p.get('fps', 0) >= args.target_fps for p in phases),
          'completed': done, 'quality_failures': quality_failures,
          'desktop_vm_kib': args.vm_kib if args.mode == 'desktop' else None,
          'input_path': 'Runtime typed button events -> Lua callbacks',
          'fixture': '1.2 kg spotted sea bass encounter; real hook/fight/landing',
          'physical_switches_verified': False, 'elapsed_seconds': round(time.monotonic()-started, 2),
          'checkpoints': checkpoints, 'cast_depth_metres': distances,
          'performance_samples': performance, 'selection_animations': animations,
          'tab_animations': tab_animations, 'missing_phases': missing_phases,
          'selection_30fps_met': bool(animations) and all(a.get('fps', 0) >= 30 for a in animations),
          'active_phase_performance': phases,
          'all_active_phases_30fps_met': done and required.issubset(checkpoints) and len(checkpoints) == 15
              and not missing_phases and all(p.get('fps', 0) >= 30 for p in phases),
          'failure': failure}
(args.out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
print(json.dumps({key: report[key] for key in ('passed', 'mode', 'elapsed_seconds', 'failure')}))
raise SystemExit(0 if report['passed'] else 1)
