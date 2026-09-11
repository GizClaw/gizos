local tracks={};for _,t in ipairs(Score) do tracks[t.id]=t end
local approved={
    ["pairing"]=216851495,
    ["battle"]=2125397759,
    ["victory"]=3005328122,
    ["defeat"]=1383743313,
    ["charge"]=1210077247,
    ["wave"]=4157570084,
    ["absorb"]=1065921020,
    ["guard"]=1649712518,
    ["hurt"]=4241290095,
    ["clash"]=2970745703,
    ["combo"]=2012122539,
    ["guard_break"]=3451214437,
    ["select"]=722183864,
    ["confirm"]=2348166504,
    ["connected"]=859448266,
    ["countdown"]=3362021759,
}
local function fingerprint(pcm)
    local h=2166136261;for i=1,#pcm do h=((h~pcm:byte(i))*16777619)&0xffffffff end;return h
end
-- Chunking and retry boundaries cannot alter the waveform.
for _,t in ipairs(Score) do
    local a,b=Retro.stream(t),Retro.stream(t)
    local whole=a.render(2048);local split={}
    for _,n in ipairs({1,17,320,511,1199}) do split[#split+1]=b.render(n) end
    assert(whole==table.concat(split),'chunk-dependent synth: '..t.id)
    assert(fingerprint(whole)==approved[t.id],'review waveform differs: '..t.id)
end
local full_score={
    ["pairing"]={128000,2356387753},
    ["battle"]={204800,401455342},
    ["victory"]={87273,81032285},
    ["defeat"]={80000,985641039},
    ["charge"]={9920,466303749},
    ["wave"]={7680,3020986267},
    ["absorb"]={10400,404567046},
    ["guard"]={7200,1568567296},
    ["hurt"]={4800,3482533002},
    ["clash"]={7360,37204448},
    ["combo"]={10240,3110366904},
    ["guard_break"]={9280,3032391359},
    ["select"]={1600,4075755192},
    ["confirm"]={3200,1491301792},
    ["connected"]={7680,1468743421},
    ["countdown"]={2080,4156913791},
}
for _,t in ipairs(Score) do
    local v=Retro.stream(t);local left,h=table.unpack(full_score[t.id]);h=2166136261
    while left>0 do
        local n=math.min(left,320);local pcm=v.render(n);left=left-n
        for i=1,#pcm do h=((h~pcm:byte(i))*16777619)&0xffffffff end
    end
    assert(h==full_score[t.id][2],'full review waveform differs: '..t.id)
end
local loop=Retro.stream(tracks.pairing)
local first=loop.render(320)
for _=1,399 do loop.render(320) end
assert(loop.render(320)==first,'loop phase / filter did not reset')
local once=Retro.stream(tracks.defeat)
for _=1,250 do once.render(320) end
assert(once.finished() and once.render(320)==string.rep('\0',640),'one-shot repeated')
local fake={};local outputs={};local busy=true
function fake.new_output()
    local out={accepted=0,closed=false}
    function out:info() return {frame_samples=320} end
    function out:write(pcm)
        assert(not self.closed and #pcm<=640 and #pcm%2==0)
        if busy then busy=false;self.accepted=self.accepted+2;return nil,'audio output: busy',2 end
        busy=true;self.accepted=self.accepted+#pcm;return true
    end
    function out:close() self.closed=true end
    outputs[#outputs+1]=out;return out
end
-- Opening a game does not allocate unused device output queues.
local game=Retro.new(fake,Score)
assert(#outputs==0,'eager unused audio tracks')
game.bgm.update('pairing',0)
assert(#outputs==1,'BGM must use exactly one output')
game.bgm.update('pairing',33)
assert(#outputs==1,'BGM output was not reused')
-- Keep only a compact event schedule, rather than expanded oscillator tables
-- for every note of the longest score. This bound includes its Lua overhead.
collectgarbage('collect')
local schedule_before=collectgarbage('count')
local schedule=Retro.stream(tracks.battle)
collectgarbage('collect')
assert(collectgarbage('count')-schedule_before<64,'oversized score schedule')
assert(#schedule.render(1)==2)
schedule=nil
collectgarbage('collect');local before=collectgarbage('count')
for t=0,9000,33 do
    game.bgm.update(t<3000 and 'pairing' or t<6500 and 'battle' or 'victory',t)
    if t%330==0 then game.play({'wave','guard','hurt'},t) end
    game.update(t)
end
collectgarbage('collect');assert(collectgarbage('count')-before<150,'unbounded retained audio state')
game.close();for _,out in ipairs(outputs) do assert(out.closed and out.accepted>0) end
local count=#outputs
game.bgm.update('defeat',10000);game.cue('select',10000);game.update(10000)
assert(#outputs==count,'closed output was reopened')
-- A failed lazy output must not block all later cue slots.
local attempts=0
local flaky={new_output=function()
    attempts=attempts+1
    if attempts==1 then return nil,'unavailable' end
    return fake.new_output()
end}
local retry=Retro.new(flaky,Score)
retry.cue('select',0);retry.cue('select',1);retry.update(1)
assert(attempts==2 and outputs[#outputs].accepted>0,'failed output blocked later slots')
retry.close()
assert(Retro.scene(nil,{kind='lose',started=10},10)=='defeat')
return 'ok'
