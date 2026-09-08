-- Transport-independent, versioned commit/reveal protocol over a reliable stream.
-- All wire times are in the central's monotonic clock, never wall-clock time.
local Rules=require("rules")
local M={};M.__index=M
local function int(v,lo,hi) return type(v)=="number" and v%1==0 and v>=lo and v<=hi end
local function hex(v,n) return type(v)=="string" and #v==n and not v:find("[^0-9a-f]") end
local function canonical(id,round,side,action,nonce)
    return table.concat({"QID1",id,round,side,action,nonce},"|")
end
function M:send(kind,fields)
    fields=fields or {};fields.v=1;fields.kind=kind;fields.id=self.id;fields.round=self.round
    if not self.io.send(fields) then self.error="SEND FAILED" end
end
function M.new(central,now,io)
    local self=setmetatable({central=central,side=central and 1 or 2,io=io,offset=0,
        phase="sync",players=Rules.new(),round=0,last=now,started=now},M)
    if central then self.id=io.nonce();self.ping=now;self:send("HELLO",{t=now}) end
    return self
end
function M:ready(now)
    self.phase="ready";self.ready_at=now;self.round=self.round+1
    self.commits={};self.reveals={};self.revealed=false;self.verified=false;self.delivered=false
    self.pending=nil;self.remote_hash=nil;self.hash=nil;self.remote_ready=false
    self:send("READY")
end
function M:start(now)
    self.start_at=now+300;self.deadline=self.start_at+Rules.SELECT_MS;self.phase="choose"
    self:send("START",{start=self.start_at})
end
function M:lock(action,now,timeout)
    if self.error or self.phase~="choose" or self.commits[self.side] or now<self.start_at or
        (not timeout and now>=self.deadline) then return false end
    local nonce=self.io.nonce()
    self.reveals[self.side]={action=action,nonce=nonce}
    local digest=self.io.digest(canonical(self.id,self.round,self.side,action,nonce))
    self.commits[self.side]=digest
    self:send("COMMIT",{hash=digest})
    return true
end
function M:receive(m,now)
    if self.error then return end
    if type(m)~="table" or m.v~=1 or type(m.kind)~="string" or not int(m.round,0,1000000) then
        self.error="BAD MESSAGE";return end
    if m.kind=="HELLO" and not self.central and self.phase=="sync" then
        if not hex(m.id,32) or not int(m.t,0,2^53) then self.error="BAD HELLO";return end
        self.id=m.id;self:send("PONG",{t=m.t,received=now,sent=now});self.last=now;return
    end
    if m.id~=self.id then self.error="WRONG SESSION";return end
    self.last=now
    if m.kind=="PONG" and self.central and self.phase=="sync" then
        if m.t~=self.ping or not int(m.received,0,2^53) or not int(m.sent,m.received,2^53) then
            self.error="BAD CLOCK";return end
        self:send("SYNC",{offset=((self.ping-m.received)+(now-m.sent))/2})
        self:ready(now);return
    elseif m.kind=="SYNC" and not self.central and self.phase=="sync" then
        if type(m.offset)~="number" or m.offset~=m.offset or math.abs(m.offset)>2^52 then self.error="BAD CLOCK";return end
        self.offset=m.offset;self:ready(now);return
    end
    if m.round<self.round then return end -- Already consumed; no double settlement.
    if m.round>self.round then
        if m.kind=="READY" and m.round==self.round+1 and self.phase=="play" then self.next_ready=true;return end
        self.error="WRONG ROUND";return
    end
    if m.kind=="READY" then
        if self.phase=="ready" then self.remote_ready=true end
    elseif m.kind=="START" and not self.central and self.phase=="ready" then
        if not int(m.start,0,2^53) then self.error="BAD START";return end
        self.start_at=m.start-self.offset;self.deadline=self.start_at+Rules.SELECT_MS;self.phase="choose"
        if self.start_at<now-200 then self.error="CLOCK TOO LATE" end
    elseif m.kind=="COMMIT" and self.phase=="choose" then
        local peer=3-self.side
        if not hex(m.hash,64) or now>self.deadline+1500 or
            (self.commits[peer] and self.commits[peer]~=m.hash) then self.error="BAD COMMIT";return end
        self.commits[peer]=m.hash
    elseif m.kind=="REVEAL" and self.phase=="choose" then
        local peer=3-self.side
        if not self.commits[1] or not self.commits[2] or not hex(m.nonce,32) or
            not ({charge=true,wave=true,guard=true,absorb=true})[m.action] or
            self.io.digest(canonical(self.id,self.round,peer,m.action,m.nonce))~=self.commits[peer] then
            self.error="BAD REVEAL";return end
        self.reveals[peer]={action=m.action,nonce=m.nonce}
    elseif m.kind=="RESULT" and self.phase=="choose" then
        if not hex(m.hash,64) then self.error="BAD RESULT";return end
        self.remote_hash=m.hash
    elseif m.kind=="PLAY" and not self.central and self.hash and self.remote_hash==self.hash then
        if not int(m.start,0,2^53) then self.error="BAD PLAY";return end
        self.play_at=m.start-self.offset;self.phase="play";self.verified=true
        if self.play_at<now-500 then self.error="PLAY TOO LATE" end
    else self.error="UNEXPECTED MESSAGE" end
end
function M:tick(now)
    if self.error then return end
    if self.phase=="sync" and now-self.started>6000 then self.error="SYNC TIMEOUT";return end
    if self.phase=="ready" then
        if self.next_ready then self.remote_ready=true;self.next_ready=nil end
        if self.central and self.remote_ready then self:start(now) end
        if now-self.ready_at>6000 then self.error="READY TIMEOUT" end
    elseif self.phase=="choose" then
        -- Early confirmations are commitments only. Neither reveal nor resolve
        -- before the local synchronized deadline, even when both have locked.
        if now<self.deadline then return end
        if now>=self.deadline and not self.commits[self.side] then
            self:lock(self.io.candidate and self.io.candidate() or "charge",now,true)
        end
        if self.commits[1] and self.commits[2] and not self.revealed then
            self.revealed=true;self:send("REVEAL",self.reveals[self.side])
        end
        if self.revealed and self.reveals[1] and self.reveals[2] and not self.pending then
            self.pending=Rules.resolve(self.players,{self.reveals[1].action,self.reveals[2].action})
            local p=self.pending.players
            local data=table.concat({self.id,self.round,p[1].hp,p[1].qi,p[1].shield_cd,
                p[2].hp,p[2].qi,p[2].shield_cd,self.pending.actions[1],self.pending.actions[2]},"|")
            self.hash=self.io.digest(data);self:send("RESULT",{hash=self.hash})
        end
        if self.hash and self.remote_hash then
            if self.hash~=self.remote_hash then self.error="STATE MISMATCH";return end
            self.verified=true
            if self.central then self.play_at=now+250;self:send("PLAY",{start=self.play_at});self.phase="play" end
        end
        if now>self.deadline+1500 then self.error="ROUND TIMEOUT" end
    elseif self.phase=="play" and now>=self.play_at+Rules.PLAY_MS then
        self.players=self.pending.players
        if self.pending.winner then self.phase="over" else self:ready(now) end
    end
end
function M:result(now)
    if self.phase=="play" and now>=self.play_at and not self.delivered then
        self.delivered=true;return self.pending,self.play_at
    end
end
return M
