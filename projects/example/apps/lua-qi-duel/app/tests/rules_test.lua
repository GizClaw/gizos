local function state(q1,q2,cd) return {{hp=5,qi=q1,shield_cd=cd or 0},{hp=5,qi=q2,shield_cd=0}} end
local function check(q1,q2,a,b,h1,h2,e1,e2,cd)
    local s=state(q1,q2)
    local r=Rules.resolve(s,{a,b})
    assert(r.players[1].hp==h1 and r.players[2].hp==h2,a.."/"..b.." hp")
    assert(r.players[1].qi==e1 and r.players[2].qi==e2,a.."/"..b.." qi")
    assert(r.players[1].shield_cd==(cd or 0))
    assert(s[1].hp==5 and s[1].qi==q1 and s[2].qi==q2,"mutated snapshot")
    return r
end
assert(Rules.SELECT_MS==3000)
local fresh=Rules.new();assert(fresh[1].qi==0 and fresh[2].qi==0 and fresh[1].hp==5)
check(0,1,"wave","wave",4,5,0,0) -- invalid wave is vulnerable
check(0,1,"charge","wave",4,5,1,0) -- charge survives being hit
check(0,1,"guard","wave",5,5,0,0)
check(0,1,"absorb","wave",5,5,1,0)
check(0,0,"absorb","charge",5,5,0,1) -- does not steal stored/new qi
check(5,0,"wave","idle",5,2,0,0)
check(0,5,"guard","wave",4,5,0,0,1)
check(0,5,"absorb","wave",3,5,1,0)
check(5,1,"wave","wave",5,3,0,0)
check(5,5,"wave","wave",5,5,0,0)
check(5,0,"charge","idle",5,5,5,0)
local cold=Rules.resolve(state(0,1,1),{"guard","wave"})
assert(cold.actions[1]=="invalid" and cold.players[1].hp==4 and cold.players[1].shield_cd==0)
assert(Rules.available(cold.players[1],"guard"))
local dead=state(0,5);dead[1].hp=1
assert(Rules.resolve(dead,{"idle","wave"}).winner==2)

local actions={"charge","wave","absorb","guard","idle"}
for q1=0,5 do for q2=0,5 do for cd=0,1 do
    for _,a in ipairs(actions) do for _,b in ipairs(actions) do
        local s=state(q1,q2,cd)
        local r=Rules.resolve(s,{a,b})
        local flipped=Rules.resolve({s[2],s[1]},{b,a})
        for i=1,2 do
            local p,other=r.players[i],flipped.players[3-i]
            assert(p.hp==other.hp and p.qi==other.qi and p.shield_cd==other.shield_cd,"asymmetric")
            assert(p.hp>=0 and p.hp<=5 and p.qi>=0 and p.qi<=5)
        end
    end end
end end end

local round=Rules.round(7,100)
assert(round.deadline==3100)
assert(not Rules.lock(round,6,1,"wave",500)) -- stale round
assert(Rules.lock(round,7,1,"charge",500))
assert(not Rules.lock(round,7,1,"wave",501)) -- cannot change locked move
assert(Rules.finish(round,Rules.new(),3099)==nil)
assert(not Rules.lock(round,7,2,"guard",3100)) -- deadline is exclusive
local timeout=Rules.finish(round,Rules.new(),3100)
assert(timeout.actions[2]=="charge" and timeout.players[1].qi==1 and timeout.players[2].qi==1)
assert(Rules.finish(round,Rules.new(),3200)==nil) -- duplicate result ignored
round=Rules.round(8,0)
assert(Rules.lock(round,8,1,"guard",300) and Rules.lock(round,8,2,"charge",500))
local snapshot=Rules.new()
assert(Rules.finish(round,snapshot,500)==nil) -- both locked: still wait
assert(Rules.finish(round,snapshot,2999)==nil and not round.done)
assert(snapshot[2].qi==0 and snapshot[1].hp==5)
assert(Rules.finish(round,snapshot,3000).players[2].qi==1)
assert(Rules.finish(round,snapshot,3001)==nil) -- exactly one settlement
round=Rules.round(9,0)
assert(Rules.finish(round,Rules.new(),2999,{"wave","charge"})==nil)
local automatic=Rules.finish(round,Rules.new(),3000,{"wave","charge"})
assert(automatic.actions[1]=="invalid" and automatic.actions[2]=="charge")
round=Rules.round(10,0)
assert(Rules.lock(round,10,1,"guard",100))
automatic=Rules.finish(round,state(0,1),3000,{"charge","wave"})
assert(automatic.actions[1]=="guard" and automatic.actions[2]=="wave" and automatic.players[1].hp==5)
return "ok"
