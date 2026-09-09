-- Short, nonblocking round cues. All timing is relative to play_result.
local M = {}
function M.new(audio, clips)
    local voices = {}
    local slots = {}
    local prepared = {}
    local self = {}
    -- Allocate the maximum simultaneous cue count before the first visible
    -- frame. Desktop track creation synchronizes with the mixer and can
    -- otherwise block a round-resolution frame for hundreds of milliseconds.
    for _=1,3 do
        local ok,output=pcall(audio.new_output, {
            sample_rate=16000, channels=1, bits_per_sample=16, volume=45})
        if ok and output then
            local info=output:info()
            local chunk=math.max(2,(info.frame_samples or 0)*2)
            if chunk==2 then chunk=640 end
            slots[#slots+1]={output=output,chunk=chunk}
        end
    end
    -- All pool tracks share one format, so frame padding is also paid once.
    if slots[1] then
        local chunk=slots[1].chunk
        for kind,pcm in pairs(clips) do
            prepared[kind]=pcm..string.rep("\0",(-#pcm)%chunk)
        end
    end
    function self.stop()
        voices = {}
    end
    function self.close()
        self.stop()
        for _,slot in ipairs(slots) do
            if slot.output then pcall(slot.output.close,slot.output);slot.output=nil end
        end
    end
    function self.play(actions, now)
        self.stop()
        local seen = {}
        local next_slot=1
        for _,kind in ipairs(actions) do
            local pcm = prepared[kind]
            if pcm and not seen[kind] then
                seen[kind] = true
                while slots[next_slot] and not slots[next_slot].output do
                    next_slot=next_slot+1
                end
                local slot=slots[next_slot]
                if slot then
                    voices[#voices+1] = {slot=slot,pcm=pcm,pos=1,
                        chunk=slot.chunk,started=now,ends=now+#pcm/32}
                    next_slot=next_slot+1
                end
            end
        end
    end
    function self.update(now)
        for i=#voices,1,-1 do
            local v = voices[i]
            if now >= v.ends+100 then
                -- Drop overdue audio rather than spill into the next round.
                table.remove(voices,i)
            else
                local limit = math.min(#v.pcm, math.max(0,now-v.started+80)*32)
                while v.pos <= limit do
                    local chunk = v.pcm:sub(v.pos,v.pos+v.chunk-1)
                    local called,ok,err,written = pcall(v.slot.output.write,v.slot.output,chunk)
                    if not called or (not ok and err~="audio output: busy") then
                        pcall(v.slot.output.close,v.slot.output)
                        v.slot.output=nil
                        table.remove(voices,i)
                        break
                    end
                    v.pos = v.pos + (ok and #chunk or (written or 0))
                    if not ok then break end
                end
            end
        end
    end
    return self
end
return M
