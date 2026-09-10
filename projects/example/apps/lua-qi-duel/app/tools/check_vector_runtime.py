#!/usr/bin/env python3
"""Bounded native runtime probes for the final, bitmap-free desktop build."""
import json
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
BIN=ROOT.parents[3]/'bazel-out/darwin_arm64-opt/bin/projects/example/targets/cc_binary/lua-qi-duel'
OUT=ROOT/'validation/components-final-runtime'
OUT.mkdir(parents=True,exist_ok=True)
cases={'full':[], 'charge':['--action=charge','--actor=both'],
       'absorb':['--action=absorb','--actor=both'],
       'wave':['--action=wave','--actor=both'],'guard':['--action=guard','--actor=both'],
       'clash-player':['--clash=player-combo'],'clash-enemy':['--clash=enemy-combo'],
       'defeat':['--result=lose'],
       'clash':['--clash=both-combo'],'settlement':['--result=win']}
parser=argparse.ArgumentParser();parser.add_argument("--only",nargs="*");options=parser.parse_args()
unknown=set(options.only or [])-set(cases)
if unknown:parser.error("unknown runtime cases: "+", ".join(sorted(unknown)))
results=json.loads((OUT/"report.json").read_text()) if options.only and (OUT/"report.json").exists() else []
for screen in ['amoled','h106']:
 for case,args in cases.items():
  if options.only and case not in options.only:continue
  name=screen+'-'+case
  with tempfile.TemporaryDirectory(prefix='qi-vector-runtime-') as temporary:
   prefix=Path(temporary)/'frame'
   command=[str(BIN/('example-lua-qi-duel'+('-h106' if screen=='h106' else ''))),
            '--rehearsal','--time-ms=0','--capture-frames=181','--capture='+str(prefix),*args]
   with (OUT/(name+'.log')).open('w') as log:
    result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env={**os.environ,'SDL_VIDEODRIVER':'dummy'},timeout=60)
   if result.returncode:raise RuntimeError(name+' failed')
   frames=sorted(Path(temporary).glob('*.ppm'))
   if len(frames)!=181:raise RuntimeError(name+' missing frames')
   for i in [0,11,30,90,180]:Image.open(frames[i]).save(OUT/f'{name}-{i:03}.png')
  log=(OUT/(name+'.log')).read_text()
  fps=[float(v) for v in re.findall(r' fps=([0-9.]+)',log)]
  draw=[int(v) for v in re.findall(r' draw_ms=(\d+)',log)]
  memory=[int(v) for v in re.findall(r' memory=(\d+)',log)]
  row={'screen':screen,'case':case,'fps_samples':fps,'draw_ms_samples':draw,'end_lua_bytes':memory[-1] if memory else None,
       'passed':len(fps)>=5 and min(fps)>=29 and max(fps)<=31 and 'result=FAIL' not in log and 'state=3' not in log}
  results=[v for v in results if (v['screen'],v['case'])!=(screen,case)];results.append(row);(OUT/'report.json').write_text(json.dumps(results,indent=2)+'\n')
  print(name,'fps',fps,'draw',draw,'memory',row['end_lua_bytes'],'PASS' if row['passed'] else 'FAIL',flush=True)

if not all(row["passed"] for row in results):raise SystemExit(1)
