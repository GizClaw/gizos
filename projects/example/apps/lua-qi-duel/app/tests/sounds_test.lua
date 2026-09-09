local opened,closed,writes = 0,0,0
local busy,failed = false,false
local audio = {new_output=function(config)
    assert(config.sample_rate==16000 and config.volume<=45)
    opened=opened+1
    return {
        info=function() return {frame_samples=320} end,
        write=function(_,pcm)
            assert(#pcm==640)
            if failed then return nil,"audio output: write failed",0 end
            if busy then return nil,"audio output: busy",0 end
            writes=writes+1
            return true
        end,
        close=function() closed=closed+1 end,
    }
end}
local sfx=Sounds.new(audio,Clips)
assert(opened==3 and closed==0) -- Three reusable cue tracks are prewarmed once.
assert(#Clips.charge==32640 and #Clips.wave==38080)
assert(#Clips.absorb==28800 and #Clips.guard==27840)
-- Preview clips are dry; embedded clips align actual voice with skill windup.
for kind,lead in pairs({wave=450,absorb=330,guard=200,hurt=720}) do
    assert(Clips[kind]:sub(1,lead*32)==string.rep("\0",lead*32))
    assert(Clips[kind]:sub(lead*32+1):find("[^%z]"))
end
sfx.play({"charge","charge"},0)
assert(opened==3) -- Same skill is deduplicated, invalid/empty moves are silent.
sfx.update(0)
assert(writes==4)
busy=true
sfx.update(33)
local before=writes
busy=false
sfx.update(66)
assert(writes>before)
for now=99,1452,33 do sfx.update(now) end
assert(writes==51 and closed==0) -- Entire clip finishes without closing its reusable track.
sfx.play({"invalid","none"},1500)
assert(opened==3)
sfx.play({"wave","guard"},1800)
assert(opened==3)
failed=true
sfx.update(1800)
assert(closed==2) -- Failed tracks retire without round-time replacement.
sfx.close()
assert(closed==3)

failed=false
local replay=Sounds.new(audio,Clips)
local before_open,before_close=opened,closed
replay.play({"wave","guard","hurt"},5000)
assert(opened==before_open) -- Round playback creates no tracks.
for now=5000,6200,20 do replay.update(now) end
assert(closed==before_close) -- Normal completion keeps all pool tracks alive.
busy=true
replay.play({"charge","absorb"},7000)
replay.update(9000)
replay.stop()
assert(closed==before_close) -- Overdue/busy cues are dropped without device churn.
busy=false
replay.close()
assert(closed==before_close+3)
local unavailable=Sounds.new({new_output=function() return nil end},Clips)
unavailable.play({"charge","wave"},0)
unavailable.update(0)
unavailable.stop()
unavailable.close()
return "ok"
