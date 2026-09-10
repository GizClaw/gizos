#!/usr/bin/env python3
"""Original procedural chiptune candidates. WAV files are REVIEW outputs only.

No recordings or per-sample waveform arrays are inputs. The portable score
contains note/drum/sweep events and can drive a bounded streaming synthesizer.
This offline audition renderer is not a device performance implementation.
"""
import hashlib
import json
import math
import struct
import wave
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'review/retro-audio-v1'
RATE=16000
TRACKS={}

def song(key,title,bpm,beats,loop,description):
    track=dict(id=key,title=title,bpm=bpm,beats=beats,loop=loop,description=description,events=[])
    TRACKS[key]=track
    return track

def event(t,beat,length,note,voice='pulse',gain=.12,end_note=None):
    t['events'].append(dict(beat=round(beat,5),length=round(length,5),note=note,
                            voice=voice,gain=gain,end_note=end_note))

def phrase(t,start,notes,step=.5,voice='pulse',gain=.13,gate=.78):
    for i,n in enumerate(notes):
        if n is not None:event(t,start+i*step,step*gate,n,voice,gain)

def rhythm(t,bar,root,full=True):
    phrase(t,bar*4,[root,root+12,root+7,root+12,root,root+12,root+7,root+10],.5,'triangle',.14,.82)
    for beat in (0,2):event(t,bar*4+beat,.32,38,'kick',.20)
    for beat in (1,3):event(t,bar*4+beat,.23,0,'snare',.13 if full else .07)
    for i in range(8):event(t,bar*4+i*.5,.09,0,'hat',.034 if i%2 else .024)

# One original A-minor motif connects the search and combat arrangements.
pair=song('pairing','配对 · 星际信标',120,16,True,'轻快的定位脉冲与短琶音，留出等待空间。')
roots=[45,41,48,43]
for bar,root in enumerate(roots):
    phrase(pair,bar*4,[root,root+12,root+7,root+12],1,'triangle',.12,.72)
    chord=[root+24,root+31,root+36,root+31]
    phrase(pair,bar*4,chord,.5,'soft',.105,.62)
    phrase(pair,bar*4+2,[root+28,None,root+31,None],.5,'pulse',.07,.48)
    for b in (0,2):event(pair,bar*4+b,.12,0,'hat',.025)
phrase(pair,12,[76,79,81,79,76,74,71,76],.5,'pulse',.095,.65)

battle=song('battle','战斗 · 像素气流',150,32,True,'脉冲主旋律、三角波低音和噪声鼓，八小节完整循环。')
melodies=[
 [76,76,79,81,None,79,76,74], [72,76,77,79,77,None,76,72],
 [79,79,84,83,81,79,76,None], [74,76,79,76,74,71,74,None],
 [76,79,81,84,83,81,79,76], [77,81,84,81,79,77,76,72],
 [79,84,83,79,81,79,76,74], [74,71,74,76,79,76,74,71]]
for bar in range(8):
    root=roots[bar%4];rhythm(battle,bar,root-12)
    phrase(battle,bar*4,melodies[bar],.5,'pulse',.125,.70)
    phrase(battle,bar*4,[root+12,root+19,root+24,root+19]*4,.25,'soft',.033,.60)
    if bar in (3,7):
        for i in range(4):event(battle,bar*4+3+i*.25,.14,0,'snare',.075+.012*i)

victory=song('victory','胜利 · 星光加冕',132,12,False,'由短上行号角进入明亮大三和弦，一次播放后收尾。')
phrase(victory,0,[72,76,79,84,None,83,84,88],.5,'pulse',.14,.80)
phrase(victory,4,[86,84,79,81,83,84,None,None],.5,'pulse',.125,.85)
for b,notes in [(0,[48,60,64]),(4,[53,65,69]),(8,[48,64,67,72])]:
    for i,n in enumerate(notes):event(victory,b,3.5,n,'triangle' if i==0 else 'soft',.095 if i==0 else .048)
for b in (0,2,4,6,8):event(victory,b,.3,38,'kick',.13)
phrase(victory,8,[84,88,91,96],.5,'soft',.09,.8)
event(victory,10,1.85,84,'soft',.08)

lose=song('defeat','失败 · 熄灭的能量',96,8,False,'短下行旋律和低音落点，克制、可快速重开。')
phrase(lose,0,[81,79,76,74,72,71,69,None],.5,'soft',.145,.86)
for b,n in [(0,45),(2,41),(4,40)]:event(lose,b,1.8,n,'triangle',.17)
for n,g in [(57,.075),(60,.065),(64,.045)]:event(lose,4,3.6,n,'soft',g)
event(lose,0,.32,37,'kick',.13)
event(lose,4,.45,32,'kick',.12)

