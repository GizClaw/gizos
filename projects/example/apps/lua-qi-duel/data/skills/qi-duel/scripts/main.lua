local display = require("display")
local touch = require("lcd_touch")
local delay = require("delay")
local system = require("system")
local Rules = require("rules")

local FRAME_MS = 33
local PERF_INTERVAL_MS = 1000
local SELF_TEST_WINDOWS = 5
local MIN_PASS_FPS_TENTHS = 270
local MAX_PASS_FPS_TENTHS = 320
local SCREEN_W, SCREEN_H = display.width, display.height
local H106 = SCREEN_W==240 and SCREEN_H==240
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
local Link=PLAY_GAME and require("duel_link") or nil
local Protocol=PLAY_GAME and require("link_protocol") or nil
local json=PLAY_GAME and require("json") or nil
local page,mode,menu_selection=PLAY_GAME and "menu" or "battle","demon",1
local network,link_error,menu_press=nil,nil,nil
local fixed_time_ms = tonumber(options.time_ms)
local impact_probe = options.impact ~= "" and options.impact or nil
local scene_started_ms = system.millis()
local supported_layers = {full=true,walls=true,wheel=true,arena=true,dust=true,
    particles=true,opponent=true,["hand-left"]=true,["hand-right"]=true,
    ["arena-dust"]=true,scene7=true,hud=true,scene8=true,scene11=true,
    ["carousel-frame"]=true,["charge-cells"]=true,["charge-base"]=true,carousel=true,
    ["skill-charge"]=true,["skill-wave"]=true,["skill-absorb"]=true,["skill-guard"]=true}
assert(supported_layers[inspector_layer], "unsupported inspector layer")
assert(fixed_time_ms == nil or (fixed_time_ms >= 0 and fixed_time_ms < math.huge),
    "invalid inspector time")
assert(impact_probe == nil or impact_probe == "combo" or impact_probe == "armor-break",
    "invalid impact probe")

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

local function iround(value)
    return math.floor(value + 0.5)
end

