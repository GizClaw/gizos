-- Reliable in-memory wire, independent clocks and delay; no real BLE claim.
local queues={{},{}};local nonce=0
local candidates={"guard","absorb"}
local function hash(data)
    -- Test-only deterministic digest; production uses the Crypto PAL HKDF-SHA256.
    local h=2166136261
    for i=1,#data do h=((h~data:byte(i))*16777619)&0xffffffff end
    return string.rep(string.format("%08x",h),8)
end
local function io(side)
    return {nonce=function()nonce=nonce+1;return string.format("%032x",nonce)end,digest=hash,
        candidate=function() return candidates[side] end,
        send=function(m)
            local copy={};for k,v in pairs(m)do copy[k]=v end
            queues[3-side][#queues[3-side]+1]=copy;return true
        end}
end
local a=Protocol.new(true,0,io(1));local b=Protocol.new(false,5000,io(2))
local function pump(t)
    for _=1,3 do
        for side,p in ipairs({a,b}) do
            local now=t+(side==2 and 5000 or 0)
            local q=queues[side];queues[side]={}
            for _,m in ipairs(q)do p:receive(m,now) end
            p:tick(now)
            assert(not p.error,p.error)
        end
    end
end
for t=0,500,20 do pump(t) end
assert(a.phase=="choose" and b.phase=="choose")
assert(math.abs((b.deadline-a.deadline)-5000)<50)
assert(a:lock("charge",600) and b:lock("wave",5600))
assert(not a:lock("wave",601))
local deadline=a.deadline
for t=600,deadline-1,20 do pump(t) end
pump(deadline-1)
assert(a.commits[1] and a.commits[2] and b.commits[1] and b.commits[2])
for _,p in ipairs({a,b}) do
    assert(p.phase=="choose" and not p.revealed and not p.pending and not p.hash)
    assert(p.players[1].qi==0 and p.players[2].hp==5)
end
assert(a:result(deadline-1)==nil and b:result(deadline+4999)==nil)
for t=deadline,deadline+500,20 do pump(t) end
local ar=a:result(deadline+500);local br=b:result(deadline+5500)
assert(ar and br and ar.actions[2]=="invalid" and ar.players[1].qi==1)
assert(ar.players[1].qi==br.players[1].qi and ar.players[2].hp==br.players[2].hp)
assert(a.play_at>=deadline and b.play_at>=b.deadline)
assert(a:result(deadline+500)==nil)
for t=deadline+520,deadline+6000,20 do pump(t) end -- no confirmation: use current candidate
assert(a.round==2 and b.round==2)
assert(a.pending.actions[1]=="guard" and a.pending.actions[2]=="absorb")
assert(b.pending.actions[1]=="guard" and b.pending.actions[2]=="absorb")
assert(a.players[1].qi==1 and b.players[1].qi==1)

-- A malformed reveal must stop before any result is emitted.
local bad=Protocol.new(true,0,io(1))
bad.phase="choose";bad.round=1;bad.start_at=0;bad.deadline=Rules.SELECT_MS
bad.commits={[1]=hash("a"),[2]=hash("b")};bad.reveals={}
bad:receive({v=1,id=bad.id,round=1,kind="REVEAL",action="wave",nonce=string.rep("0",32)},1000)
assert(bad.error=="BAD REVEAL" and bad.pending==nil)

local missing=Protocol.new(true,0,io(1))
missing.phase="choose";missing.round=1;missing.start_at=0;missing.deadline=Rules.SELECT_MS
missing.commits={};missing.reveals={}
candidates[1]="wave"
missing:tick(Rules.SELECT_MS);assert(missing.reveals[1].action=="wave")
missing:tick(Rules.SELECT_MS+1501);assert(missing.error=="ROUND TIMEOUT" and missing.pending==nil)
return "ok"