def sfx(key,title,duration,desc):return song(key,title,60,duration,False,desc)
a=sfx('charge','聚气',.62,'逐级上行的能量音，末端扫频收束。')
phrase(a,0,[57,64,69,76,81],.10,'pulse',.19,.72);event(a,.34,.25,69,'soft',.12,81)
a=sfx('wave','发波',.48,'从高处俯冲的激光扫频，带短噪声尾迹。')
event(a,0,.39,94,'pulse',.24,40);event(a,.02,.34,0,'noise',.12);event(a,0,.10,38,'kick',.18)
a=sfx('absorb','吸收',.65,'先下潜再吸入的双向扫频，与发波区分。')
event(a,0,.32,81,'triangle',.27,45);event(a,.27,.30,45,'soft',.20,88)
phrase(a,.12,[76,72,69,64,69,76],.075,'pulse',.065,.7)
a=sfx('guard','防御',.45,'清亮的金属护盾和低音锁定。')
for n,g in [(76,.15),(83,.12),(91,.07)]:event(a,0,.30,n,'soft',g)
event(a,0,.18,48,'triangle',.21);event(a,.015,.045,0,'hat',.09)
a=sfx('hurt','受击',.30,'短促的低频冲击与失真噪声。')
event(a,0,.24,0,'noise',.23);event(a,0,.22,51,'pulse',.18,30)
a=sfx('clash','对波碰撞',.46,'集中爆破后迅速衰减，不拖尾遮盖下一招。')
event(a,0,.30,0,'snare',.23);event(a,0,.34,44,'kick',.24)
event(a,.02,.25,86,'pulse',.10,59)
a=sfx('combo','三连击',.64,'三次递进冲击，最后一击更重。')
for i in range(3):
    event(a,i*.18,.20,45-i*3,'kick',.19+i*.04)
    event(a,i*.18,.13,0,'snare',.13+i*.025)
    event(a,i*.18,.12,81+i*3,'pulse',.12,60)
a=sfx('guard_break','破盾',.58,'玻璃感的碎裂音阶，下坠收尾。')
phrase(a,0,[95,89,83,77,71,65],.07,'pulse',.15,.65);event(a,.02,.37,0,'noise',.15)
a=sfx('select','切换技能',.10,'轻微短音，连续滑动时不刺耳。');event(a,0,.075,81,'soft',.17)
a=sfx('confirm','确认释放',.20,'短上行双音。');phrase(a,0,[76,88],.075,'pulse',.15,.8)
a=sfx('connected','配对成功',.48,'上行三音提示连接完成。');phrase(a,0,[72,79,84],.13,'soft',.20,.85)
a=sfx('countdown','倒计时',.13,'单次计时音，预留给最后三秒。');event(a,0,.10,79,'pulse',.13)


def blep(p,step):
    if p<step:
        x=p/step;return x+x-x*x-1
    if p>1-step:
        x=(p-1)/step;return x*x+x+x+1
    return 0


def render(track):
    count=round(track['beats']*60/track['bpm']*RATE)
    samples=[0.0]*count
    starts=[0]*(count+1);voices=0
    for ordinal,e in enumerate(track['events']):
        start=round(e['beat']*60/track['bpm']*RATE)
        length=max(1,round(e['length']*60/track['bpm']*RATE))
        starts[min(count,start)]+=1;starts[min(count,start+length)]-=1
        hz=440*2**((e['note']-69)/12)
        end=hz if e['end_note'] is None else 440*2**((e['end_note']-69)/12)
        phase=0.;previous=0.;low=0.;rng=(ordinal+1)*12347+137
        voice=e['voice'];attack=.003 if voice in ('kick','hat','snare','noise') else .006
        release=.03 if voice in ('pulse','triangle') else .075
        for j in range(length):
            t=j/RATE;p=j/max(1,length-1)
            freq=hz*(end/hz)**p
            if voice=='kick':freq=46+135*math.exp(-t*37)
            step=min(.42,freq/RATE)
            if voice in ('noise','hat','snare'):
                rng=(1664525*rng+1013904223)&0xffffffff
                white=(rng/2147483648)-1
                low+=.22*(white-low)
                value=white-low if voice=='hat' else white*.72+low*.28
                if voice=='snare':value=value*.68+math.sin(2*math.pi*180*t)*.32
                value*=math.exp(-t*(38 if voice=='hat' else 15))
            elif voice=='pulse':
                duty=.25;value=(1 if phase<duty else -1)+blep(phase,step)-blep((phase-duty)%1,step)
                value+=.5 # remove 25% pulse DC; final mix also runs a DC blocker
                value*=.56
            elif voice=='triangle':value=(1-4*abs(phase-.5))*.85
            elif voice=='soft':value=math.sin(2*math.pi*phase)*.8+math.sin(6*math.pi*phase)*.12
            else:value=math.sin(2*math.pi*phase)*math.exp(-t*15)
            phase=(phase+step)%1
            env=min(1,j/max(1,attack*RATE),(length-1-j)/max(1,release*RATE))
            if voice in ('pulse','soft'):env*=.75+.25*math.exp(-t*12)
            at=start+j
            if at<count:samples[at]+=value*env*e['gain']
    # Fixed master level and gentle DC removal; no per-track loudness inflation.
    old=filtered=0.
    for i,s in enumerate(samples):
        filtered=s-old+.995*filtered;old=s;samples[i]=filtered*.95
    # Both loop ends are at silence; wrap to zero rather than a DC step.
    for i in range(min(96,count)):
        samples[i]*=i/96;samples[-1-i]*=i/96
    active=max_active=0
    for delta in starts:active+=delta;max_active=max(active,max_active)
    return samples,max_active