local function skill_index(index)
    return ((index - 1) % #SKILLS) + 1
end

local function safe_line(x0, y0, x1, y1, color)
    display.draw_line(
        clamp(iround(x0), 0, W - 1), clamp(iround(y0), 0, H - 1),
        clamp(iround(x1), 0, W - 1), clamp(iround(y1), 0, H - 1), color)
end

local function thick_line(x0, y0, x1, y1, thickness, color)
    local dx, dy = x1 - x0, y1 - y0
    local length = math.max(1, math.sqrt(dx * dx + dy * dy))
    local nx, ny = -dy / length, dx / length
    local half = math.floor(thickness / 2)
    for offset = -half, half do
        safe_line(x0 + nx * offset, y0 + ny * offset,
            x1 + nx * offset, y1 + ny * offset, color)
    end
end

local function polygon_fill(points, color)
    for i = 2, #points - 1 do
        display.fill_triangle(
            clamp(iround(points[1][1]), 0, W - 1),
            clamp(iround(points[1][2]), 0, H - 1),
            clamp(iround(points[i][1]), 0, W - 1),
            clamp(iround(points[i][2]), 0, H - 1),
            clamp(iround(points[i + 1][1]), 0, W - 1),
            clamp(iround(points[i + 1][2]), 0, H - 1), color)
    end
end

local function polygon_outline(points, color, thickness)
    for i = 1, #points do
        local next_i = i == #points and 1 or i + 1
        thick_line(points[i][1], points[i][2],
            points[next_i][1], points[next_i][2], thickness or 1, color)
    end
end

local function transformed_polygon(cx, cy, locals, angle, scale)
    local points = {}
    local cos_a, sin_a = math.cos(angle or 0), math.sin(angle or 0)
    scale = scale or 1
    for i = 1, #locals do
        local x, y = locals[i][1] * scale, locals[i][2] * scale
        points[i] = {
            cx + x * cos_a - y * sin_a,
            cy + x * sin_a + y * cos_a,
        }
    end
    return points
end

local function octagon_points(cx, cy, width, height, angle, scale)
    local x, y, cut = width * 0.5, height * 0.5, 4
    return transformed_polygon(cx, cy, {
        {-x + cut, -y}, {x - cut, -y}, {x, -y + cut}, {x, y - cut},
        {x - cut, y}, {-x + cut, y}, {-x, y - cut}, {-x, -y + cut},
    }, angle, scale)
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
local SKILL_SCALE=H106 and .82 or 1
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
    return x - math.floor(x)
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
    battle={players=Rules.new(),number=1}
    qi,player_hp,enemy_hp=0,5,5
    meter_fx={charge=nil,player=nil,enemy=nil}
    cast,throw_fx,invalid_fx,confirm_fx=nil,nil,nil,nil
    begin_round(now)
end
local function activate_mode(index,now)
    menu_selection=index;network=nil;link_error=nil;battle=nil;cast=nil
    if index==1 then mode="demon";page="battle";new_battle(now)
    else
        mode="void"
        local ok,rc=Link.pair()
        page=ok and "pairing" or "link-error"
        if not ok then link_error="BLE UNAVAILABLE ("..tostring(rc)..")" end
    end
end
local function leave_link()
    if mode=="void" then Link.stop() end
    page="menu";network=nil;battle=nil;cast=nil
end
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
        return true
    end
end
local function play_result(result,now)
    battle.result=result;battle.phase="play";battle.started=now;battle.applied=false
    local index=confirm_fx and confirm_fx.index or selected
    if result.actions[1]=="invalid" then
        invalid_fx={kind=SKILLS[index].kind,started=now}
    else start_icon_echo(index,0) end
    confirm_fx=nil
    cast={started=now,duration=Rules.PLAY_MS,actor="both",round_actions=result.actions,power=result.power}
    print(string.format("H2_QI_DUEL_ROUND round=%d player=%s enemy=%s damage=%d/%d",
        battle.number,result.actions[1],result.actions[2],result.damage[1],result.damage[2]))
end
local function apply_result(now)
    if battle.phase=="play" and not battle.applied and now-battle.started>=750 then
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
    if not PLAY_GAME or page=="menu" or page=="link-error" then return end
    if mode=="void" then
        local ok,err=pcall(update_network,now)
        if not ok then
            link_error=tostring(err):match("([^:]+)$") or "LINK ERROR"
            page="link-error";Link.stop();network=nil
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
local function mode_button(x,y)
    local left=(SCREEN_W-math.min(224,SCREEN_W-40))/2
    if x<left or x>SCREEN_W-left then return nil end
    local top=math.floor(SCREEN_H*.43);local height=H106 and 42 or 56
    if y>=top and y<=top+height then return 1 end
    if y>=top+height+12 and y<=top+height*2+12 then return 2 end
end
local function menu_touch(info)
    if info.just_pressed then
        menu_press=page=="menu" and mode_button(info.x,info.y) or
            (info.y>=SCREEN_H*.76 and 3 or nil)
    end
    if info.just_released and menu_press then
        if page=="menu" and mode_button(info.x,info.y)==menu_press then activate_mode(menu_press,scene_time())
        elseif page~="menu" and menu_press==3 and info.y>=SCREEN_H*.76 then leave_link() end
        menu_press=nil
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
    if PLAY_GAME and page~="battle" then return menu_touch(info) end
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

-- H106 product logical component IDs; its launcher maps these to real keys.
-- Desktop mirrors Volume+ / Volume- / Record with Up / Down / Tab.
if H106 then
    local runtime=require("runtime")
    local held={}
    local function key_down(id)
        if held[id] then return end
        held[id]=true
        if PLAY_GAME and page~="battle" then
            if page=="menu" then
                if id==9 or id==10 then menu_selection=3-menu_selection
                elseif id==11 then activate_mode(menu_selection,scene_time()) end
            elseif id==11 then leave_link() end
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
            print(string.format("H2_QI_DUEL_BUTTON key=%s selected=%s",id==9 and "volume_up" or "volume_down",SKILLS[selected].kind))
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
local function arena_project(x,z) return {184+420*x/z,187+624/z} end
local function arena_polar(radius,angle)
    return arena_project(radius*math.cos(angle),8+radius*math.sin(angle))
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
local function draw_particle_path(points,width,hue,alpha)
    for i=2,#points do
        local u=(i-1)/(#points-1)
        local taper=.12+.88*u^1.35
        local segment_alpha=alpha*u^1.7
        local a,b=points[i-1],points[i]
        display.add_line(a[1],a[2],b[1],b[2],width*taper*4.4,hsl(hue,.58),segment_alpha*.18)
        display.add_line(a[1],a[2],b[1],b[2],math.max(.18,width*taper),hsl(hue,.76),segment_alpha)
    end
end
local function draw_space_particles(now_ms)
    local t=now_ms*.001
    for index,p in ipairs(space_particles) do
        local i=index-1
        if i<52 then
            local progress=(p.p0+t*p.speed*.72)%1
            local size_fraction,opacity
            if progress<.58 then
                local grow=smoothstep(progress/.58)
                size_fraction=.24+grow*.76;opacity=.28+grow*.72
            else
                local inverse=1-(progress-.58)/.42
                size_fraction=inverse^1.45;opacity=inverse^1.7
            end
            local lateral=(hash01(i*31+4)*2-1)*.76
            local forward=math.sqrt(1-lateral*lateral)
            local distance=smoothstep(progress)*(5.05+hash01(i*47+9)*.72)
            local world_z=8-forward*distance
            if world_z>1.15 then
                local perspective=8/world_z
                local trail=(.21+p.size*.11)*(.5+perspective^.82*.82)
                local tail_distance=math.max(0,distance-trail)
                local head=arena_project(lateral*distance,world_z)
                local tail=arena_project(lateral*tail_distance,8-forward*tail_distance)
                local points={}
                for step=0,8 do
                    points[#points+1]={tail[1]+(head[1]-tail[1])*step/8,tail[2]+(head[2]-tail[2])*step/8}
                end
                local width=math.min(14,(.44+p.size*.36)*perspective^1.58*size_fraction)
                draw_particle_path(points,width,p.hue,math.min(1,opacity*(.35+perspective*.28)))
            end
        else
            local progress=(p.p0+t*p.speed*.62)%1
            local size_fraction,opacity=1,1
            if progress<.36 then
                local grow=smoothstep(progress/.36)
                size_fraction=.24+grow*.76;opacity=.28+grow*.72
            end
            local route=smoothstep(progress)
            local angle=.025+(i-52+hash01(i*53+7)*.72)/52*(math.pi-.05)
            local hit_radius=3+hash01(i*79+3)*.38
            local base_perspective=8/(8+math.sin(angle)*hit_radius)
            local turn_radius=hit_radius-.34
            local turn_start=arena_polar(turn_radius,angle)
            local before=arena_polar(turn_radius-.12,angle)
            local radial_x,radial_y=turn_start[1]-before[1],turn_start[2]-before[2]
            local hit=arena_polar(hit_radius,angle)
            local turn_end={hit[1],hit[2]-9}
            local raw_y=math.abs(radial_x)>.5 and
                turn_start[2]+(turn_end[1]-turn_start[1])*radial_y/radial_x or
                (turn_start[2]+turn_end[2])*.5
            local control={turn_end[1],clamp(raw_y,math.min(turn_start[2],turn_end[2]),math.max(turn_start[2],turn_end[2]))}
            local wall_rise=turn_end[2]+34+hash01(i*101+5)*22
            local function point_at(u)
                if u<=.56 then
                    local radius=turn_radius*u/.56
                    local point=arena_polar(radius,angle)
                    point[3]=8/(8+math.sin(angle)*radius)
                    return point
                elseif u<=.72 then
                    local q=(u-.56)/(.72-.56);local v=1-q
                    return {v*v*turn_start[1]+2*v*q*control[1]+q*q*turn_end[1],
                        v*v*turn_start[2]+2*v*q*control[2]+q*q*turn_end[2],base_perspective*(1-q*.1)}
                else
                    local wall_u=(u-.72)/(1-.72)
                    return {turn_end[1],turn_end[2]-wall_rise*wall_u,base_perspective*(1-wall_u*.66)}
                end
            end
            local head=point_at(route);local perspective=head[3]
            if head[2]<42 then
                local inverse=1-math.min(1,(42-head[2])/78)
                size_fraction=size_fraction*inverse^1.35;opacity=opacity*inverse^1.65
            end
            local wanted=(8+perspective*24)*(.34+size_fraction*.66)
            local points={head};local sampled,accumulated=route,0
            for _=1,22 do
                if sampled<=0 or accumulated>=wanted then break end
                sampled=math.max(0,sampled-.005)
                local point=point_at(sampled);local first=points[1]
                accumulated=accumulated+math.sqrt((first[1]-point[1])^2+(first[2]-point[2])^2)
                table.insert(points,1,point)
            end
            local width=math.max(.16,(.46+p.size*.36)*perspective^1.58*size_fraction)
            draw_particle_path(points,width,p.hue,math.min(1,opacity*(.38+perspective*.55)))
        end
    end
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
    local source_index=math.floor((index-1)*9/(CHARGE_MAX-1)+.5)
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
    local digit=clamp(math.ceil(remaining/1000),1,3)
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
    local combo=cast.power and (cast.power[1]==3 or cast.power[2]==3)
    local broken=battle.result and battle.result.broken and
        (battle.result.broken[1] or battle.result.broken[2])
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

local function mode_label(row,y,scale)
    local s=scale or (H106 and .75 or 1)
    display.draw_affine_asset("@qi-duel/ui-labels.h2r8",s,0,0,s,
        (SCREEN_W-160*s)/2,y-row*32*s,{0,row*32,160,32})
end
local function draw_mode_page(now)
    viewport("screen")
    local top=math.floor(SCREEN_H*.43);local height=H106 and 42 or 56
    if page=="menu" then
        local left=(SCREEN_W-math.min(224,SCREEN_W-40))/2
        local right=SCREEN_W-left
        mode_label(2,top-45)
        for i=1,2 do
            local y=top+(i-1)*(height+12)
            local color=i==1 and COLOR.cyan or COLOR.violet
            display.draw_polygon({{left+4,y},{right-4,y},{right,y+6},
                {right,y+height-6},{right-4,y+height},{left+4,y+height},
                {left,y+height-6},{left,y+6}},COLOR.near_black,.85,color,
                menu_selection==i and .9 or .45,1.2,color,menu_selection==i and .25 or .1,5)
            mode_label(i-1,y+(height-(H106 and 24 or 32))/2)
        end
    else
        mode_label(page=="pairing" and 3 or 4,top)
        if page=="pairing" then
            for i=0,7 do
                local a=now*.003+i*TAU/8
                display.add_disc(SCREEN_W/2+math.cos(a)*24,top-26+math.sin(a)*12,
                    1.8,COLOR.violet,(i+1)/8)
            end
        end
        mode_label(5,SCREEN_H*.8)
    end
end

local function render(now_ms)
    local draw_started_ms = system.millis()
    if PLAY_GAME and page~="battle" then
        display.clear(COLOR.black);display.begin_composite();draw_mode_page(now_ms);display.end_composite()
        if page=="link-error" then
            local text=link_error or "LINK ERROR"
            text=text:sub(1,math.floor(SCREEN_W/6)-2)
            display.draw_text(6,math.floor(SCREEN_H*.63),text,{font_size=7,color=COLOR.orange})
        end
        display.present()
        return system.millis()-draw_started_ms,0
    end
    local full=inspector_layer=="full"
    local scene11=full or inspector_layer=="scene11"
    local scene8=scene11 or inspector_layer=="scene8"
    local scene7=scene8 or inspector_layer=="scene7"
    local arena=scene7 or inspector_layer=="arena" or inspector_layer=="arena-dust"
    display.clear(COLOR.black)
    if arena or inspector_layer=="walls" then draw_wall_lights(now_ms) end
    if arena or inspector_layer=="wheel" then draw_arena_lights(now_ms) end
    if inspector_layer~="walls" and inspector_layer~="wheel" and inspector_layer~="arena" then
        display.begin_composite()
        viewport("scene")
        if scene7 or inspector_layer=="dust" or inspector_layer=="arena-dust" then draw_dust(now_ms) end
        if scene7 or inspector_layer=="particles" then draw_space_particles(now_ms) end
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
        display.end_composite()
    end
    local present_started_ms = system.millis()
    display.present()
    local present_finished_ms = system.millis()
    return present_started_ms - draw_started_ms,
        present_finished_ms - present_started_ms
end

local screen_created = true
local function cleanup()
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

local touch_ok, touch_info = pcall(touch.sync)
if not touch_ok then
    print("[qi-duel] ERROR: touch unavailable: " .. tostring(touch_info))
    cleanup()
    return
end

display.begin_frame({ clear = true, color = COLOR.black })
print(string.format("[qi-duel] ready screen=%dx%d layout=%s target_fps=30 skills=4 selected=%d layer=%s game=%s", SCREEN_W, SCREEN_H,H106 and "h106" or "amoled", selected, inspector_layer, tostring(PLAY_GAME)))

local perf_window_started_ms = system.millis()
local perf_frames = 0
local perf_update_total_ms = 0
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
    local polled, info = pcall(touch.poll)
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

    update_battle(scene_time()) -- Deadline wins over an input arriving too late.
    handle_touch(info)
    update_battle(scene_time()) -- Lock input now; settlement still waits for the deadline.
    local update_finished_ms = system.millis()
    local draw_ms, present_ms = render(fixed_time_ms or (frame_started_ms - scene_started_ms))
    frame_count = frame_count + 1
    local active_finished_ms = system.millis()
    local remaining_ms = FRAME_MS - (active_finished_ms - frame_started_ms)
    delay.delay_ms(math.max(1, remaining_ms))
    local frame_finished_ms = system.millis()
    local update_ms = update_finished_ms - frame_started_ms
    local frame_ms = frame_finished_ms - frame_started_ms

    perf_frames = perf_frames + 1
    perf_update_total_ms = perf_update_total_ms + update_ms
    perf_draw_total_ms = perf_draw_total_ms + draw_ms
    perf_present_total_ms = perf_present_total_ms + present_ms
    perf_frame_total_ms = perf_frame_total_ms + frame_ms
    perf_frame_max_ms = math.max(perf_frame_max_ms, frame_ms)

    local perf_window_ms = frame_finished_ms - perf_window_started_ms
    if perf_window_ms >= PERF_INTERVAL_MS then
        local fps_tenths = math.floor(perf_frames * 10000 / perf_window_ms)
        print(string.format(
            "H2_QI_DUEL_PERF frames=%d fps=%d.%d update_ms=%d draw_ms=%d present_ms=%d frame_ms=%d/%d skill=%s qi=%d hp=%d/%d",
            perf_frames, fps_tenths // 10, fps_tenths % 10,
            perf_update_total_ms // perf_frames,
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
        perf_draw_total_ms = 0
        perf_present_total_ms = 0
        perf_frame_total_ms = 0
        perf_frame_max_ms = 0
    end
end

cleanup()
