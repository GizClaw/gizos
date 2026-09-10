-- Pure, simultaneous rules. No rendering, wall clock, random input or transport.
local M={MAX_QI=5,MAX_HP=5,SELECT_MS=3000,PLAY_MS=1800}
function M.countdown_step_ms(number)
    return math.max(600,1000-200*math.floor((number-1)/7))
end
function M.selection_ms(number) return 3*M.countdown_step_ms(number) end
function M.new()
    return {{hp=5,qi=0,shield_cd=0},{hp=5,qi=0,shield_cd=0}}
end
function M.available(player,action)
    if action=="wave" then return player.qi>0 end
    if action=="guard" then return player.shield_cd==0 end
    return action=="charge" or action=="absorb" or action=="idle"
end
function M.resolve(before,choices)
    local out={players={},actions={},power={},damage={0,0},broken={false,false}}
    for i=1,2 do
        local p=before[i]
        assert(p.hp>=0 and p.hp<=5 and p.qi>=0 and p.qi<=5 and
            p.shield_cd>=0 and p.shield_cd<=1,"invalid round state")
        local action=choices[i] or "idle"
        if not M.available(p,action) then action="invalid" end
        out.actions[i]=action
        out.players[i]={hp=p.hp,qi=p.qi,shield_cd=math.max(0,p.shield_cd-1)}
        out.power[i]=action=="wave" and (p.qi==5 and 3 or 1) or 0
        if action=="wave" then out.players[i].qi=p.qi-(p.qi==5 and 5 or 1) end
    end
    local cancelled=math.min(out.power[1],out.power[2])
    for i=1,2 do
        local incoming=out.power[3-i]-cancelled
        local p,action=out.players[i],out.actions[i]
        local damage=incoming
        if action=="guard" then
            damage=math.max(0,incoming-2)
            if incoming==3 then p.shield_cd=1;out.broken[i]=true end
        elseif action=="absorb" and incoming>0 then
            p.qi=math.min(5,p.qi+1);damage=incoming-1
        elseif action=="charge" then p.qi=math.min(5,p.qi+1) end
        out.damage[i]=damage;p.hp=math.max(0,p.hp-damage)
    end
    if out.players[1].hp==0 and out.players[2].hp==0 then out.winner="draw"
    elseif out.players[1].hp==0 then out.winner=2
    elseif out.players[2].hp==0 then out.winner=1 end
    return out
end

-- One authoritative local round coordinator. A future network adapter must
-- deliver authenticated commit/reveal choices here, not remote damage values.
function M.round(number,now) return {number=number,deadline=now+M.selection_ms(number),choices={}} end
function M.lock(round,number,side,action,now)
    if number~=round.number or (side~=1 and side~=2) or round.done or
        now>=round.deadline or round.choices[side]~=nil then return false end
    round.choices[side]=action
    return true
end
function M.finish(round,before,now,candidates)
    -- Locking both moves never shortens the shared countdown.
    if round.done or now<round.deadline then return nil end
    round.done=true
    for side=1,2 do
        round.choices[side]=round.choices[side] or (candidates and candidates[side]) or "charge"
    end
    return M.resolve(before,round.choices)
end
return M
