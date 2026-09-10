#!/usr/bin/env python3
"""Capture actual desktop RGB565 output and gate vector components, frame by frame.

The denominator is the union of nonempty component pixels, never the whole
black screen. Full-scene probes use explicit component ROIs. Reports retain
every frame's MAE, maximum, source hashes, timing and native logs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[2]
REPO=ROOT.parents[3]
BIN=REPO/'bazel-out/darwin_arm64-opt/bin/projects/example/targets/cc_binary/lua-qi-duel'
CASES=[]
def case(name,component,args,time=0,roi=None):CASES.append(dict(name=name,component=component,args=args,time=time,roi=roi))
case('hud','hud',['--layer=hud'])
for fx in ['player-down','player-up','enemy-down','enemy-up']:case('hud-'+fx,'hud',['--layer=hud','--health-fx='+fx])
case('carousel-frame','carousel',['--layer=carousel-frame'])
for qi in range(6):case('charge-'+str(qi),'carousel',['--layer=charge-cells','--qi='+str(qi)])
for fx in ['up','down']:case('charge-'+fx,'carousel',['--layer=charge-cells','--qi=3','--charge-fx='+fx])
for action in ['charge','wave','absorb','guard']:
 for actor,layer in [('player','hand-left'),('opponent','opponent')]:
  case(action+'-'+actor,'actions',['--layer='+layer,'--action='+action,'--actor='+actor],350)
for clash in ['equal','player-combo','enemy-combo','both-combo']:case('clash-'+clash,'clash',['--clash='+clash],420)
for result in ['win','lose']:case('result-'+result,'settlement',['--result='+result],640)
for impact in ['combo','armor-break']:case('impact-'+impact,'impact',['--layer=impact','--impact='+impact],120)
for layer in ['walls','wheel']:case(layer,'arena',['--layer='+layer],300)
for digit,t in [(3,0),(2,1100),(1,2100)]:
 case('countdown-'+str(digit),'countdown',['--game'],t,{'amoled':[166,112,36,36],'h106':[106,39,28,28]})

def main():
 p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--frames',type=int,default=1)
 p.add_argument('--only',nargs='*');p.add_argument('--sequence',action='store_true');p.add_argument('--reuse',action='store_true')
 p.add_argument('--baseline',type=Path,help='Verified original PNG sequence/report from an earlier reference-resource build')
 opt=p.parse_args()
 if not 1<=opt.frames<=300:p.error("--frames must be 1..300")
 unknown=set(opt.only or [])-{c["name"] for c in CASES}
 if unknown:p.error("unknown component cases: "+", ".join(sorted(unknown)))
 out=Path(opt.output).resolve();out.mkdir(parents=True,exist_ok=True)
 manifest={'pack_sha256':hashlib.sha256((ROOT/'assets/vector/components/components.h2vp').read_bytes()).hexdigest(),
           'executables':{n:hashlib.sha256((BIN/n).read_bytes()).hexdigest() for n in ['example-lua-qi-duel','example-lua-qi-duel-h106']},
           'frames':opt.frames,'sequence':opt.sequence}
 manifest_path=out/'capture-manifest.json'
 baseline_report=None
 if opt.baseline:
  baseline_report=json.loads((opt.baseline/'report.json').read_text())
  manifest['baseline_report_sha256']=hashlib.sha256((opt.baseline/'report.json').read_bytes()).hexdigest()
 if opt.reuse:
  if not manifest_path.exists() or json.loads(manifest_path.read_text())!=manifest:
   raise RuntimeError('Refusing to reuse captures from an unverified or different build')
 manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
 report={'metric':'RGB MAE over union of component pixels > 3/255, after optional ROI; RGB565 native captures','threshold':30,'step_ms':33,'cases':[]}
 for c in CASES:
  if opt.only and c['name'] not in opt.only:continue
  for screen in ['amoled','h106']:
   executable=BIN/('example-lua-qi-duel'+('-h106' if screen=='h106' else ''))
   start=0 if opt.sequence and not c['name'].startswith('countdown') else c['time']
   images={}
   for mode in ['reference',c['component']]:
    prefix=out/(screen+'-'+c['name']+'-'+mode)
    names=[Path(str(prefix)+(f'-{i:03}' if opt.frames>1 else '')+'.png') for i in range(opt.frames)]
    if mode=='reference' and opt.baseline:
     source=next(v for v in baseline_report['cases'] if v['name']==c['name'] and v['screen']==screen)
     if [v['time_ms'] for v in source['frames']]!=[start+i*33 for i in range(opt.frames)]:
      raise RuntimeError('Reference timestamps do not match this probe')
     for name in names:shutil.copyfile(opt.baseline/name.name,name)
    elif not(opt.reuse and all(n.exists() for n in names)):
     cmd=[str(executable),*c['args'],'--time-ms='+str(start),'--draw-component='+mode,'--capture='+str(prefix)+('' if opt.frames>1 else '.ppm'),'--capture-frames='+str(opt.frames)]
     with Path(str(prefix)+'.log').open('w') as log:
      result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=max(45,opt.frames*2),env={**os.environ,"SDL_VIDEODRIVER":"dummy"})
     if result.returncode:raise RuntimeError(f'capture failed: {prefix}, exit {result.returncode}')
     for name in names:
      ppm=name.with_suffix('.ppm');Image.open(ppm).save(name);ppm.unlink()
    images[mode]=names
   rows=[]
   for i,(a,b) in enumerate(zip(*images.values())):
    ref=np.array(Image.open(a).convert('RGB'),dtype=np.int16);candidate=np.array(Image.open(b).convert('RGB'),dtype=np.int16)
    if c['roi']:
     x,y,w,h=c['roi'][screen];ref=ref[y:y+h,x:x+w];candidate=candidate[y:y+h,x:x+w]
    support=np.maximum(ref.max(axis=2),candidate.max(axis=2))>3
    error=np.abs(ref-candidate);mae=float(error[support].mean()) if support.any() else 0
    rows.append({'time_ms':start+i*33,'mae':round(mae,4),'pixels':int(support.sum())})
   maximum=max(v['mae'] for v in rows);mean=float(np.mean([v['mae'] for v in rows]))
   pose_exception=c['name']=='absorb-player'
   record=dict(pose_exception=pose_exception,component=c['component'],name=c['name'],screen=screen,mean=round(mean,4),maximum=maximum,passed=maximum<30 and any(v['pixels'] for v in rows),frames=rows)
   report['cases'].append(record)
   (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
   print(screen,c['name'],f'mean={mean:.2f} max={maximum:.2f}', 'POSE_CHANGE' if pose_exception else 'PASS' if record['passed'] else 'FAIL',flush=True)
 report['passed']=all(c['passed'] for c in report['cases'] if not c['pose_exception'])
 report['frame_pairs']=sum(len(c['frames']) for c in report['cases'])
 report['vector_pack_sha256']=hashlib.sha256((ROOT/'assets/vector/components/components.h2vp').read_bytes()).hexdigest()
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 if not report['passed']:raise SystemExit(1)

if __name__=='__main__':main()