def save(key,samples):
    peak=max(map(abs,samples),default=0)
    assert peak<.98,(key,'insufficient mix headroom',peak)
    pcm=struct.pack('<'+'h'*len(samples),*[round(v*32767) for v in samples])
    with wave.open(str(OUT/(key+'.wav')),'wb') as f:
        f.setparams((1,2,RATE,0,'NONE','not compressed'));f.writeframes(pcm)
    return dict(file=key+'.wav',seconds=len(samples)/RATE,peak_dbfs=round(20*math.log10(max(peak,1e-9)),2),
                rms_dbfs=round(20*math.log10(max(math.sqrt(sum(x*x for x in samples)/max(1,len(samples))),1e-9)),2),
                pcm_bytes=len(pcm),sha256=hashlib.sha256(pcm).hexdigest(),clipped_samples=sum(abs(v)>=1 for v in samples))


def lua(value):
    if value is None:return 'nil'
    if isinstance(value,bool):return 'true' if value else 'false'
    if isinstance(value,(float,int)):return str(value)
    if isinstance(value,str):return json.dumps(value,ensure_ascii=False)
    if isinstance(value,list):return '{'+','.join(lua(v) for v in value)+'}'
    return '{'+','.join('['+lua(k)+']='+lua(v) for k,v in value.items())+'}'


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    score={'version':1,'sample_rate':RATE,'authoring':'original procedural composition; no sampled audio',
           'tracks':list(TRACKS.values())}
    (OUT/'score.json').write_text(json.dumps(score,ensure_ascii=False,indent=2)+'\n')
    compact=[]
    for t in TRACKS.values():
        compact.append(dict(id=t['id'],bpm=t['bpm'],beats=t['beats'],loop=t['loop'],
            events=[[e['beat'],e['length'],e['note'],e['voice'],e['gain'],e['end_note']] for e in t['events']]))
    (OUT/'score.lua').write_text('-- Review score only; no PCM samples. Event: beat,duration,note,voice,gain,end_note.\nreturn '+lua(compact)+'\n')
    rendered={};report=[]
    for t in TRACKS.values():
        rendered[t['id']],poly=render(t)
        report.append(dict(id=t['id'],title=t['title'],description=t['description'],loop=t['loop'],
                           bpm=t['bpm'],event_count=len(t['events']),max_polyphony=poly,**save(t['id'],rendered[t['id']])))
    # Context preview: search -> connect -> combat with effects -> victory.
    mix=[];segments=[]
    for key in ['pairing','battle','victory']:
        segments.append(dict(id=key,start_seconds=len(mix)/RATE));mix+=rendered[key]
    cues=[('connected',7.3),('charge',9),('guard',10.5),('wave',12),('clash',12.25),
          ('absorb',14),('wave',16),('guard_break',16.2),('combo',18),('hurt',18.4)]
    for key,time in cues:
        start=round(time*RATE)
        for i,v in enumerate(rendered[key]):
            if start+i<len(mix):mix[start+i]=mix[start+i]*.58+v*.82
    sequence=[]
    for key in list(TRACKS)[4:]:sequence+=rendered[key]+[0.0]*round(.35*RATE)
    result={'status':'awaiting-user-review','format':'16 kHz mono S16LE','tracks':report,
            'context_mix':{**save('context-mix',mix),'segments':segments,'cues':cues},
            'sfx_sequence':save('sfx-sequence',sequence),
            'score_lua_bytes':(OUT/'score.lua').stat().st_size,
            'renderer_source_bytes':Path(__file__).stat().st_size,
            'firmware_integration':False,'device_cpu_and_memory_verified':False,
            'note':'WAVs are audition exports only; compact score and future streaming synth are the firmware inputs.'}
    (OUT/'manifest.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('tracks',)},ensure_ascii=False,indent=2))

if __name__=='__main__':main()
