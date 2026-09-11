local display = require("display")
local delay = require("delay")
local system = require("system")
local Rules = require("rules")

-- Native vector renderer; component selection supports deterministic visual regression.
if display.draw_vector_affine and display.draw_vector_icon then
    local review=type(args)=="table" and args.draw_component or ""
    local accepted=review=="" or review=="all" or review=="hand-left" or review=="hand-right"
    local affine,atlas=display.draw_affine_asset,display.draw_sprite_atlas
    local names={"charge","wave","absorb","guard"}
    local function focus_at(frame)
        if frame<=17 then return (frame-1)/32 end
        if frame==18 then return .500001 end
        return (frame-2)/32
    end
    display.draw_affine_asset=function(name,a,b,c,d,e,f,crop,opacity)
        if accepted and name=="@qi-duel/opponent.h2r8" then
            assert(crop==nil,"vector opponent crop is unsupported")
            return display.draw_vector_affine("@qi-duel/vector/opponent.h2vg",a,b,c,d,e,f,opacity or 1)
        end
        if accepted and name=="@qi-duel/hand-left.h2r8" then
            assert(crop==nil,"vector hand crop is unsupported")
            return display.draw_vector_affine("@qi-duel/vector/hand-left.h2vg",
                a*145/1141,b*145/1141,c*177/1379,d*177/1379,e,f,opacity or 1,1)
        end
        if accepted and name=="@qi-duel/hand-right.h2r8" then
            assert(crop==nil,"vector hand crop is unsupported")
            return display.draw_vector_affine("@qi-duel/vector/hand-right.h2vg",
                a*145/1199,b*145/1199,c*177/1312,d*177/1312,e,f,opacity or 1,2)
        end
        return affine(name,a,b,c,d,e,f,crop,opacity)
    end
    display.draw_sprite_atlas=function(name,lo,hi,mix,x,y,scale,opacity)
        if not accepted or (name~="@qi-duel/skill-styles.h2rs" and name~="@qi-duel/skill-colors.h2rs") then
            return atlas(name,lo,hi,mix,x,y,scale,opacity)
        end
        local index,dir,focus,sheen
        if lo>408 then
            index=(lo-409)//21+1
            local frame=(lo-409)%21
            dir,focus,sheen=0,1,frame==20 and -1 or frame/19
        else
            index=(lo-1)//102+1;dir=((lo-1)%102)//34
            focus=focus_at((lo-1)%34+1)*(1-mix)+focus_at((hi-1)%34+1)*mix
            sheen=-2
        end
        assert(index>=1 and index<=4,"invalid vector skill")
        return display.draw_vector_icon("@qi-duel/vector/"..names[index]..".h2vg",
            focus,dir,name=="@qi-duel/skill-styles.h2rs" and -1 or index-1,
            sheen,x,y,scale or 1,opacity or 1)
    end
    local component=(review=="" or review=="hand-left" or review=="hand-right") and "all" or review
    if component~="reference" then
        if component=="arena" or component=="all" then require("procedural").install(display,"arena") end
        if component~="arena" then require("component_renderer").install(display,component) end
    end
end

local FRAME_MS = 33
local PERF_INTERVAL_MS = 1000
local SELF_TEST_WINDOWS = 5
local MIN_PASS_FPS_TENTHS = 270
local MAX_PASS_FPS_TENTHS = 320
local SCREEN_W, SCREEN_H = display.width, display.height
local H106 = SCREEN_W==240 and SCREEN_H==240
-- H106 hardware is button-only; desktop previews may also provide touch.
local touch_ok, touch = pcall(require, "lcd_touch")
if not touch_ok or type(touch) ~= "table" then
    if not H106 then error("touch unavailable: " .. tostring(touch)) end
    touch = nil
end
-- Art/animation coordinates stay in the approved design space. Each H106
-- group has its own uniform transform; never squash the portrait scene.
local W, H = 368, 448
local layouts = H106 and {
    scene={240/368,0,-30}, opponent={.52,24.32,6.2},
    hands={.62,5.92,4}, player={.57,1.3,-24.49}, enemy={.57,27.94,-.55},
    carousel={.60,9.6,-31}, screen={1,0,0},
} or nil
local function viewport(name) end
if H106 then
    local native=display
    display=setmetatable({}, {__index=native})
    local s,tx,ty=1,0,0
    viewport=function(name) s,tx,ty=table.unpack(layouts[name]) end
    display.draw_affine_asset=function(name,a,b,c,d,e,f,crop,opacity)
        native.draw_affine_asset(name,a*s,b*s,c*s,d*s,e*s+tx,f*s+ty,crop,opacity)
    end
    display.draw_sprite_atlas=function(name,lo,hi,mix,x,y,scale,opacity)
        native.draw_sprite_atlas(name,lo,hi,mix,x*s+tx,y*s+ty,s*(scale or 1),opacity or 1)
    end
    display.add_disc=function(x,y,r,color,alpha) native.add_disc(x*s+tx,y*s+ty,r*s,color,alpha) end
    for _,name in ipairs({"add_line","over_line"}) do
        display[name]=function(ax,ay,bx,by,w,color,alpha)
            native[name](ax*s+tx,ay*s+ty,bx*s+tx,by*s+ty,w*s,color,alpha)
        end
    end
    display.glow_line=function(ax,ay,bx,by,w,color,alpha,blur,shadow)
        native.glow_line(ax*s+tx,ay*s+ty,bx*s+tx,by*s+ty,w*s,color,alpha,blur*s,shadow)
    end
    display.add_lines=function(lines,count) native.add_lines(lines,count,s,tx,ty) end
    display.draw_polygon=function(points,fill,fa,stroke,sa,w,shadow,sha,blur)
        local mapped={}
        for i,p in ipairs(points) do mapped[i]={p[1]*s+tx,p[2]*s+ty} end
        native.draw_polygon(mapped,fill,fa,stroke,sa,w*s,shadow,sha,blur*s)
    end
    display.draw_light_atlas=function(name,gains) native.draw_light_atlas(name,gains,layouts.scene) end
end
local CX = W // 2
local TAU = math.pi * 2
local options = type(args) == "table" and args or {}
local DESKTOP_CLICKS = options.click_controls=="1"
local inspector_layer = options.layer or "full"
local PLAY_GAME=options.battle=="1" and inspector_layer=="full"
-- Keep synthesis and audio output closed while validating display performance.
local AUDIO_ENABLED=false
local sfx
if PLAY_GAME and AUDIO_ENABLED then
    local ok,audio = pcall(require,"audio")
    if ok and type(audio)=="table" then
        sfx=require("retro_audio").new(audio,require("retro_score"))
    end
end
local fixed_time_ms = tonumber(options.time_ms)
local impact_probe = options.impact ~= "" and options.impact or nil
local result_probe = options.result ~= "" and options.result or nil
local Link=PLAY_GAME and require("duel_link") or nil
local Protocol=PLAY_GAME and require("link_protocol") or nil
local json=PLAY_GAME and require("json") or nil
local INTRO_ENABLED=PLAY_GAME and fixed_time_ms==nil and result_probe==nil
local page,mode=INTRO_ENABLED and "intro" or "battle",
    INTRO_ENABLED and "pending" or "demon"
local network=nil
local scene_started_ms = system.millis()
local Intro={PAIR_TIMEOUT_MS=8000,MIN_SEARCH_MS=2400,SLOWDOWN_MS=650,
    TILT_MS=1450,FADE_MS=480,
    clash_probe=options.clash and options.clash~="" and options.clash or nil}
local intro=INTRO_ENABLED and {phase="ready",particle_ms=0,last_ms=0} or nil
local supported_layers = {impact=true,full=true,walls=true,wheel=true,arena=true,dust=true,
    particles=true,opponent=true,["hand-left"]=true,["hand-right"]=true,
    ["arena-dust"]=true,scene7=true,hud=true,scene8=true,scene11=true,
    ["carousel-frame"]=true,["charge-cells"]=true,["charge-base"]=true,carousel=true,
    ["skill-charge"]=true,["skill-wave"]=true,["skill-absorb"]=true,["skill-guard"]=true}
assert(supported_layers[inspector_layer], "unsupported inspector layer")
assert(fixed_time_ms == nil or (fixed_time_ms >= 0 and fixed_time_ms < math.huge),
    "invalid inspector time")
assert(impact_probe == nil or impact_probe == "combo" or impact_probe == "armor-break",
    "invalid impact probe")
assert(result_probe == nil or result_probe == "win" or result_probe == "lose",
    "invalid result probe")
assert(Intro.clash_probe==nil or Intro.clash_probe=="equal" or
    Intro.clash_probe=="player-combo" or Intro.clash_probe=="enemy-combo" or
    Intro.clash_probe=="both-combo","invalid clash probe")

-- Fixed 368x448 composition values from the approved device layout.
local HUD_W, HUD_H = 190, 190 * 116 / 398
local PLAYER_HUD_X, PLAYER_HUD_Y = 10, 57
local ENEMY_HUD_X, ENEMY_HUD_Y = 168, 15
local CAROUSEL_CX, CAROUSEL_CY = 184, 678
local CAROUSEL_RADIUS, CAROUSEL_STEP = 278, 0.39
-- Shrink the approved base and charge orbit together about its lower anchor.
local METER_SCALE, METER_ANCHOR_Y = .78, 418
local CHARGE_MAX = 5
local CHARGE_CX, CHARGE_CY = 184, METER_ANCHOR_Y+(694-METER_ANCHOR_Y)*METER_SCALE
local CHARGE_RADIUS, CHARGE_SPAN = 359*METER_SCALE, 0.74
local SWIPE_THRESHOLD = 28

local function rgb(r, g, b)
    return { r = r, g = g, b = b }
end

local COLOR = {
    black = rgb(0, 0, 0),
    near_black = rgb(0, 4, 8),
    cyan = rgb(52, 226, 255),
    cyan_hot = rgb(178, 251, 255),
    cyan_mid = rgb(18, 125, 164),
    cyan_dark = rgb(4, 39, 57),
    violet = rgb(188, 65, 255),
    violet_mid = rgb(85, 28, 142),
    violet_dark = rgb(28, 8, 55),
    orange = rgb(255, 126, 34),
    orange_mid = rgb(133, 62, 13),
    white = rgb(238, 250, 255),
}

