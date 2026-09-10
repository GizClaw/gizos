-- Streaming implementation of the reviewed retro-audio-v1 score.
-- Only note events and oscillator state persist. PCM is one bounded write frame.
local M={}
local RATE,TAU=16000,math.pi*2
local function round(x) return math.floor(x+.5) end
local function blep(p,d)
    if p<d then local x=p/d;return x+x-x*x-1 end
    if p>1-d then local x=(p-1)/d;return x*x+x+x+1 end
    return 0
end
function M.stream(track)
    local events={}
    for i,e in ipairs(track.events) do
        local noise=e[4]=='noise' or e[4]=='hat' or e[4]=='snare'
        local hz=440*2^((e[3]-69)/12)
        events[i]={start=round(e[1]*60/track.bpm*RATE),length=math.max(1,round(e[2]*60/track.bpm*RATE)),
            hz=hz,ratio=e[6] and 2^((e[6]-e[3])/12) or 1,kind=e[4],gain=e[5],seed=i*12347+137,
            attack=(noise or e[4]=='kick') and 48 or 96,release=(e[4]=='pulse' or e[4]=='triangle') and 480 or 1200}
    end
    table.sort(events,function(a,b) if a.start==b.start then return a.seed<b.seed end;return a.start<b.start end)
    local count=round(track.beats*60/track.bpm*RATE)
    local position,next_event,active,old,filtered=0,1,{},0,0
    local self={}
    function self.finished() return position>=count and not track.loop end
    function self.render(n)
        assert(n>=1 and n<=4096 and n%1==0,'invalid synth frame')
        local pcm={}
        for k=1,n do
            if position==count and track.loop then position,next_event,active,old,filtered=0,1,{},0,0 end
            local sum=0
            if position<count then
                while events[next_event] and events[next_event].start<=position do
                    local e=events[next_event];next_event=next_event+1
                    assert(#active<16,'score polyphony exceeds bounded synthesizer')
                    active[#active+1]={e=e,phase=0,low=0,rng=e.seed}
                end
                for i=#active,1,-1 do
                    local v=active[i];local e=v.e;local j=position-e.start
                    if j>=e.length then table.remove(active,i)
                    else
                        local t=j/RATE;local kind=e.kind
                        local freq=kind=='kick' and (46+135*math.exp(-t*37)) or e.hz*e.ratio^(j/math.max(1,e.length-1))
                        local step=math.min(.42,freq/RATE);local value
                        if kind=='noise' or kind=='hat' or kind=='snare' then
                            v.rng=(1664525*v.rng+1013904223)&0xffffffff
                            local white=v.rng/2147483648-1;v.low=v.low+.22*(white-v.low)
                            value=kind=='hat' and white-v.low or white*.72+v.low*.28
                            if kind=='snare' then value=value*.68+math.sin(TAU*180*t)*.32 end
                            value=value*math.exp(-t*(kind=='hat' and 38 or 15))
                        elseif kind=='pulse' then value=((v.phase<.25 and 1 or -1)+blep(v.phase,step)-blep((v.phase-.25)%1,step)+.5)*.56
                        elseif kind=='triangle' then value=(1-4*math.abs(v.phase-.5))*.85
                        elseif kind=='soft' then value=math.sin(TAU*v.phase)*.8+math.sin(TAU*3*v.phase)*.12
                        else value=math.sin(TAU*v.phase)*math.exp(-t*15) end
                        v.phase=(v.phase+step)%1
                        local env=math.min(1,j/e.attack,(e.length-1-j)/e.release)
                        if kind=='pulse' or kind=='soft' then env=env*(.75+.25*math.exp(-t*12)) end
                        sum=sum+value*env*e.gain
                    end
                end
                filtered=sum-old+.995*filtered;old=sum
                sum=filtered*.95*math.min(1,position/96,(count-1-position)/96)
                position=position+1
            end
            pcm[k]=string.pack('<i2',math.max(-32768,math.min(32767,round(sum*32767))))
        end
        return table.concat(pcm)
    end
    return self
end
function M.scene(intro,settlement,now)
    if settlement and now>=settlement.started then return settlement.kind=='win' and 'victory' or 'defeat' end
    return intro and intro.phase~='done' and 'pairing' or 'battle'
end
function M.new(audio,score)
    local tracks={};for _,t in ipairs(score) do tracks[t.id]=t end
    local slots={}
    for i=1,4 do
        local ok,out=pcall(audio.new_output,{sample_rate=RATE,channels=1,bits_per_sample=16,volume=i==1 and 48 or 55})
        if ok and out then
            local n=out:info().frame_samples or 320
            slots[i]={output=out,n=math.max(1,math.min(4096,n)),pending='',due=0,gain=1}
        end
    end
    local self={bgm_scene=M.scene}
    local current,target
    local function start(slot,kind,now)
        if not slot then return end
        slot.voice=tracks[kind] and M.stream(tracks[kind]) or nil
        slot.pending='';slot.due=now;slot.fade=nil;slot.gain=1
    end
    local function close(slot)
        if slot and slot.output then pcall(slot.output.close,slot.output);slot.output=nil;slot.voice=nil;slot.pending='' end
    end
    local function pump(slot,now)
        if not slot or not slot.output or not slot.voice then return end
        if slot.due<now-120 then slot.due=now end
        local budget=math.ceil(160*16/slot.n)
        while slot.due<now+80 and budget>0 do
            budget=budget-1
            if slot.pending=='' then
                if slot.voice.finished() then slot.voice=nil;return end
                local pcm=slot.voice.render(slot.n)
                if slot.fade or slot.gain<1 then
                    local parts={}
                    for j=1,#pcm,2 do
                        if slot.fade then
                            slot.gain=math.max(0,math.min(1,slot.gain+slot.fade))
                            if slot.gain==0 or slot.gain==1 then slot.fade=nil end
                        end
                        parts[#parts+1]=string.pack('<i2',round(string.unpack('<i2',pcm,j)*slot.gain))
                    end
                    pcm=table.concat(parts)
                end
                slot.pending=pcm
            end
            local called,ok,err,written=pcall(slot.output.write,slot.output,slot.pending)
            if not called or (not ok and err~='audio output: busy') then close(slot);return end
            local take=ok and #slot.pending or (written or 0)
            if take<0 or take>#slot.pending or take%2~=0 then close(slot);return end
            slot.pending=slot.pending:sub(take+1);slot.due=slot.due+take/32
            if not ok then return end
            if slot==slots[1] and slot.gain==0 and target then
                local due=slot.due;start(slot,target,due);current=target;slot.gain=0;slot.fade=1/4800
            end
        end
    end
    function self.stop() for i=2,4 do if slots[i] then slots[i].voice=nil;slots[i].pending='' end end end
    function self.close() for i=1,4 do close(slots[i]) end end
    function self.play(actions,now)
        self.stop();local seen={};local index=2
        for _,kind in ipairs(actions) do
            if tracks[kind] and not seen[kind] and index<=4 then start(slots[index],kind,now);index=index+1;seen[kind]=true end
        end
    end
    function self.cue(kind,now)
        if not tracks[kind] then return end
        for i=2,4 do if slots[i] and not slots[i].voice then start(slots[i],kind,now);return end end
    end
    function self.update(now) for i=2,4 do pump(slots[i],now) end end
    self.bgm={close=function() close(slots[1]) end,update=function(kind,now)
        local slot=slots[1];if not slot or not slot.output then return end
        if kind~=target then
            target=kind
            if slot.voice and slot.gain>0 then slot.fade=(kind==current and 1/4800 or -1/3200)
            else start(slot,kind,now);current=kind;slot.gain=0;slot.fade=1/4800 end
        end
        pump(slot,now)
    end}
    return self
end
return M