local SKILLS = {
    { name = "CHARGE", kind = "charge", asset = "skill-charge" },
    { name = "WAVE", kind = "wave", asset = "skill-wave" },
    { name = "ABSORB", kind = "absorb", asset = "skill-absorb" },
    { name = "GUARD", kind = "guard", asset = "skill-guard" },
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function ease(value)
    value=clamp(value,0,1)
    return value*value*(3-2*value)
end

local function skill_index(index)
    return ((index - 1) % #SKILLS) + 1
end

local selected = (tonumber(options.selected) or 0)+1
local fixed_drag = tonumber(options.drag)
assert(selected>=1 and selected<=4 and selected%1==0,"invalid carousel selection")
assert(not fixed_drag or (fixed_drag>=-90 and fixed_drag<=90),"invalid carousel drag")
local qi = tonumber(options.qi) or 3
assert(qi>=0 and qi<=CHARGE_MAX and qi%1==0,"invalid charge value")
local player_hp, enemy_hp = 4, 4
if PLAY_GAME then qi,player_hp,enemy_hp=0,5,5 end
local battle,invalid_fx,confirm_fx=nil,nil,nil
local settlement=result_probe and {kind=result_probe,started=0,clash=false} or nil
local RESULT_ENTRY_MS,RESULT_EXIT_MS,INTRO_REENTRY_MS=1100,1050,480
-- Keep the charge ring clear; the touch targets stay generous.
local SKILL_SCALE=(H106 and .82 or 1)*.75
local function controls_locked()
    return PLAY_GAME and battle and battle.phase~="over" and
        (battle.phase~="select" or battle.round.choices[1]~=nil)
end
local frame_count = 0
local carousel_offset = 0
local target_drag = 0
local touch_tracking = false
local touch_target = nil
local touch_start_x, touch_start_y = 0, 0
local meter_fx = { charge = nil, player = nil, enemy = nil }
local cast = nil
local gesture_mode, gesture_skill = nil, nil
local lifted_x, lifted_y = 0, 0
local throw_fx = nil
local CAST_LIFT = 44 -- design pixels; H106 inverse mapping preserves the gesture
local ACTIONS = {
    charge={row=0,windup=360,hold=820,recover=300},
    wave={row=1,windup=480,hold=620,recover=320},
    absorb={row=2,windup=360,hold=1000,recover=360},
    guard={row=3,windup=230,hold=800,recover=280},
}
local function scene_time() return fixed_time_ms or (system.millis()-scene_started_ms) end
local function start_icon_echo(index,progress)
    if PLAY_GAME and battle and battle.phase~="play" then return end
    progress=clamp(progress or 0,0,1)
    throw_fx={index=index,started=scene_time(),x=CAROUSEL_CX,y=400,
        scale=SKILL_SCALE*(1+progress*.45),opacity=progress>0 and progress*.38 or .4}
end
local function start_cast(kind,now,actor)
    if cast and now<cast.started+cast.duration then
        print("H2_QI_DUEL_CAST ignored=busy")
        return false
    end
    local spec=assert(ACTIONS[kind],"invalid action")
    -- Visual rehearsal, deliberately no combat costs, damage or AI rules.
    cast={kind=kind,started=now,duration=spec.windup+spec.hold+spec.recover,
        actor=actor or "both"}
    print(string.format("H2_QI_DUEL_CAST skill=%s actor=%s",kind,cast.actor))
    return true
end
local submit_action=start_cast
if options.action and options.action~="" then
    assert(ACTIONS[options.action],"invalid action")
    assert(not options.actor or options.actor=="" or options.actor=="both" or
        options.actor=="player" or options.actor=="opponent","invalid actor")
    start_cast(options.action,0,options.actor~="" and options.actor or "both")
end

math.randomseed(system.millis() + W * 19 + H * 37)

local wall_lights = {}
local wall_gains = {}
local function hash01(value)
    local x = math.sin(value * 91.733 + 17.133) * 43758.5453
    return x-math.floor(x)
end
-- Same ordering and deterministic envelopes as desktop-preview wallLights.
-- The atlas only stores static light coverage; Lua drives every lamp live.
for _, side in ipairs({-1, 1}) do
    for index = 0, 35 do
        local key = index * 109 + side * 31
        wall_lights[#wall_lights + 1] = {
            depth = math.min(.995, ((index % 12) + hash01(key + 7) * .88) / 12),
            phase = hash01((index % 12) * 71 + side * 17) * TAU
                + math.floor(index / 12) * TAU / 3,
            rate = .34 + hash01(key + 83) * .72,
        }
    end
end

-- Atlas order matches the original seven ring loops, radial fragments and
-- twelve projected rectangles. Positions never travel along the rings.
local arena_lights = {{kind = "base"}}
local arena_gains = {}
for ring = 0, 6 do
    for slot = 0, 15 + ring * 2 do
        if hash01(ring * 401 + slot * 97) >= .45 then
            arena_lights[#arena_lights + 1] = {
                kind = "ring",
                phase = hash01(ring * 117 + slot * 19) * TAU,
                rate = .65 + hash01(ring * 71 + slot * 13) * 2.2,
                faulty = hash01(ring * 263 + slot * 41) > .88,
                contact_rate = 8 + hash01(slot + ring * 9) * 10,
            }
        end
    end
end
for i = 0, 31 do
    if hash01(i * 173) >= .42 then
        arena_lights[#arena_lights + 1] = {
            kind = "radial", phase = hash01(i * 61) * TAU,
            rate = .8 + hash01(i * 37) * 2.6,
            faulty = hash01(i * 83) > .9,
            contact_rate = 9 + hash01(i) * 11,
        }
    end
end
-- Reproduce the reference's LCG state without coupling it to other Lua effects.
local arena_seed = 0x51D0E1
local function arena_rand()
    arena_seed = (arena_seed * 1664525 + 1013904223) % 4294967296
    return arena_seed / 4294967296
end
local space_particles, ring_dust = {}, {}
for i = 0, 103 do
    space_particles[i + 1] = {
        a = (i * 2.399 + arena_rand() * .32) % TAU,
        p0 = arena_rand(), speed = .11 + arena_rand() * .23,
        size = .42 + arena_rand() * 1.18,
        hue = i % 5 == 0 and 292 or (i % 7 == 0 and 28 or 193),
        wobble = arena_rand() * TAU,
    }
end
-- Geometry seeds and angles are immutable; calculate them once, preserving
-- the original double-precision expressions used by each animation frame.
for index,p in ipairs(space_particles) do
    local i=index-1
    local pair=math.floor(i/2)
    p.top_angle=((pair*.61803398875)%1)*TAU+(i%2)*math.pi
    p.top_extent=5.10+hash01(pair*47+9)*.72
    p.top_cos,p.top_sin=math.cos(p.top_angle),math.sin(p.top_angle)
    p.lateral=(hash01(i*31+4)*2-1)*.76
    p.forward=math.sqrt(1-p.lateral*p.lateral)
    p.final_extent=5.05+hash01(i*47+9)*.72
    p.path_angle=.025+(i-52+hash01(i*53+7)*.72)/52*(math.pi-.05)
    p.path_cos,p.path_sin=math.cos(p.path_angle),math.sin(p.path_angle)
    p.hit_radius=3+hash01(i*79+3)*.38
    p.wall_extra=hash01(i*101+5)*22
end
for i = 0, 33 do
    ring_dust[i + 1] = {a = arena_rand() * TAU, r = .24 + arena_rand() * .76,
        speed = .11 + arena_rand() * .2, size = .45 + arena_rand() * 1.15,
        hue = i % 4 == 0 and 286 or 190}
end
for _ = 1, 12 do
    for _ = 1, 4 do arena_rand() end -- static angle, radius, length and width
    local phase, rate = arena_rand() * TAU, .42 + arena_rand() * 1.25
    arena_rand() -- static orientation
    arena_lights[#arena_lights + 1] = {kind = "rect", phase = phase, rate = rate}
end

local function start_meter_fx(target, old_value, new_value)
    local direction = new_value > old_value and 1 or -1
    local index
    if target == "charge" or target == "player" then
        index = direction > 0 and new_value or old_value
    else
        index = direction > 0 and 6 - new_value or 6 - old_value
    end
    meter_fx[target] = {
        index = index, direction = direction,
        started = fixed_time_ms or (system.millis() - scene_started_ms),
    }
end

local function change_meter(target, delta)
    local old_value, new_value
    if target == "charge" then
        old_value = qi
        new_value = clamp(qi + delta, 0, CHARGE_MAX)
        qi = new_value
    elseif target == "player" then
        old_value = player_hp
        new_value = clamp(player_hp + delta, 0, 5)
        player_hp = new_value
    else
        old_value = enemy_hp
        new_value = clamp(enemy_hp + delta, 0, 5)
        enemy_hp = new_value
    end
    if old_value ~= new_value then
        start_meter_fx(target, old_value, new_value)
        print(string.format("H2_QI_DUEL_METER target=%s old=%d new=%d direction=%d",
            target,old_value,new_value,delta))
    end
end

local function begin_round(now)
    confirm_fx=nil
    battle.round=Rules.round(battle.number,now)
    battle.phase="select"
    -- Bot commits before any player input. It only sees public round-start state.
    local p=battle.players[2]
    local pool={"charge","charge","absorb"}
    if p.shield_cd==0 then pool[#pool+1]="guard" end
    if p.qi>0 then pool[#pool+1]="wave";pool[#pool+1]="wave" end
    if p.qi==5 then pool={"wave","wave","guard","absorb"}
        if p.shield_cd>0 then pool[3]="wave" end end
    battle.bot_choice=pool[math.random(#pool)]
    battle.bot_at=now+math.random(650,1500)
end
local function new_battle(now)
    if sfx then sfx.stop() end
    battle={players=Rules.new(),number=1}
    qi,player_hp,enemy_hp=0,5,5
    meter_fx={charge=nil,player=nil,enemy=nil}
    cast,throw_fx,invalid_fx,confirm_fx,settlement=nil,nil,nil,nil,nil
    begin_round(now)
end

local function begin_settlement_exit(now)
    if not PLAY_GAME or not settlement or settlement.exit_started then return false end
    if now-settlement.started<RESULT_ENTRY_MS then return false end
    settlement.exit_started=now
    print("H2_QI_DUEL_RESULT phase=fade-out")
    return true
end

local function finish_settlement_exit(now)
    if not settlement or not settlement.exit_started or
        now-settlement.exit_started<RESULT_EXIT_MS then return false end
    if mode=="void" then pcall(Link.stop) end
    network,battle,cast,throw_fx,invalid_fx,confirm_fx=nil,nil,nil,nil,nil,nil
    settlement=nil;mode="pending";page="intro"
    intro={phase="ready",particle_ms=0,last_ms=now,reentry_started=now}
    -- Result input is the new-session confirmation. Begin pairing immediately;
    -- do not require a second press after the black transition.
    Intro.start(now)
    print("H2_QI_DUEL_RESULT phase=pairing-wait")
    return true
end

function Intro.rate(now)
    if not intro or intro.phase=="done" then return 1 end
    if intro.phase=="ready" then return .22 end
    if intro.phase=="search" then
        local age=now-intro.started
        if age<1200 then return .22+ease(age/1200)*.38 end
        if age<2200 then return .60+ease((age-1200)/1000)*1.60 end
        return 2.20
    end
    if intro.phase=="slowdown" then
        local p=ease((now-intro.phase_started)/Intro.SLOWDOWN_MS)
        return 2.20+(0.30-2.20)*p
    end
    if intro.phase=="tilt" then
        return .30+.70*ease((now-intro.phase_started)/Intro.TILT_MS)
    end
    return 1
end

function Intro.start(now)
    if not intro or intro.phase~="ready" then return end
    intro.phase="search";intro.started=now;intro.phase_started=now
    intro.last_ms=now;intro.connected=false;intro.decision=nil
    local called,ok,rc=pcall(Link.pair)
    intro.pair_started=called and ok or false
    intro.pair_result=called and rc or tostring(ok)
    print(string.format("H2_QI_DUEL_INTRO phase=search pair_started=%s result=%s",
        tostring(intro.pair_started),tostring(intro.pair_result)))
end

function Intro.finish(now)
    local online=false
    if intro.decision=="void" and intro.pair_started then
        local ok,_,connected=pcall(Link.state)
        online=ok and connected or false
    end
    intro.phase="done";intro.last_ms=now;page="battle"
    if online then
        mode="void";network=nil;battle=nil
        print("H2_QI_DUEL_INTRO result=online")
    else
        if intro.pair_started then pcall(Link.stop) end
        intro.pair_started=false;mode="demon";network=nil
        new_battle(now)
        print("H2_QI_DUEL_INTRO result=computer")
    end
end

function Intro.update(now)
    if not intro then return end
    local dt=clamp(now-intro.last_ms,0,100)
    intro.particle_ms=intro.particle_ms+dt*Intro.rate(now)
    intro.last_ms=now
    if intro.phase=="ready" or intro.phase=="done" then return end
    if intro.phase=="search" then
        local connected=false
        if intro.pair_started then
            local ok,_,is_connected=pcall(Link.state)
            if ok then connected=is_connected else intro.pair_started=false end
        end
        intro.connected=connected
        local age=now-intro.started
        if (connected and age>=Intro.MIN_SEARCH_MS) or age>=Intro.PAIR_TIMEOUT_MS then
            intro.decision=connected and "void" or "demon"
            intro.phase="slowdown";intro.phase_started=now
            if sfx and sfx.cue then sfx.cue("connected",now) end
            if not connected and intro.pair_started then
                pcall(Link.stop);intro.pair_started=false
            end
            print(string.format("H2_QI_DUEL_INTRO phase=slowdown decision=%s waited_ms=%d",
                intro.decision,age))
        end
    elseif intro.phase=="slowdown" and now-intro.phase_started>=Intro.SLOWDOWN_MS then
        intro.phase="tilt";intro.phase_started=now
        print("H2_QI_DUEL_INTRO phase=tilt")
    elseif intro.phase=="tilt" and now-intro.phase_started>=Intro.TILT_MS then
        intro.phase="fade";intro.phase_started=now
        print("H2_QI_DUEL_INTRO phase=fade")
    elseif intro.phase=="fade" and now-intro.phase_started>=Intro.FADE_MS then
        Intro.finish(now)
    end
end
local function leave_link()
    if sfx then sfx.stop() end
    if mode=="void" then Link.stop() end
    network=nil;battle=nil;cast=nil;settlement=nil;mode="pending";page="intro"
    intro={phase="ready",particle_ms=0,last_ms=scene_time()}
end
if PLAY_GAME and not INTRO_ENABLED and not result_probe then new_battle(0) end
if PLAY_GAME then
    submit_action=function(kind,now)
        if not battle then return false end
        if battle.phase=="over" then
            if mode=="void" then leave_link() else new_battle(now) end
            return false
        end
        if battle.phase~="select" then return false end
        local locked
        if mode=="void" then locked=network and network:lock(kind,now)
        else locked=Rules.lock(battle.round,battle.number,1,kind,now) end
        if not locked then return false end
        battle.round.choices[1]=kind
        carousel_offset=0
        for index,skill in ipairs(SKILLS) do
            if skill.kind==kind then confirm_fx={index=index,started=now};break end
        end
        print(string.format("H2_QI_DUEL_CONFIRM skill=%s at_ms=%d deadline_ms=%d",kind,now,battle.round.deadline))
        if not Rules.available(battle.players[1],kind) then
            invalid_fx={kind=kind,started=now}
            print("H2_QI_DUEL_INVALID skill="..kind)
            return false
        end
        if sfx and sfx.cue then sfx.cue("confirm",now) end
        return true
    end
end
local function play_result(result,now)
    if sfx then
        -- Results are in local-player order, including network matches.
        sfx.play({result.actions[1]},now)
    end
    battle.result=result;battle.phase="play";battle.started=now;battle.applied=false
    local index=confirm_fx and confirm_fx.index or selected
    if result.actions[1]=="invalid" then
        invalid_fx={kind=SKILLS[index].kind,started=now}
    else start_icon_echo(index,0) end
    confirm_fx=nil
    cast={started=now,duration=Rules.PLAY_MS,actor="both",round_actions=result.actions,power=result.power}
    if result.winner==1 or result.winner==2 then
        local clash=result.actions[1]=="wave" and result.actions[2]=="wave"
        settlement={kind=result.winner==1 and "win" or "lose",
            started=now+(clash and 1600 or 1500),clash=clash}
    else settlement=nil end
    print(string.format("H2_QI_DUEL_ROUND round=%d player=%s enemy=%s damage=%d/%d",
        battle.number,result.actions[1],result.actions[2],result.damage[1],result.damage[2]))
end
local function apply_result(now)
    if battle.phase=="play" and not battle.applied and now-battle.started>=750 then
        if sfx and sfx.cue then
            if battle.result.broken[1] or battle.result.broken[2] then sfx.cue("guard_break",now)
            elseif battle.result.damage[1]>0 or battle.result.damage[2]>0 then sfx.cue("hurt",now) end
        end
        local new=battle.result.players
        if qi~=new[1].qi then start_meter_fx("charge",qi,new[1].qi) end
        if player_hp~=new[1].hp then start_meter_fx("player",player_hp,new[1].hp) end
        if enemy_hp~=new[2].hp then start_meter_fx("enemy",enemy_hp,new[2].hp) end
        battle.players=new;qi,player_hp,enemy_hp=new[1].qi,new[1].hp,new[2].hp
        battle.applied=true
        print(string.format("H2_QI_DUEL_STATE hp=%d/%d qi=%d/%d shield=%d/%d",
            player_hp,enemy_hp,qi,new[2].qi,new[1].shield_cd,new[2].shield_cd))
    end
end
local function update_network(now)
    local state,connected,central=Link.state()
    if not network then
        if connected then
            network=Protocol.new(central,now,{nonce=Link.nonce,digest=Link.digest,
                candidate=function() return SKILLS[selected].kind end,
                send=function(m) return Link.send(json.encode(m)) end})
        elseif state=="error" or state=="idle" then error("PAIRING ENDED") end
        return
    end
    if not connected then error("CONNECTION LOST") end
    for _=1,8 do
        local bytes=Link.receive();if not bytes then break end
        network:receive(json.decode(bytes),now);network:tick(now)
    end
    network:tick(now)
    if network.error then error(network.error) end
    local side=network.side
    if battle then apply_result(now) end
    if network.phase=="choose" then
        if not battle or battle.number~=network.round then
            battle={players={network.players[side],network.players[3-side]},number=network.round,phase="select",
                round={deadline=network.deadline,choices={}}}
            qi,player_hp,enemy_hp=battle.players[1].qi,battle.players[1].hp,battle.players[2].hp
            cast=nil;confirm_fx=nil;page="battle"
        end
        battle.round.choices[1]=network.commits[side] and true or nil
    end
    local result,start=network:result(now)
    if result then
        local local_result={winner=result.winner=="draw" and "draw" or result.winner and
            (result.winner==side and 1 or 2) or nil}
        for _,name in ipairs({"players","actions","damage","power","broken"}) do
            local_result[name]={result[name][side],result[name][3-side]}
        end
        play_result(local_result,start)
    end
    if network.phase=="over" and battle then battle.phase="over";cast=nil
    elseif network.phase=="ready" and battle then battle.phase="waiting";cast=nil end
end
local function update_battle(now)
    if not PLAY_GAME then return end
    if intro and intro.phase~="done" then Intro.update(now);return end
    if page~="battle" then return end
    if mode=="void" then
        local ok,err=pcall(update_network,now)
        if not ok then
            local message=tostring(err):match("([^:]+)$") or "LINK ERROR"
            print("H2_QI_DUEL_LINK_ERROR "..message)
            leave_link()
        end
        return
    end
    if not battle then return end
    if battle.phase=="select" then
        if now>=battle.bot_at then Rules.lock(battle.round,battle.number,2,battle.bot_choice,now) end
        local result=Rules.finish(battle.round,battle.players,now,{SKILLS[selected].kind,battle.bot_choice})
        if result then
            play_result(result,now)
        end
    elseif battle.phase=="play" then
        apply_result(now)
        if now-battle.started>=Rules.PLAY_MS then
            cast=nil
            if battle.result.winner then battle.phase="over"
            else battle.number=battle.number+1;begin_round(now) end
        end
    end
end

-- Capture probes use the same transition as real pointer-down events.
if options.health_fx and options.health_fx ~= "" then
    local side,action=options.health_fx:match("^(%a+)%-(%a+)$")
    assert((side=="player" or side=="enemy") and (action=="up" or action=="down"),"invalid health effect")
    change_meter(side,action=="up" and 1 or -1)
    meter_fx[side].started=0
end
if options.charge_fx and options.charge_fx~="" then
    assert(options.charge_fx=="up" or options.charge_fx=="down","invalid charge effect")
    change_meter("charge",options.charge_fx=="up" and 1 or -1)
    if meter_fx.charge then meter_fx.charge.started=0 end
end

local function point_inside(x, y, left, top, width, height)
    return x >= left and x <= left + width and y >= top and y <= top + height
end

local function resolve_touch_target(x, y)
    if PLAY_GAME then return nil,nil end
    local function hit(group,px,py,w,h)
        if not H106 then return point_inside(x,y,px,py,w,h),px+w*.5 end
        local t=layouts[group];local s,tx,ty=t[1],t[2],t[3]
        return point_inside(x,y,px*s+tx,py*s+ty,w*s,h*s),(px+w*.5)*s+tx
    end
    local hud=inspector_layer=="full" or inspector_layer=="hud" or inspector_layer=="scene8" or inspector_layer=="scene11"
    local inside,mid=hit("player",PLAYER_HUD_X,PLAYER_HUD_Y,HUD_W,HUD_H)
    if hud and inside then
        return "player", mid
    end
    inside,mid=hit("enemy",ENEMY_HUD_X,ENEMY_HUD_Y,HUD_W,HUD_H)
    if hud and inside then
        return "enemy", mid
    end
    local charge=inspector_layer=="full" or inspector_layer=="charge-cells" or
        inspector_layer=="charge-base" or inspector_layer=="scene11" or inspector_layer=="carousel"
    inside,mid=hit("carousel",74,336,220,39)
    if charge and inside then return "charge",mid end
    return nil, nil
end

local click_zone,click_cancelled=nil,false
local intro_pressed=false
local function intro_touch(info)
    if not intro or intro.phase~="ready" then intro_pressed=false;return end
    if info.just_pressed then intro_pressed=true end
    if info.just_released then
        if intro_pressed then Intro.start(scene_time()) end
        intro_pressed=false
    end
end
local function carousel_click_zone(x,y)
    if x<0 or x>W or y<375 or y>H then return nil end
    return x<CAROUSEL_CX-52 and -1 or (x>CAROUSEL_CX+52 and 1 or 0)
end

local function handle_desktop_click(info,x,y)
    if info.just_pressed then
        click_zone,click_cancelled=nil,false
        local target,mid=resolve_touch_target(info.x,info.y)
        if target then
            change_meter(target,info.x<mid and -1 or 1)
        elseif inspector_layer=="full" or inspector_layer=="carousel" then
            click_zone=carousel_click_zone(x,y)
            touch_start_x,touch_start_y=x,y
        end
    end
    if click_zone~=nil and (info.pressed or info.just_released) then
        if (x-touch_start_x)^2+(y-touch_start_y)^2>12^2 then click_cancelled=true end
    end
    if info.just_released then
        if click_zone~=nil and not click_cancelled and carousel_click_zone(x,y)==click_zone then
            if click_zone==0 then
                if submit_action(SKILLS[selected].kind,scene_time()) then start_icon_echo(selected,0) end
            else
                -- Positive angle moves the icons physically right/clockwise.
                -- Compensate the new selected index to keep motion continuous.
                selected=skill_index(selected-click_zone)
                carousel_offset=carousel_offset-click_zone*CAROUSEL_RADIUS*CAROUSEL_STEP
                print(string.format("H2_QI_DUEL_CLICK direction=%s selected=%s",
                    click_zone<0 and "left" or "right",SKILLS[selected].kind))
            end
        end
        click_zone=nil
    end
    carousel_offset=fixed_drag or carousel_offset*.76
end

local function handle_touch(info)
    if PLAY_GAME and settlement then
        if info.just_pressed then begin_settlement_exit(scene_time()) end
        return
    end
    if PLAY_GAME and intro and intro.phase~="done" then return intro_touch(info) end
    if controls_locked() then
        click_zone=nil;touch_tracking=false;gesture_mode=nil;gesture_skill=nil
        carousel_offset=carousel_offset*.76
        return
    end
    local x,y=info.x,info.y
    if H106 then
        local t=layouts.carousel;x,y=(x-t[2])/t[1],(y-t[3])/t[1]
    end
    if DESKTOP_CLICKS then return handle_desktop_click(info,x,y) end
    if info.just_pressed then
        touch_start_x, touch_start_y = x, y
        gesture_mode,gesture_skill=nil,nil
        lifted_x,lifted_y=0,0
        touch_target = resolve_touch_target(info.x, info.y)
        if touch_target~=nil then
            local _,midpoint=resolve_touch_target(info.x,info.y)
            change_meter(touch_target,info.x<midpoint and -1 or 1)
            touch_target,touch_tracking=nil,false
            return
        end
        touch_tracking = (inspector_layer=="full" or inspector_layer=="carousel") and y >= 315
        if touch_tracking then
            -- Only a gesture starting on an actual icon may cast. The rest of
            -- the bottom area still rotates the wheel, but cannot fire a skill.
            for relative=-1,1 do
                local angle=relative*CAROUSEL_STEP+carousel_offset/CAROUSEL_RADIUS
                local ix=CAROUSEL_CX+math.sin(angle)*CAROUSEL_RADIUS
                local iy=CAROUSEL_CY-math.cos(angle)*CAROUSEL_RADIUS
                local radius=relative==0 and 48 or 34
                if (x-ix)^2+(y-iy)^2<=radius^2 then
                    gesture_skill=skill_index(selected+relative)
                    break
                end
            end
        end
    end

    if touch_tracking and (info.pressed or info.just_released) then
        local dx,dy=x-touch_start_x,y-touch_start_y
        if gesture_skill and dy < -12 and -dy>math.abs(dx)*1.15 and gesture_mode~="rotate" then
            gesture_mode="cast"
        elseif gesture_mode==nil and math.abs(dx)>16 and math.abs(dx)>math.abs(dy)*1.15 then
            gesture_mode="rotate"
        end
        if gesture_mode=="cast" then
            lifted_x,lifted_y=clamp(dx,-55,55),clamp(dy,-180,30)
            target_drag=0
        else
            target_drag=clamp(dx,-90,90)
        end
    end

    if info.just_released then
        if touch_tracking then
            -- SDL may deliver the final position only on UP for a quick drag.
            -- Include that endpoint instead of requiring an intermediate MOVE.
            if gesture_mode=="cast" then
                if touch_start_y-y>=CAST_LIFT and submit_action(SKILLS[gesture_skill].kind,scene_time()) then
                    selected=gesture_skill
                    start_icon_echo(selected,-lifted_y/90)
                end
            else
                target_drag=clamp(x-touch_start_x,-90,90)
                if target_drag < -SWIPE_THRESHOLD then selected = skill_index(selected + 1) end
                if target_drag > SWIPE_THRESHOLD then selected = skill_index(selected - 1) end
                print(string.format("[qi-duel] selected=%s swipe_x=%.1f->%.1f", SKILLS[selected].kind,touch_start_x,x))
            end
        end
        target_drag = 0
        touch_target, touch_tracking = nil, false
        gesture_mode,gesture_skill=nil,nil
    end
    if not touch_tracking then lifted_x,lifted_y=lifted_x*.65,lifted_y*.65 end

    carousel_offset = fixed_drag or (carousel_offset+(target_drag-carousel_offset)*.24)
end

-- Logical next / previous / confirm IDs; the H106 launcher maps Right / Left / Record.
if H106 then
    local runtime=require("runtime")
    local held={}
    local function key_down(id)
        if held[id] then return end
        held[id]=true
        if PLAY_GAME and settlement then
            if id==11 then begin_settlement_exit(scene_time()) end
            return
        end
        if PLAY_GAME and intro and intro.phase~="done" then
            if id==11 then Intro.start(scene_time()) end
            return
        end
        if inspector_layer~="full" and inspector_layer~="carousel" then return end
        if controls_locked() then return end
        touch_tracking,gesture_mode,gesture_skill=false,nil,nil
        click_zone=nil
        target_drag,lifted_x,lifted_y=0,0,0
        if id==9 or id==10 then
            local direction=id==9 and 1 or -1
            selected=skill_index(selected+direction)
            carousel_offset=direction*CAROUSEL_RADIUS*CAROUSEL_STEP
            print(string.format("H2_QI_DUEL_BUTTON key=%s selected=%s",id==9 and "right" or "left",SKILLS[selected].kind))
        elseif id==11 and submit_action(SKILLS[selected].kind,scene_time()) then
            start_icon_echo(selected,0)
        end
    end
    for _,id in ipairs({9,10,11}) do
        -- Subscribe directly: the legacy state-query proxy binds only one button.
        -- Touch-only hosts may not expose the H106 hardware components.
        local ok,result=pcall(runtime.components.on,id,runtime.event.BUTTON_DOWN,function() key_down(id) end)
        if ok then
            runtime.components.on(id,runtime.event.BUTTON_UP,function() held[id]=nil end)
        elseif not tostring(result):find("unknown Runtime component id",1,true) then
            error(result)
        end
    end
end

local function draw_wall_lights(now_ms)
    for i = 1, #wall_lights do
        local lamp = wall_lights[i]
        local pulse = math.max(0, math.sin(now_ms * .001 * lamp.rate + lamp.phase))
        local brightness = pulse ^ 2.55 * (.58 + lamp.depth * .41)
        wall_gains[i] = brightness < .018 and 0 or brightness
    end
    display.draw_light_atlas("@qi-duel/wall-lights.h2lf", wall_gains)
end

local function draw_arena_lights(now_ms)
    local t = now_ms * .001
    for i = 1, #arena_lights do
        local lamp = arena_lights[i]
        local brightness = 1
        if lamp.kind ~= "base" then
            local wave = .5 + .5 * math.sin(t * lamp.rate + lamp.phase)
            if lamp.kind == "ring" then
                local secondary = .72 + .28 * math.sin(t * (lamp.rate * .37 + .31) + lamp.phase * 1.7)
                brightness = (.08 + .92 * wave * wave) * secondary
                if lamp.faulty then
                    local contact = .5 + .5 * math.sin(t * lamp.contact_rate + lamp.phase)
                    brightness = brightness * (.07 + .93 * contact ^ 7)
                end
            elseif lamp.kind == "radial" then
                brightness = .06 + .94 * wave * wave
                if lamp.faulty then
                    local contact = .5 + .5 * math.sin(t * lamp.contact_rate + lamp.phase * 1.3)
                    brightness = brightness * (.06 + .94 * contact ^ 8)
                end
            else
                brightness = wave ^ 5
                if brightness < .018 then brightness = 0 end
            end
        end
        arena_gains[i] = brightness
    end
    -- The browser's full scene inherits square caps from its wall pass;
    -- its wheel-only inspector uses the default butt caps. Preserve both.
    display.draw_light_atlas(inspector_layer == "wheel" and
        "@qi-duel/arena-lights.h2lf" or "@qi-duel/arena-lights-full.h2lf", arena_gains)
end

local color_cache = {}
local function hsl(hue, lightness)
    local key = hue * 1000 + lightness * 100
    if color_cache[key] then return color_cache[key] end
    local c = 1 - math.abs(2 * lightness - 1)
    local x = c * (1 - math.abs((hue / 60) % 2 - 1))
    local m = lightness - c * .5
    local r,g,b = 0,0,0
    if hue < 60 then r,g=c,x elseif hue < 120 then r,g=x,c
    elseif hue < 180 then g,b=c,x elseif hue < 240 then g,b=x,c
    elseif hue < 300 then r,b=x,c else r,b=c,x end
    local result = rgb((r+m)*255,(g+m)*255,(b+m)*255)
    color_cache[key] = result
    return result
end
local particle_tilt_value=1
function Intro.camera_tilt(now)
    if not intro or intro.phase=="done" or intro.phase=="fade" then return 1 end
    if intro.phase~="tilt" then return 0 end
    return ease((now-intro.phase_started)/Intro.TILT_MS)
end
function Intro.scene_fade(now)
    if not intro or intro.phase=="done" then return 1 end
    if intro.phase~="fade" then return 0 end
    return ease((now-intro.phase_started)/Intro.FADE_MS)
end
local function arena_project_45(x,z) return {184+420*x/z,187+624/z} end
local function arena_project_top(x,z) return {184+x*36,220+(z-8)*36} end
-- The entry camera starts directly above the arena. Every particle uses the
-- same clock, size, colour and radius as its diametrically opposite partner,
-- so the waiting pattern is exactly point-symmetric instead of merely radial.
function Intro.top_particle(index,t)
    local zero=index-1
    local pair=math.floor(zero/2)
    local seed=space_particles[pair*2+1]
    local angle=space_particles[index].top_angle
    local progress=(seed.p0+t*seed.speed*.67)%1
    local radius=space_particles[index].top_extent
    return progress,angle,radius,seed.size,seed.hue
end
local function smoothstep(value) return value*value*(3-2*value) end

local function draw_dust(now_ms)
    local t=now_ms*.001
    for index,p in ipairs(ring_dust) do
        local i=index-1
        local a=p.a+t*p.speed*(i%2==1 and 1 or -1)
        local radius=28+p.r*142
        local twinkle=.35+.65*math.abs(math.sin(t*2.4+i))
        display.add_disc(184+math.cos(a)*radius,265+math.sin(a)*radius*.235,
            p.size,hsl(p.hue,.66),twinkle*.7)
    end
end
-- Keep one command array; the native batch preserves per-segment halo/core
-- order. Taper coefficients depend only on segment count, not the frame.
local draw_particle_path
do
local particle_lines,particle_line_count,particle_tapers={},0,{}
local function append_particle_line(a,b,width,color,alpha)
    local n=particle_line_count*9
    particle_lines[n+1],particle_lines[n+2]=a[1],a[2]
    particle_lines[n+3],particle_lines[n+4]=b[1],b[2]
    particle_lines[n+5]=width
    particle_lines[n+6],particle_lines[n+7],particle_lines[n+8]=color.r,color.g,color.b
    particle_lines[n+9]=alpha
    particle_line_count=particle_line_count+1
end
draw_particle_path=function(points,width,hue,alpha)
    if Intro.history_active then
        local head=points[#points]
        local p=space_particles[Intro.history_index]
        if not p.history_halo then p.history_halo=hsl(hue,.58);p.history_core=hsl(hue,.76) end
        local halo,core=p.history_halo,p.history_core
        local x,y=head[1],head[2]
        local previous=p.history_x and p.history_progress<=Intro.history_progress
        local core_width=math.max(.18,width)
        local gain=Intro.history_gain or 1
        local core_gain,halo_gain=1,1
        if previous then
            local distance=math.sqrt((x-p.history_x)^2+(y-p.history_y)^2)
            -- A short new segment overlaps more retained round caps. Account
            -- for travel and cap width so slow frames do not brighten tails.
            core_gain=(distance+core_width*gain)/(distance+core_width)
            halo_gain=(distance+width*4.4*gain)/(distance+width*4.4)
        end
        local core_alpha=math.min(1,alpha*1.2*core_gain)
        local halo_alpha=math.min(1,alpha*.18*1.2*halo_gain)
        if previous then
            display.add_line(p.history_x,p.history_y,x,y,width*4.4,halo,halo_alpha)
            display.add_line(p.history_x,p.history_y,x,y,core_width,core,core_alpha)
        else
            display.add_disc(x,y,width*2.2,halo,halo_alpha)
            display.add_disc(x,y,core_width*.5,core,core_alpha)
        end
        p.history_x,p.history_y,p.history_progress=x,y,Intro.history_progress
        return
    end
    particle_line_count=0
    local count=#points-1
    local coefficients=particle_tapers[count]
    if not coefficients then
        coefficients={}
        for i=1,count do local u=i/count;coefficients[i]={.12+.88*u^1.35,u^1.7} end
        particle_tapers[count]=coefficients
    end
    local halo,core=hsl(hue,.58),hsl(hue,.76)
    for i=2,#points do
        local taper=coefficients[i-1][1]
        local segment_alpha=alpha*coefficients[i-1][2]
        local a,b=points[i-1],points[i]
        append_particle_line(a,b,width*taper*4.4,halo,segment_alpha*.18)
        append_particle_line(a,b,math.max(.18,width*taper),core,segment_alpha)
    end
    display.add_lines(particle_lines,particle_line_count)
end
end
local function draw_space_particles(now_ms)
    particle_tilt_value=Intro.camera_tilt(now_ms)
    local particle_ms=now_ms
    if intro then
        particle_ms=intro.particle_ms
        if intro.phase=="done" then particle_ms=particle_ms+now_ms-intro.last_ms end
    end
    local t=particle_ms*.001
    if Intro.history_active then
        -- Top-down history needs only the current head. Reuse its storage;
        -- tilted projections and backwards tail samples belong to the full path.
        for index,p in ipairs(space_particles) do
            local progress,_,extent,size,hue=Intro.top_particle(index,t)
            local fraction,opacity=1,1
            local visible=true
            if index<=52 then
                if progress<.58 then
                    local grow=smoothstep(progress/.58)
                    fraction=.24+grow*.76;opacity=.28+grow*.72
                else
                    local inverse=1-(progress-.58)/.42
                    fraction=inverse^1.45;opacity=inverse^1.7
                end
                local final_progress=(p.p0+t*p.speed*.72)%1
                visible=8-p.forward*smoothstep(final_progress)*p.final_extent>1.15
            else
                if progress<.36 then
                    local grow=smoothstep(progress/.36)
                    fraction=.24+grow*.76;opacity=.28+grow*.72
                end
                if progress>.80 then opacity=opacity*clamp((1-progress)/.20,0,1) end
            end
            if visible then
                local radius=smoothstep(progress)*extent
                local points=p.history_points
                if not points then points={{0,0}};p.history_points=points end
                points[1][1]=184+radius*p.top_cos*36
                points[1][2]=220+((8+radius*p.top_sin)-8)*36
                local width
                if index<=52 then
                    width=math.min(14,(.44+size*.36)*fraction)
                    opacity=math.min(1,opacity*(.35+.28))
                else
                    width=math.max(.16,(.46+size*.36)*fraction)
                    opacity=math.min(1,opacity*(.38+.55))
                end
                Intro.history_index=index;Intro.history_progress=progress
                draw_particle_path(points,width,hue,opacity)
            end
        end
        return
    end
    for index,p in ipairs(space_particles) do
        local i=index-1
        Intro.history_index=index
        local top_progress,top_angle,top_extent,top_size,top_hue=
            Intro.top_particle(index,t)
        Intro.history_progress=top_progress
        local top_cos=p.top_cos
        local top_sin=p.top_sin
        if i<52 then
            local final_progress=(p.p0+t*p.speed*.72)%1
            local progress=top_progress+(final_progress-top_progress)*particle_tilt_value
            local size_fraction,opacity
            if progress<.58 then
                local grow=smoothstep(progress/.58)
                size_fraction=.24+grow*.76;opacity=.28+grow*.72
            else
                local inverse=1-(progress-.58)/.42
                size_fraction=inverse^1.45;opacity=inverse^1.7
            end
            local lateral=p.lateral
            local forward=p.forward
            local final_distance=smoothstep(final_progress)*p.final_extent
            local top_distance=smoothstep(top_progress)*top_extent
            local world_z=8-forward*final_distance
            if world_z>1.15 then
                local final_perspective=8/world_z
                local perspective=1+(final_perspective-1)*particle_tilt_value
                local mix=particle_tilt_value
                local particle_size=top_size+(p.size-top_size)*mix
                local trail=(.21+particle_size*.11)*(.5+perspective^.82*.82)
                local final_tail_distance=math.max(0,final_distance-trail)
                local top_tail_distance=math.max(0,top_distance-trail)
                local top_head=arena_project_top(top_distance*top_cos,
                    8+top_distance*top_sin)
                local top_tail=arena_project_top(top_tail_distance*top_cos,
                    8+top_tail_distance*top_sin)
                local final_head=arena_project_45(lateral*final_distance,world_z)
                local final_tail=arena_project_45(lateral*final_tail_distance,
                    8-forward*final_tail_distance)
                local head={top_head[1]+(final_head[1]-top_head[1])*mix,
                    top_head[2]+(final_head[2]-top_head[2])*mix}
                local tail={top_tail[1]+(final_tail[1]-top_tail[1])*mix,
                    top_tail[2]+(final_tail[2]-top_tail[2])*mix}
                local points={}
                if Intro.history_active then points[1]=head else
                    for step=0,8 do
                        points[#points+1]={tail[1]+(head[1]-tail[1])*step/8,tail[2]+(head[2]-tail[2])*step/8}
                    end
                end
                local width=math.min(14,(.44+particle_size*.36)*perspective^1.58*size_fraction)
                draw_particle_path(points,width,top_hue,math.min(1,opacity*(.35+perspective*.28)))
            end
        else
            local final_progress=(p.p0+t*p.speed*.62)%1
            local progress=top_progress+(final_progress-top_progress)*particle_tilt_value
            local size_fraction,opacity=1,1
            if progress<.36 then
                local grow=smoothstep(progress/.36)
                size_fraction=.24+grow*.76;opacity=.28+grow*.72
            end
            local route=smoothstep(progress)
            local angle=p.path_angle
            local angle_cos=p.path_cos
            local angle_sin=p.path_sin
            local hit_radius=p.hit_radius
            local final_base_perspective=8/(8+angle_sin*hit_radius)
            local turn_radius=hit_radius-.34
            local turn_start=arena_project_45(turn_radius*angle_cos,
                8+turn_radius*angle_sin)
            local before=arena_project_45((turn_radius-.12)*angle_cos,
                8+(turn_radius-.12)*angle_sin)
            local radial_x,radial_y=turn_start[1]-before[1],turn_start[2]-before[2]
            local hit=arena_project_45(hit_radius*angle_cos,
                8+hit_radius*angle_sin)
            local turn_end={hit[1],hit[2]-9}
            local raw_y=math.abs(radial_x)>.5 and
                turn_start[2]+(turn_end[1]-turn_start[1])*radial_y/radial_x or
                (turn_start[2]+turn_end[2])*.5
            local control={turn_end[1],clamp(raw_y,math.min(turn_start[2],turn_end[2]),math.max(turn_start[2],turn_end[2]))}
            local wall_rise=turn_end[2]+34+p.wall_extra
            local function final_point_at(u)
                if u<=.56 then
                    local radius=turn_radius*u/.56
                    local point=arena_project_45(radius*angle_cos,
                        8+radius*angle_sin)
                    point[3]=8/(8+angle_sin*radius)
                    return point
                elseif u<=.72 then
                    local q=(u-.56)/(.72-.56);local v=1-q
                    return {v*v*turn_start[1]+2*v*q*control[1]+q*q*turn_end[1],
                        v*v*turn_start[2]+2*v*q*control[2]+q*q*turn_end[2],final_base_perspective*(1-q*.1)}
                else
                    local wall_u=(u-.72)/(1-.72)
                    return {turn_end[1],turn_end[2]-wall_rise*wall_u,final_base_perspective*(1-wall_u*.66)}
                end
            end
            local function point_at(u)
                local flat_radius=top_extent*u
                local top=arena_project_top(flat_radius*top_cos,
                    8+flat_radius*top_sin)
                if particle_tilt_value==0 then return {top[1],top[2],1} end
                local tilted=final_point_at(u)
                local mix=particle_tilt_value
                return {top[1]+(tilted[1]-top[1])*mix,
                    top[2]+(tilted[2]-top[2])*mix,
                    1+(tilted[3]-1)*mix}
            end
            local head=point_at(route);local perspective=head[3]
            if head[2]<42 then
                local inverse=1-math.min(1,(42-head[2])/78)
                local edge_size=inverse^1.35
                local edge_opacity=inverse^1.65
                size_fraction=size_fraction*(1+(edge_size-1)*particle_tilt_value)
                opacity=opacity*(1+(edge_opacity-1)*particle_tilt_value)
            end
            if progress>.80 and particle_tilt_value<1 then
                local edge=clamp((1-progress)/.20,0,1)
                opacity=opacity*(edge+(1-edge)*particle_tilt_value)
            end
            local wanted=(8+perspective*24)*(.34+size_fraction*.66)
            local points={head};local sampled,accumulated=route,0
            for _=1,Intro.history_active and 0 or 22 do
                if sampled<=0 or accumulated>=wanted then break end
                sampled=math.max(0,sampled-.005)
                local point=point_at(sampled);local first=points[1]
                accumulated=accumulated+math.sqrt((first[1]-point[1])^2+(first[2]-point[2])^2)
                table.insert(points,1,point)
            end
            local particle_size=top_size+(p.size-top_size)*particle_tilt_value
            local width=math.max(.16,(.46+particle_size*.36)*perspective^1.58*size_fraction)
            draw_particle_path(points,width,top_hue,math.min(1,opacity*(.38+perspective*.55)))
        end
    end
end

-- The pairing surface is its own retained history. Scene/camera changes clear
-- this history so old screen-space trails never leak into the next scene.
function Intro.draw_history(now_ms)
    local current=intro.particle_ms
    local reset=not Intro.history_last or current<Intro.history_last
    display.begin_composite(reset and true or "retain")
    viewport("scene")
    Intro.history_active=true
    if reset then
        for _,p in ipairs(space_particles) do p.history_x=nil end
        -- Warm the existing trajectories, including particles alive at t=0.
        -- This prepares history once, outside subsequent moving frames.
        Intro.history_gain=1
        for sample=current-594,current,33 do
            display.fade_composite(1-.85^(33*.06))
            intro.particle_ms=sample;draw_space_particles(now_ms)
        end
        intro.particle_ms=current
    else
        local elapsed=current-Intro.history_last
        Intro.history_gain=elapsed/33
        display.fade_composite(1-.85^(elapsed*.06))
        draw_space_particles(now_ms)
    end
    Intro.history_active=false;Intro.history_last=current
    display.end_composite()
end

local function action_state(now_ms,actor)
    if not cast or (cast.actor~="both" and cast.actor~=actor) then return nil end
    local age=now_ms-cast.started
    if age<0 or age>=cast.duration then return nil end
    local side=actor=="player" and 1 or 2
    local kind=cast.round_actions and cast.round_actions[side] or cast.kind
    local spec=ACTIONS[kind]
    if not spec then return nil end -- Empty / invalid moves keep the idle pose.
    if age>=spec.windup+spec.hold+spec.recover then return nil end
    local step,amount
    if age<spec.windup then
        amount=age/spec.windup
        step=math.min(3,math.floor(amount*4))
    elseif age<spec.windup+spec.hold then
        amount=1;step=3
    else
        amount=1-(age-spec.windup-spec.hold)/spec.recover
        step=math.min(3,math.floor(amount*4))
        -- Wave returns through a relaxed extension, not back to its hip windup.
        if kind=="wave" and step==1 then step=0 end
    end
    return {index=spec.row*4+step+1,step=step,amount=amount,age=age,spec=spec,kind=kind,
        combo=cast.power and cast.power[side]==3,
        held=age>=spec.windup and age<spec.windup+spec.hold}
end

local function draw_action_body(now_ms,actor)
    local state=action_state(now_ms,actor)
    if not state or state.step==0 then return false end
    -- Whole-body / paired-hand sprites: no disconnected limb transforms.
    -- Pose changes are discrete drawings; the continuous bob/recoil and VFX
    -- keep moving each display frame. No pose crossfade / doubled fingers.
    local pulse=math.sin(state.age*.025)
    if actor=="opponent" then
        local size=182
        local down=state.kind=="charge" and state.amount*3 or 0
        display.draw_sprite_atlas("@qi-duel/action-opponent.h2rs",state.index,state.index,0,
            184-size/2,265-size*.92+down+state.amount*pulse*.6,size/192)
    else
        local size=340
        local recoil=state.kind=="wave" and state.held and math.exp(-(state.age-state.spec.windup)/110)*7 or 0
        display.draw_sprite_atlas("@qi-duel/action-hands.h2rs",state.index,state.index,0,
            184-size/2,132+recoil+state.amount*pulse*.7,size/192)
    end
    return true
end

local function draw_opponent(now_ms)
    if draw_action_body(now_ms,"opponent") then return end
    local t=now_ms*.001
    local bob=math.sin(t*4.1)*1.6;local sway=math.sin(t*2.2)*1.15
    local lean=math.sin(t*2.2)*.009
    local scale=(1+math.sin(t*3.05)*.0045)*156/1285
    local a,b=math.cos(lean)*scale,math.sin(lean)*scale
    display.draw_affine_asset("@qi-duel/opponent.h2r8",a,b,-b,a,
        184+sway-a*1224*.5+b*1285,265+bob-b*1224*.5-a*1285)
end
local function draw_player_hands(now_ms,side)
    if action_state(now_ms,"player") and action_state(now_ms,"player").step>0 then
        if side~="right" then draw_action_body(now_ms,"player") end
        return
    end
    local t=now_ms*.001;local breathe,spread=math.sin(t*3.3),math.sin(t*1.8)
    if side=="all" or side=="left" then
        local angle=-.035+breathe*.007;local a,b=math.cos(angle),math.sin(angle)
        display.draw_affine_asset("@qi-duel/hand-left.h2r8",a,b,-b,a,
            -5-spread*2-12*a,176+breathe*2.5-12*b)
    end
    if side=="all" or side=="right" then
        local angle=.035-breathe*.007;local a,b=math.cos(angle),math.sin(angle)
        display.draw_affine_asset("@qi-duel/hand-right.h2r8",a,b,-b,a,
            242+spread*2,176-breathe*2.5)
    end
end

local function screen_point(group,x,y)
    if not H106 then return x,y,1 end
    local t=layouts[group]
    return x*t[1]+t[2],y*t[1]+t[3],t[1]
end

local function energy_core(x,y,r,color,alpha)
    display.add_disc(x,y,r*2.3,color,alpha*.05)
    display.add_disc(x,y,r*1.6,color,alpha*.16)
    display.add_disc(x,y,r,color,alpha*.55)
    display.add_disc(x,y,r*.45,COLOR.white,alpha*.9)
end

-- Authored anchors in the same design space as the reviewed pose artwork.
-- A beam originates at the palms, not at the character's torso center.
local function action_anchor(actor,state)
    local player=actor=="player"
    local x,y=184,player and 280 or 174
    if state.kind=="wave" then
        if player then
            x,y=state.step==1 and 219 or 184,state.step==1 and 299 or 285
        else
            x,y=state.step==1 and 188 or 156,state.step==1 and 176 or 153
        end
    elseif state.kind=="absorb" then
        x,y=player and 210 or 185,player and 300 or 157
    elseif state.kind=="guard" then
        x,y=184,player and 231 or 148
    end
    return screen_point(player and "hands" or "opponent",x,y)
end

local function draw_beam(ax,ay,bx,by,near_width,far_width,color,age,alpha)
    local dx,dy=bx-ax,by-ay
    local length=math.max(1,math.sqrt(dx*dx+dy*dy))
    local nx,ny=-dy/length,dx/length
    -- Short tapered strips preserve perspective without a full-screen blur.
    for i=0,13 do
        local u,v=i/14,(i+1)/14
        local x0,y0=ax+dx*u,ay+dy*u
        local x1,y1=ax+dx*v,ay+dy*v
        local w0,w1=near_width+(far_width-near_width)*u,near_width+(far_width-near_width)*v
        for layer=1,3 do
            local gain=layer==1 and 2.3 or layer==2 and 1 or .32
            local a=alpha*(layer==1 and .1 or layer==2 and .6 or .95)
            display.draw_polygon({{x0+nx*w0*gain,y0+ny*w0*gain},{x1+nx*w1*gain,y1+ny*w1*gain},
                {x1-nx*w1*gain,y1-ny*w1*gain},{x0-nx*w0*gain,y0-ny*w0*gain}},
                layer==3 and COLOR.white or color,a,color,0,0,color,0,0)
        end
    end
    for i=0,5 do
        local u=(age*.002+i/6)%1
        local v=math.min(1,u+.08)
        local side=math.sin(age*.025+i*3)*.5
        local w=near_width+(far_width-near_width)*u
        display.add_line(ax+dx*u+nx*w*side,ay+dy*u+ny*w*side,
            ax+dx*v+nx*w*side,ay+dy*v+ny*w*side,math.max(.6,w*.16),COLOR.white,alpha*.8)
    end
end

-- A deterministic, black-stage close-up for wave-vs-wave rounds. The same
-- renderer is exposed through --clash so desktop captures can be compared at
-- exact times without changing combat state.
function Intro.clash_state(now)
    local player_power,enemy_power,age
    if Intro.clash_probe then
        age=now%1080
        player_power=(Intro.clash_probe=="player-combo" or
            Intro.clash_probe=="both-combo") and 3 or 1
        enemy_power=(Intro.clash_probe=="enemy-combo" or
            Intro.clash_probe=="both-combo") and 3 or 1
    else
        if not cast or not cast.round_actions then return nil end
        local player_action=cast.round_actions[1]
        local enemy_action=cast.round_actions[2]
        if player_action~="wave" or enemy_action~="wave" then return nil end
        age=now-cast.started-ACTIONS.wave.windup
        if age<0 or age>=1120 then return nil end
        player_power=cast.power and cast.power[1] or 1
        enemy_power=cast.power and cast.power[2] or 1
    end
    return {age=age,player_power=player_power,enemy_power=enemy_power}
end

function Intro.draw_clash(now,state)
    local age=state.age
    local base
    if state.player_power==state.enemy_power then base=0
    elseif state.player_power>state.enemy_power then base=4
    else base=8 end
    local fade_started=760
    local impact=clamp((age-70)/80,0,1)
    local opacity=age<fade_started and 1 or 1-ease((age-fade_started)/360)
    local shake=(1.1+1.5*math.sin(age*.083)^2)*impact*opacity
    local sx=math.sin(age*.71)*shake
    local sy=math.cos(age*.57)*shake*.72
    local pulse=1+.008*impact*math.sin(age*.061)^2
    local frame_w,frame_h=H106 and 240 or 184,H106 and 240 or 224
    local scale=(H106 and 1 or 2)*pulse
    local x=(SCREEN_W-frame_w*scale)*.5+sx
    local y=(SCREEN_H-frame_h*scale)*.5+sy
    local atlas,lo,hi,blend
    if age>=fade_started and state.player_power==state.enemy_power then
        -- Fade the terminal clash directly instead of packaging four more
        -- copies of the same decaying geometry.
        atlas=H106 and "@qi-duel/beam-clash-h106.h2rs" or
            "@qi-duel/beam-clash-amoled.h2rs"
        lo,hi,blend=4,4,0
    else
        local phase=clamp(age/fade_started*3,0,3)
        lo=math.floor(phase);hi=math.min(3,lo+1);blend=ease(phase-lo)
        atlas=H106 and "@qi-duel/beam-clash-h106.h2rs" or
            "@qi-duel/beam-clash-amoled.h2rs"
        lo,hi=base+lo+1,base+hi+1
    end

    display.draw_sprite_atlas(atlas,lo,hi,blend,x,y,scale,opacity)
end

function Intro.settlement_state(now)
    if not settlement then return nil end
    if result_probe and not PLAY_GAME then
        -- Review mode repeats the one-shot entry after a short OLED-black gap;
        -- real game settlements remain lit until the player restarts.
        return {kind=settlement.kind,age=fixed_time_ms and now or now%3000-400}
    end
    local age=now-settlement.started
    if age<0 then return nil end
    return {kind=settlement.kind,age=age,
        exit_age=settlement.exit_started and now-settlement.exit_started or nil}
end

function Intro.clash_return_alpha(now)
    if not cast or not cast.round_actions or settlement then return 0 end
    if cast.round_actions[1]~="wave" or cast.round_actions[2]~="wave" then return 0 end
    local age=now-cast.started-ACTIONS.wave.windup
    if age<1120 or age>=1320 then return 0 end
    return 1-ease((age-1120)/200)
end

Intro.result_streaks={}
for i=1,(H106 and 25 or 30) do
    Intro.result_streaks[i]={
        lane=hash01(i*67+3)*2-1,
        phase=((i*7)%(H106 and 25 or 30))/(H106 and 25 or 30),
        speed=.30+hash01(i*107+17)*.52,
        length=.65+hash01(i*131+23)*.90,
        width=.70+hash01(i*43+7)^2*.90,
        color=((i-1)%5)+1,
        alpha=.60+hash01(i*181+37)*.40,
        pulse=.0022+hash01(i*211+47)*.0031,
    }
end
Intro.result_streak_crops={}
for row=0,9 do Intro.result_streak_crops[row+1]={0,row*48,160,48} end

local function result_light_bars(age,win,exit_age)
    local diagonal=math.sqrt(SCREEN_W*SCREEN_W+SCREEN_H*SCREEN_H)
    local entry=ease(age/180)
    -- A shared basis keeps every strip, trail and exit trajectory parallel.
    local tx,ty=math.cos(-.54),math.sin(-.54)
    local nx,ny=-ty,tx
    for _,streak in ipairs(Intro.result_streaks) do
        local phase
        if exit_age then
            local exit_started_age=math.max(0,age-exit_age)
            local phase_at_exit=(exit_started_age*.001*streak.speed+streak.phase)%1
            -- Existing streaks finish their route with a short exit boost, but
            -- never wrap back to phase zero: no new light is spawned.
            phase=phase_at_exit+exit_age*.001*(streak.speed+.75)
        else
            phase=(math.max(0,age)*.001*streak.speed+streak.phase)%1
        end
        if phase<1 then
            local along=(phase*2-1)*diagonal*.62
            local lane=streak.lane*diagonal*.30
            local cx=SCREEN_W*.5+tx*along+nx*lane
            local cy=SCREEN_H*.5+ty*along+ny*lane
            local pulse=.78+.22*math.sin(age*streak.pulse+streak.phase*TAU)^2
            local edge=clamp(math.sin(phase*math.pi)*3.2,0,1)
            local opacity=entry*streak.alpha*pulse*edge
            local scale_x=streak.length*(H106 and .90 or 1.20)
            local scale_y=streak.width*(H106 and .90 or 1.10)
            local a,b=tx*scale_x,ty*scale_x
            local c,d=nx*scale_y,ny*scale_y
            local row=(win and 0 or 5)+streak.color
            local source_y=(row-1)*48+24
            -- One pre-baked affine sprite contains the storyboard's slanted block,
            -- white core, layered stretched wakes and bloom. Each instance still
            -- owns its transform, speed, phase, scale, opacity and colour row.
            display.draw_affine_asset("@qi-duel/result-streaks.h2r8",a,b,c,d,
                cx-a*80-c*source_y,cy-b*80-d*source_y,
                Intro.result_streak_crops[row],opacity)
        end
    end
end

function Intro.draw_settlement(now,state)
    local age=state.age
    local win=state.kind=="win"
    result_light_bars(age,win,state.exit_age)

    local scale=H106 and 1 or 1.55
    local final_x=(SCREEN_W-224*scale)*.5
    local you_y=H106 and 52 or 105
    local result_y=H106 and 99 or 205
    local slide=ease(age/390)
    local distance=SCREEN_W+224*scale
    local exit=state.exit_age and ease(state.exit_age/500) or 0
    local you_x=final_x-distance*(1-slide)+distance*exit
    local result_x=final_x+distance*(1-slide)-distance*exit
    local light=ease((age-390)/190)
    local base=win and 1 or 5
    display.draw_sprite_atlas("@qi-duel/result-words.h2rs",base,base+1,light,
        you_x,you_y,scale,1)
    display.draw_sprite_atlas("@qi-duel/result-words.h2rs",base+2,base+3,light,
        result_x,result_y,scale,1)

    local flash=math.sin(clamp((age-390)/230,0,1)*math.pi)^3
    if flash>0 then
        display.add_disc(SCREEN_W*.5,SCREEN_H*.5,(12+flash*24)*(H106 and 1 or 1.5),
            COLOR.white,flash*.14)
    end
end
local function draw_action_effects(now_ms)
    if not cast then return end
    viewport("screen")
    for _,actor in ipairs({"opponent","player"}) do
        local state=action_state(now_ms,actor)
        if state and state.step>0 then
            local player=actor=="player"
            local x,y,s=action_anchor(actor,state)
            local color=player and COLOR.cyan or COLOR.violet
            local a=state.amount
            local age=state.age
            if state.kind=="charge" then
                y=y+35*s
                for i=0,15 do
                    local u=(age*.0008+i/16)%1
                    local side=i%2==0 and -1 or 1
                    local xx=x+side*(22+(i%4)*9+math.sin(u*4+i)*3)*s
                    local yy=y+(40-u*75)*s
                    display.add_line(xx,yy+9*s,xx+side*2*s,yy,1.3*s,color,a*math.sin(u*math.pi)*.75)
                end
                energy_core(x,y,5*s,color,a*(.65+.15*math.sin(age*.028)))
            elseif state.kind=="absorb" then
                -- Inward travel, only while holding the Tai Chi peak; never
                -- animate the discarded 'lower ball to abdomen' recovery.
                for i=0,17 do
                    local u=(age*.0007+i/18)%1
                    local angle=i*2.399+u*2.8
                    local radius=(5+(1-u)^1.6*58)*s
                    local tail=radius+7*s
                    display.add_line(x+math.cos(angle-.12)*tail,y+math.sin(angle-.12)*tail*.67,
                        x+math.cos(angle)*radius,y+math.sin(angle)*radius*.67,
                        (.55+u*.8)*s,color,a*math.sin(u*math.pi)*.85)
                end
                energy_core(x,y,(4+math.sin(age*.018)*.6)*s,color,a)
            elseif state.kind=="guard" then
                local beat=(age-state.spec.windup)%330
                if state.held and beat<160 then
                    local p=beat/160
                    for side=-1,1,2 do
                        local gx,gy=x+side*28*s,y-8*s
                        for i=0,5 do
                            local angle=i*TAU/6
                            local r=(3+p*17)*s
                            display.add_line(gx+math.cos(angle)*r,gy+math.sin(angle)*r,
                                gx+math.cos(angle)*(r+5*s),gy+math.sin(angle)*(r+5*s),
                                1.1*s,color,(1-p)*.8)
                        end
                    end
                end
            elseif state.kind=="wave" then
                if age<state.spec.windup then
                    energy_core(x,y,(4+a*5)*s,color,a)
                else
                    local elapsed=age-state.spec.windup
                    local life=clamp(elapsed/90,0,1)*clamp((state.spec.hold+100-elapsed)/160,0,1)
                    local pulse_index=0
                    if cast.round_actions then
                        pulse_index=math.floor(elapsed/200)
                        local pulse=elapsed%200
                        life=pulse_index<(state.combo and 3 or 1) and
                            clamp(pulse/35,0,1)*clamp((190-pulse)/70,0,1) or 0
                    end
                    local px,py=screen_point("hands",184,285)
                    local ex,ey=screen_point("opponent",156,153)
                    local tx,ty
                    local other=action_state(now_ms,player and "opponent" or "player")
                    local clash=cast.actor=="both" and (not cast.round_actions or
                        (other and other.kind=="wave" and (pulse_index==0 or other.combo)))
                    if clash then tx,ty=(px+ex)*.5,(py+ey)*.5
                    elseif player then tx,ty=ex,ey
                    else tx,ty=SCREEN_W*.5,SCREEN_H*.86 end
                    local reach=clamp(elapsed/140,0,1)
                    tx,ty=x+(tx-x)*reach,y+(ty-y)*reach
                    draw_beam(x,y,tx,ty,(player and 11 or 4)*s,
                        (player and 3 or 12)*s,color,elapsed,life)
                    energy_core(x,y,7*s,color,life)
                    if reach>=1 then energy_core(tx,ty,(6+math.sin(age*.055))*s,color,life) end
                end
            end
        end
    end
end

local function health_cell_points(panel_x, panel_y, side, index)
    local start_x = side == "player" and 68 or 9
    local width, cell_height, gap, slant = 20, 13, 2, 3
    local x = panel_x + start_x + (index - 1) * (width + gap)
    local y = panel_y + 22
    return {
        {x + slant, y}, {x + width, y},
        {x + width - slant, y + cell_height}, {x, y + cell_height},
    }
end

local function draw_health_panel(side, panel_x, panel_y, health, now_ms)
    local enemy = side == "enemy"
    local color = enemy and rgb(255,133,35) or rgb(70,226,255)
    local fx = meter_fx[side]
    local age=fx and now_ms-fx.started or math.huge
    local shake_duration=fx and fx.direction<0 and 280 or 220
    local shake=age>=0 and age<shake_duration and
        math.sin(age*.095)*(1-age/shake_duration)*(fx.direction<0 and 2.2 or 1.1) or 0
    local row=((enemy and 6 or 0)+health)*56
    display.draw_affine_asset("@qi-duel/hud-states.h2r8",1,0,0,1,
        panel_x+shake,panel_y-row,{0,row,190,56})
    if not fx then return end
    local duration=fx.direction>0 and 480 or 360
    if age<0 or age>duration then return end
    local p=age/duration
    local pulse=math.sin(math.pi*math.min(1,p))
    local points=health_cell_points(panel_x+shake,panel_y,side,fx.index)
    local white=rgb(255,255,255)
    display.draw_polygon(points,fx.direction>0 and white or color,
        (fx.direction>0 and .62 or .72)*(1-p),white,.9*(1-p),
        1+pulse,color,.9*(1-p),5+pulse*10)
    local cx=(points[1][1]+points[2][1]+points[3][1]+points[4][1])*.25
    local cy=(points[1][2]+points[2][2]+points[3][2]+points[4][2])*.25
    for n=0,3 do
        local a=n*math.pi*.5+.35
        local inner=9+p*2;local outer=inner+3+p*7
        display.over_line(cx+math.cos(a)*inner,cy+math.sin(a)*inner,
            cx+math.cos(a)*outer,cy+math.sin(a)*outer,1,color,.8*(1-p))
    end
end

local function draw_hud(now_ms)
    viewport("player")
    draw_health_panel("player", PLAYER_HUD_X, PLAYER_HUD_Y, player_hp, now_ms)
    viewport("enemy")
    draw_health_panel("enemy", ENEMY_HUD_X, ENEMY_HUD_Y, enemy_hp, now_ms)
end

local function orbit_point(cx, cy, radius, angle)
    return cx + math.sin(angle) * radius, cy - math.cos(angle) * radius
end

local function draw_carousel_frame()
    local s=METER_SCALE
    display.draw_affine_asset("@qi-duel/carousel-base.h2r8",s,0,0,s,
        CHARGE_CX+(-6-CHARGE_CX)*s,METER_ANCHOR_Y+(267-METER_ANCHOR_Y)*s)
end

local function draw_charge_cell(index, lit, scale, alpha, transient)
    local angle = -CHARGE_SPAN * 0.5 + (index - 1) * CHARGE_SPAN / (CHARGE_MAX-1)
    local x, y = orbit_point(CHARGE_CX, CHARGE_CY, CHARGE_RADIUS, angle)
    scale,alpha=(scale or 1)*METER_SCALE,alpha or 1
    -- Reuse the crystalline material, undo its baked orbit angle, then rotate
    -- onto the new five-cell circle. Empty and full share identical geometry.
    -- Reuse one authored crystal orientation; the affine transform rotates it
    -- around the meter without duplicating its paths in every state.
    local source_index=0
    local source_angle=-.37+source_index*.74/9
    local sx,sy=orbit_point(184,694,359,source_angle)
    -- Widen along each cell's tangent, not the screen's horizontal axis:
    -- R(new angle) * scale(width, height) * R(-baked angle).
    local width=1.65
    local ca,sa,cs,ss=math.cos(angle),math.sin(angle),math.cos(source_angle),math.sin(source_angle)
    local a,b=scale*(width*ca*cs+sa*ss),scale*(width*sa*cs-ca*ss)
    local c,d=scale*(width*ca*ss-sa*cs),scale*(width*sa*ss+ca*cs)
    for part=0,transient and 4 or 0 do
        local row=((transient and 20+part*10 or (lit and 10 or 0))+source_index)*45
        local ox,oy=math.floor(sx)-27-sx,math.floor(sy)-22-sy-row
        display.draw_affine_asset("@qi-duel/charge-cells.h2r8",a,b,c,d,
            x+a*ox+c*oy,y+b*ox+d*oy,{0,row,55,45},alpha)
    end
    return x,y
end

local function draw_charge_cells(now_ms)
    for i = 1, CHARGE_MAX do draw_charge_cell(i, i <= qi) end
    local fx=meter_fx.charge
    if not fx then return end
    local age=now_ms-fx.started
    local duration=fx.direction>0 and 520 or 420
    if age<0 or age>duration then return end
    local p=age/duration;local pulse=math.sin(math.pi*math.min(1,p))
    local x,y=draw_charge_cell(fx.index,true,
        fx.direction>0 and 1+pulse*.2 or 1-p*.18,
        fx.direction>0 and .72*(1-p) or 1-p,true)
    local color,shadow=rgb(122,245,255),rgb(76,236,255)
    for n=0,5 do
        local a=n*TAU/6+.18;local inner=12+p*4;local outer=inner+4+p*10
        display.glow_line(x+math.cos(a)*inner,y+math.sin(a)*inner,
            x+math.cos(a)*outer,y+math.sin(a)*outer,1.2,color,.9*(1-p),7+pulse*9,shadow)
    end
end

local focuses={}
for i=0,16 do focuses[#focuses+1]=i/32 end
focuses[#focuses+1]=.500001
for i=17,32 do focuses[#focuses+1]=i/32 end
local function draw_skill_symbol(index,angle)
    local x,y=orbit_point(CAROUSEL_CX,CAROUSEL_CY,CAROUSEL_RADIUS,angle)
    local kind=SKILLS[index].kind
    local unavailable=PLAY_GAME and battle and not Rules.available(battle.players[1],kind)
    if invalid_fx and invalid_fx.kind==kind then
        local age=scene_time()-invalid_fx.started
        if age>=0 and age<420 then x=x+math.sin(age*.095)*6*(1-age/420) end
    end
    local focus=math.max(0,1-math.abs(angle)/CAROUSEL_STEP)
    local direction=angle<-.035 and 1 or (angle>.035 and 2 or 0)
    local lo=1
    while lo<#focuses and focuses[lo+1]<=focus do lo=lo+1 end
    local hi=math.min(#focuses,lo+1)
    local blend=hi==lo and 0 or (focus-focuses[lo])/(focuses[hi]-focuses[lo])
    local base=((index-1)*3+direction)*#focuses
    local scale=SKILL_SCALE
    local atlas=unavailable and "@qi-duel/skill-styles.h2rs" or "@qi-duel/skill-colors.h2rs"
    local frame_lo,frame_hi=base+lo,base+hi
    if confirm_fx and confirm_fx.index==index and not unavailable then
        local age=math.max(0,scene_time()-confirm_fx.started)
        local frame=20
        if age<250 then
            local p=age/250
            frame=math.min(19,math.floor(p*20))
            -- Brief compression, overshoot, then settle without a new ring.
            local t=math.max(0,(p-.3)/.7)
            local bounce=p<.3 and -.1*math.sin(p/.3*math.pi/2) or -.1*math.cos(t*TAU)*(1-t)
            scale=scale*(1+bounce)
        end
        frame_lo=409+(index-1)*21+frame;frame_hi=frame_lo;blend=0
    end
    display.draw_sprite_atlas(atlas,frame_lo,frame_hi,blend,x-80*scale,y-80*scale,scale,unavailable and .3 or 1)
    if unavailable then
        local points={{x+3*scale,y-28*scale},{x-5*scale,y-12*scale},{x+4*scale,y-2*scale},{x-4*scale,y+11*scale},{x+3*scale,y+28*scale}}
        for i=1,#points-1 do
            local p,q=points[i],points[i+1]
            display.over_line(p[1],p[2],q[1],q[2],4,COLOR.black,.95)
            display.over_line(p[1]+2,p[2],q[1]+2,q[2],.65,rgb(130,140,150),.7)
        end
    elseif PLAY_GAME and qi==5 and kind=="wave" then
        for i=-1,1 do
            local xx=x+i*9
            display.glow_line(xx-3,y-36,xx,y-40,1.2,COLOR.cyan_hot,.8,3,COLOR.cyan)
            display.glow_line(xx,y-40,xx+3,y-36,1.2,COLOR.cyan_hot,.8,3,COLOR.cyan)
        end
    end
    if gesture_mode=="cast" and gesture_skill==index and not unavailable then
        local progress=clamp(-lifted_y/90,0,1)
        local scale=SKILL_SCALE*(1+progress*.45)
        -- The original symbol never leaves its orbit. Only its translucent
        -- echo grows around the exact same center as the user pulls upward.
        display.draw_sprite_atlas("@qi-duel/skill-colors.h2rs",base+lo,base+hi,blend,
            x-80*scale,y-80*scale,scale,progress*.38)
    end
end

local function draw_skills()
    for relative = -1, 1 do
        local angle = relative * CAROUSEL_STEP + carousel_offset / CAROUSEL_RADIUS
        draw_skill_symbol(skill_index(selected+relative),angle)
    end
    if gesture_mode=="cast" then
        local color=-lifted_y>=CAST_LIFT and COLOR.white or COLOR.cyan_mid
        local x,y=CAROUSEL_CX,400
        display.add_line(x-7,y-53,x,y-60,1.2,color,.8)
        display.add_line(x,y-60,x+7,y-53,1.2,color,.8)
    end
    if throw_fx then
        local p=(scene_time()-throw_fx.started)/620
        if p>=0 and p<1 then
            local base=((throw_fx.index-1)*3)*#focuses+#focuses
            local scale=throw_fx.scale+p*.95
            local x,y=throw_fx.x,throw_fx.y
            display.draw_sprite_atlas("@qi-duel/skill-colors.h2rs",base,base,0,
                x-80*scale,y-80*scale,scale,throw_fx.opacity*(1-p)^3)
        elseif p>=1 then
            throw_fx=nil
        end
    end
end

-- Approved three-color sprites replace all in-battle text overlays.
local function draw_countdown(now)
    if not PLAY_GAME or not battle or battle.phase~="select" then return end
    local remaining=battle.round.deadline-now
    if remaining<=0 then return end
    local digit=clamp(math.ceil(remaining/Rules.countdown_step_ms(battle.number)),1,3)
    local size=H106 and 28 or 36
    local x,y=math.floor((SCREEN_W-size)/2),H106 and 39 or 112
    local row=3-digit
    local scale=size/64
    viewport("screen")
    display.draw_affine_asset("@qi-duel/countdown.h2r8",scale,0,0,scale,
        x,y-row*size,{0,row*64,64,64})
end

-- Sprite Maker-authored impact words. The image carries the bevel, glow and
-- fracture detail; runtime motion supplies the short critical-hit shake/pop.
local function impact_label_state(now)
    if impact_probe then return impact_probe,0 end
    if not PLAY_GAME or not battle or battle.phase~="play" or not cast then return nil end
    -- This overlay is strictly player-centric and mutually exclusive: show
    -- the player's combo, or the player's own broken guard. An opponent combo
    -- is communicated through the hit animation unless it breaks our guard.
    local combo=cast.power and cast.power[1]==3
    local broken=battle.result and battle.result.broken and battle.result.broken[1]
    if broken and now>=cast.started+780 then return "armor-break",cast.started+780 end
    if combo and now>=cast.started+410 then return "combo",cast.started+410 end
    return nil
end

local function draw_impact_label(now)
    local kind,started=impact_label_state(now)
    if not kind then return end
    local duration=kind=="combo" and 560 or 720
    local age=now-started
    if age<0 or age>=duration then return end
    viewport("screen")
    local p=age/duration
    local fade=clamp(age/55,0,1)*clamp((duration-age)/150,0,1)
    local base_scale=H106 and (kind=="combo" and .79 or .76) or
        (kind=="combo" and 1.02 or .98)
    local enter=clamp(age/105,0,1)
    local scale=base_scale*(.58+.48*math.sin(enter*math.pi*.5))
    if age>105 then
        scale=scale*(1+.055*math.exp(-(age-105)/190)*math.sin((age-105)*.052))
    end
    local strength=kind=="combo" and 5.2 or 7.4
    local shake=strength*(.18+.82*(1-p)^2)*fade
    local dx=math.sin(age*.39)*shake
    local dy=math.cos(age*.31)*shake*.48
    local cx,cy=SCREEN_W*.5,(H106 and 91 or 184)
    local row=kind=="combo" and 0 or 1
    local crop={0,row*96,192,96}
    local color=kind=="combo" and COLOR.cyan_hot or COLOR.orange

    -- Repeated radial shards read as a compact critical impact at 1x scale.
    local burst=clamp(1-age/330,0,1)
    local beat=1-(age%165)/165
    for i=0,13 do
        local angle=i*TAU/14+(kind=="combo" and 0 or .11)
        local inner=(42+age*.055+(i%3)*4)*(H106 and .72 or 1)
        local length=(9+20*burst*beat)*(H106 and .72 or 1)
        local x0,y0=cx+math.cos(angle)*inner,cy+math.sin(angle)*inner*.52
        display.add_line(x0,y0,x0+math.cos(angle)*length,
            y0+math.sin(angle)*length*.52,kind=="combo" and 1.2 or 1.7,
            i%3==0 and COLOR.white or color,fade*burst*(.34+.58*beat))
    end
    display.add_disc(cx,cy,(18+burst*18)*(H106 and .72 or 1),color,
        fade*burst*.055)

    local function sprite(draw_scale,angle,opacity,offset_x,offset_y)
        local a,b=math.cos(angle)*draw_scale,math.sin(angle)*draw_scale
        local c,d=-b,a
        local source_cx,source_cy=96,row*96+48
        local target_x,target_y=cx+dx+offset_x,cy+dy+offset_y
        display.draw_affine_asset("@qi-duel/impact-labels.h2r8",a,b,c,d,
            target_x-a*source_cx-c*source_cy,
            target_y-b*source_cx-d*source_cy,crop,opacity)
    end
    local angle=math.sin(age*.47)*(kind=="combo" and .008 or .013)*(1-p)
    sprite(scale*1.075,-angle,fade*.20,-dx*.7,dy*.5)
    sprite(scale,angle,fade,0,0)
end

-- Account for the panel transfer in every render branch, including cinematics.
local function present_render(draw_started_ms)
    local present_started_ms=system.millis()
    display.present()
    return present_started_ms-draw_started_ms,system.millis()-present_started_ms
end

local render_phase
local function start_render_phase(phase)
    if phase~=render_phase then
        Intro.history_last=nil
        if display.reset_vector_cache then display.reset_vector_cache() end
        render_phase=phase
    end
end
local function render(now_ms)
    local draw_started_ms = system.millis()
    local intro_active=PLAY_GAME and intro and intro.phase~="done"
    local intro_fading=intro_active and intro.phase=="fade"
    if intro_active and not intro_fading then
        start_render_phase("intro")
        if display.fade_composite and Intro.camera_tilt(now_ms)==0 and not intro.reentry_started then
            Intro.draw_history(now_ms)
            return present_render(draw_started_ms)
        end
        Intro.history_last=nil
        display.begin_composite(true)
        viewport("scene");draw_space_particles(now_ms)
        if intro.reentry_started then
            local cover=1-ease((now_ms-intro.reentry_started)/INTRO_REENTRY_MS)
            if cover>0 then
                viewport("screen")
                display.draw_polygon({{0,0},{SCREEN_W,0},{SCREEN_W,SCREEN_H},{0,SCREEN_H}},
                    COLOR.black,cover,COLOR.black,0,0,COLOR.black,0,0)
            end
        end
        display.end_composite()
        return present_render(draw_started_ms)
    end
    local clash_state=inspector_layer=="full" and Intro.clash_state(now_ms) or nil
    if clash_state then
        start_render_phase("clash")
        -- A hard cinematic cut keeps the close-up genuinely OLED black: no
        -- arena, actors, hands, HUD or residual glow is drawn underneath.
        display.begin_composite(true)
        viewport("screen");Intro.draw_clash(now_ms,clash_state)
        display.end_composite()
        return present_render(draw_started_ms)
    end
    local settlement_state=inspector_layer=="full" and Intro.settlement_state(now_ms) or nil
    if settlement_state then
        start_render_phase("settlement")
        display.begin_composite(true)
        viewport("screen");Intro.draw_settlement(now_ms,settlement_state)
        display.end_composite()
        return present_render(draw_started_ms)
    end
    local full=inspector_layer=="full"
    local scene11=full or inspector_layer=="scene11"
    local scene8=scene11 or inspector_layer=="scene8"
    local scene7=scene8 or inspector_layer=="scene7"
    local arena=scene7 or inspector_layer=="arena" or inspector_layer=="arena-dust"
    start_render_phase("battle")
    if display.procedural_lights then display.begin_composite(true)
    else display.clear(COLOR.black) end
    if arena or inspector_layer=="walls" then draw_wall_lights(now_ms) end
    if arena or inspector_layer=="wheel" then draw_arena_lights(now_ms) end
    if inspector_layer~="walls" and inspector_layer~="wheel" and inspector_layer~="arena" then
        if not display.procedural_lights then display.begin_composite() end
        viewport("scene")
        if scene7 or inspector_layer=="dust" or inspector_layer=="arena-dust" then draw_dust(now_ms) end
        if (scene7 or inspector_layer=="particles") and not intro_fading then
            draw_space_particles(now_ms)
        end
        viewport("opponent")
        if scene7 or inspector_layer=="opponent" then draw_opponent(now_ms) end
        viewport("hands")
        if scene7 or inspector_layer=="hand-left" then draw_player_hands(now_ms,"left") end
        if scene7 or inspector_layer=="hand-right" then draw_player_hands(now_ms,"right") end
        if scene7 then draw_action_effects(now_ms) end
        if scene8 or inspector_layer=="hud" then draw_hud(now_ms) end
        viewport("carousel")
        if scene11 or inspector_layer=="carousel-frame" or inspector_layer=="charge-base" or inspector_layer=="carousel" then draw_carousel_frame() end
        if scene11 or inspector_layer=="charge-cells" or inspector_layer=="charge-base" or inspector_layer=="carousel" then draw_charge_cells(now_ms) end
        if full or inspector_layer=="carousel" then draw_skills() end
        for i,skill in ipairs(SKILLS) do
            if inspector_layer==skill.asset then draw_skill_symbol(i,0) end
        end
        draw_countdown(now_ms)
        draw_impact_label(now_ms)
        if intro_fading then
            local fade=Intro.scene_fade(now_ms)
            viewport("screen")
            display.draw_polygon({{0,0},{SCREEN_W,0},{SCREEN_W,SCREEN_H},{0,SCREEN_H}},
                COLOR.black,1-fade,COLOR.black,0,0,COLOR.black,0,0)
            viewport("scene");draw_space_particles(now_ms)
        end
        if full then
            local cover=Intro.clash_return_alpha(now_ms)
            if settlement and not settlement.clash then
                cover=math.max(cover,clamp((now_ms-(settlement.started-300))/300,0,1))
            end
            if cover>0 then
                viewport("screen")
                display.draw_polygon({{0,0},{SCREEN_W,0},{SCREEN_W,SCREEN_H},{0,SCREEN_H}},
                    COLOR.black,cover,COLOR.black,0,0,COLOR.black,0,0)
            end
        end
        display.end_composite()
    elseif display.procedural_lights then
        display.end_composite()
    end
    return present_render(draw_started_ms)
end

local screen_created = true
local function cleanup()
    if sfx and sfx.bgm then sfx.bgm.close() end
    if sfx then sfx.close() end
    if screen_created then
        pcall(display.end_frame)
        pcall(display.deinit)
        screen_created = false
    end
end

if not H106 and (SCREEN_W ~= 368 or SCREEN_H ~= 448) then
    print(string.format("[qi-duel] ERROR: expected AMOLED 368x448 or H106 240x240, got %dx%d", SCREEN_W, SCREEN_H))
    cleanup()
    return
end

local sync_ok, touch_info = true, nil
if touch then sync_ok, touch_info = pcall(touch.sync) end
if not sync_ok then
    print("[qi-duel] ERROR: touch unavailable: " .. tostring(touch_info))
    cleanup()
    return
end

display.begin_frame({ clear = true, color = COLOR.black })
if H106 and display.prepare_vector_coverage then
    local bytes = display.prepare_vector_coverage(512 * 1024)
    print("H2_QI_DUEL_PREPARE coverage_bytes=" .. bytes)
end
print(string.format("[qi-duel] ready screen=%dx%d layout=%s target_fps=30 skills=4 selected=%d layer=%s game=%s", SCREEN_W, SCREEN_H,H106 and "h106" or "amoled", selected, inspector_layer, tostring(PLAY_GAME)))

-- Prepare moving vector bodies once at more than twice their on-screen size.
-- Progress is outside the game clock; no battle deadline advances while loading.
if display.prepare_vector then
    local started,bytes=system.millis(),0
    local body_scale,hand_scale=H106 and .52 or 1,H106 and .62 or 1
    local entries={{"opponent",math.ceil(312*body_scale),math.ceil(328*body_scale)},
        {"hand-left",math.ceil(308*hand_scale),math.ceil(376*hand_scale)},
        {"hand-right",math.ceil(308*hand_scale),math.ceil(376*hand_scale)}}
    local function progress(n)
        if not PLAY_GAME or fixed_time_ms then return end
        display.clear(COLOR.black)
        display.draw_text(math.floor((SCREEN_W-126)/2),math.floor(SCREEN_H/2)-28,
            "LOADING",{font_size=21,color=COLOR.cyan_hot})
        display.fill_rect(32,math.floor(SCREEN_H/2)+12,SCREEN_W-64,4,COLOR.near_black)
        if n>0 then display.fill_rect(32,math.floor(SCREEN_H/2)+12,
            math.floor((SCREEN_W-64)*n/#entries),4,COLOR.cyan) end
        display.present()
    end
    progress(0)
    for i,entry in ipairs(entries) do
        local allocated,reason=display.prepare_vector("@qi-duel/vector/"..entry[1]..".h2vg",entry[2],entry[3])
        if not allocated then error("vector preparation failed: "..tostring(reason)) end
        bytes=bytes+allocated
        progress(i)
    end
    print(string.format("H2_QI_DUEL_PREPARE bodies=%d bytes=%d elapsed_ms=%d",#entries,bytes,system.millis()-started))
end

local perf_window_started_ms = system.millis()
local perf_frames = 0
local perf_update_total_ms = 0
local perf_audio_total_ms = 0
local perf_draw_total_ms = 0
local perf_present_total_ms = 0
local perf_frame_total_ms = 0
local perf_frame_max_ms = 0
local self_test_samples = 0
local self_test_fps_sum_tenths = 0
local self_test_reported = false
local touch_error_reported = false
scene_started_ms = system.millis()

while true do
    local frame_started_ms = system.millis()
    local polled, info = true, {
        pressed = false, just_pressed = false, just_released = false,
        x = CX, y = 400,
    }
    if touch then polled, info = pcall(touch.poll) end
    if not polled then
        if not touch_error_reported then
            print("[qi-duel] WARN: touch.poll failed: " .. tostring(info))
            touch_error_reported = true
        end
        info = {
            pressed = false, just_pressed = false, just_released = false,
            x = CX, y = 400,
        }
    else
        touch_error_reported = false
    end

    finish_settlement_exit(scene_time())
    update_battle(scene_time()) -- Deadline wins over an input arriving too late.
    handle_touch(info)
    update_battle(scene_time()) -- Lock input now; settlement still waits for the deadline.
    if sfx and sfx.cue then
        local now=scene_time()
        if Intro.audio_selected and selected~=Intro.audio_selected then sfx.cue("select",now) end
        Intro.audio_selected=selected
        if battle and battle.phase=="select" then
            local digit=math.ceil((battle.round.deadline-now)/Rules.countdown_step_ms(battle.number))
            local key=tostring(battle.number)..":"..digit
            if digit>=1 and digit<=3 and Intro.audio_countdown~=key then sfx.cue("countdown",now) end
            Intro.audio_countdown=key
        end
        if cast and cast.round_actions and Intro.audio_cast~=cast and now-cast.started>=ACTIONS.wave.windup then
            Intro.audio_cast=cast
            if cast.round_actions[1]=="wave" and cast.round_actions[2]=="wave" then sfx.cue("clash",now)
            elseif cast.power and (cast.power[1]==3 or cast.power[2]==3) then sfx.cue("combo",now) end
        end
    end
    local audio_started_ms=system.millis()
    if sfx then sfx.update(scene_time()) end
    if sfx and sfx.bgm then
        local now=scene_time()
        sfx.bgm.update(sfx.bgm_scene(intro,settlement,now),now)
    end
    local update_finished_ms = system.millis()
    local draw_ms, present_ms = render(fixed_time_ms or (frame_started_ms - scene_started_ms))
    -- Advance collection between frames so transient path tables do not crowd
    -- native raster workspaces out of PSRAM before Lua reaches its own limit.
    collectgarbage("step",128)
    frame_count = frame_count + 1
    if fixed_time_ms and tonumber(options.capture_step_ms) then
        fixed_time_ms=fixed_time_ms+math.max(1,math.min(1000,tonumber(options.capture_step_ms)))
    end
    local active_finished_ms = system.millis()
    local remaining_ms = FRAME_MS - (active_finished_ms - frame_started_ms)
    delay.delay_ms(math.max(1, remaining_ms))
    local frame_finished_ms = system.millis()
    local update_ms = update_finished_ms - frame_started_ms
    local frame_ms = frame_finished_ms - frame_started_ms

    perf_frames = perf_frames + 1
    perf_update_total_ms = perf_update_total_ms + update_ms
    perf_audio_total_ms = perf_audio_total_ms + update_finished_ms-audio_started_ms
    perf_draw_total_ms = perf_draw_total_ms + draw_ms
    perf_present_total_ms = perf_present_total_ms + present_ms
    perf_frame_total_ms = perf_frame_total_ms + frame_ms
    perf_frame_max_ms = math.max(perf_frame_max_ms, frame_ms)

    local perf_window_ms = frame_finished_ms - perf_window_started_ms
    if perf_window_ms >= PERF_INTERVAL_MS then
        local fps_tenths = math.floor(perf_frames * 10000 / perf_window_ms)
        print(string.format(
            "H2_QI_DUEL_PERF frames=%d fps=%d.%d update_ms=%d audio_ms=%d draw_ms=%d present_ms=%d frame_ms=%d/%d skill=%s qi=%d hp=%d/%d",
            perf_frames, fps_tenths // 10, fps_tenths % 10,
            perf_update_total_ms // perf_frames,
            perf_audio_total_ms // perf_frames,
            perf_draw_total_ms // perf_frames,
            perf_present_total_ms // perf_frames,
            perf_frame_total_ms // perf_frames, perf_frame_max_ms,
            SKILLS[selected].name, qi, player_hp, enemy_hp))

        if not self_test_reported then
            self_test_samples = self_test_samples + 1
            self_test_fps_sum_tenths = self_test_fps_sum_tenths + fps_tenths
            if self_test_samples >= SELF_TEST_WINDOWS then
                local average = self_test_fps_sum_tenths // self_test_samples
                local passed = average >= MIN_PASS_FPS_TENTHS and average <= MAX_PASS_FPS_TENTHS
                print(string.format(
                    "H2_QI_DUEL_SELF_TEST result=%s target_fps=30 measured=%d.%d tolerance=27.0-32.0 samples=%d",
                    passed and "PASS" or "FAIL", average // 10, average % 10,
                    self_test_samples))
                self_test_reported = true
            end
        end

        perf_window_started_ms = frame_finished_ms
        perf_frames = 0
        perf_update_total_ms = 0
        perf_audio_total_ms = 0
        perf_draw_total_ms = 0
        perf_present_total_ms = 0
        perf_frame_total_ms = 0
        perf_frame_max_ms = 0
    end
end

cleanup()
