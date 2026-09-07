local display = require("display")
local touch = require("lcd_touch")
local delay = require("delay")
local system = require("system")

local FRAME_MS = 33
local PERF_INTERVAL_MS = 1000
local SELF_TEST_WINDOWS = 5
local MIN_PASS_FPS_TENTHS = 270
local MAX_PASS_FPS_TENTHS = 320
local W, H = display.width, display.height
local CX = W // 2
local TAU = math.pi * 2

-- Fixed 368x448 composition values from the approved device layout.
local HUD_W, HUD_H = 190, 55
local PLAYER_HUD_X, PLAYER_HUD_Y = 10, 57
local ENEMY_HUD_X, ENEMY_HUD_Y = 168, 15
local CAROUSEL_CX, CAROUSEL_CY = 184, 678
local CAROUSEL_RADIUS, CAROUSEL_STEP = 278, 0.39
local CHARGE_CX, CHARGE_CY = 184, 694
local CHARGE_RADIUS, CHARGE_SPAN = 359, 0.74
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

local selected = 1
local qi = 3
local player_hp, enemy_hp = 4, 4
local frame_count = 0
local carousel_offset = 0
local touch_tracking = false
local touch_target = nil
local touch_start_x, touch_start_y = 0, 0
local meter_fx = { charge = nil, player = nil, enemy = nil }

math.randomseed(system.millis() + W * 19 + H * 37)

local wall_lights = {}
for i = 1, 34 do
    wall_lights[i] = {
        side = i % 2 == 0 and -1 or 1,
        depth = math.random(), lane = math.random(),
        length = math.random(4, 13), phase = math.random() * TAU,
        rate = 0.0007 + math.random() * 0.0013,
        color = i % 9 == 0 and COLOR.orange or
            (i % 4 == 0 and COLOR.violet or COLOR.cyan),
    }
end

local arena_lights = {}
for i = 1, 24 do
    arena_lights[i] = {
        radius = 1.7 + math.random() * 2.2,
        angle = math.random() * TAU,
        span = 0.025 + math.random() * 0.055,
        phase = math.random() * TAU,
        rate = 0.0006 + math.random() * 0.0011,
        color = i % 4 == 0 and COLOR.violet or COLOR.cyan,
    }
end

local floor_particles = {}
for i = 1, 13 do
    floor_particles[i] = {
        progress = math.random(), speed = 0.010 + math.random() * 0.012,
        target_x = math.random(-85, W + 85),
        color = i % 5 == 0 and COLOR.violet or COLOR.cyan,
    }
end

local wall_particles = {}
for i = 1, 13 do
    local side = i % 2 == 0 and -1 or 1
    wall_particles[i] = {
        progress = math.random(), speed = 0.007 + math.random() * 0.011,
        side = side,
        edge_x = CX + side * math.random(92, 174),
        top_x = CX + side * math.random(118, 205),
        color = i % 4 == 0 and COLOR.violet or COLOR.cyan,
    }
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
        index = index, direction = direction, started = system.millis(),
    }
end

local function change_meter(target, delta)
    local old_value, new_value
    if target == "charge" then
        old_value = qi
        new_value = clamp(qi + delta, 0, 10)
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
    if old_value ~= new_value then start_meter_fx(target, old_value, new_value) end
end

local function point_inside(x, y, left, top, width, height)
    return x >= left and x <= left + width and y >= top and y <= top + height
end

local function resolve_touch_target(x, y)
    if point_inside(x, y, PLAYER_HUD_X, PLAYER_HUD_Y, HUD_W, HUD_H) then
        return "player", PLAYER_HUD_X + HUD_W * 0.5
    end
    if point_inside(x, y, ENEMY_HUD_X, ENEMY_HUD_Y, HUD_W, HUD_H) then
        return "enemy", ENEMY_HUD_X + HUD_W * 0.5
    end
    if point_inside(x, y, 42, 312, 284, 58) then return "charge", CX end
    return nil, nil
end

local function handle_touch(info)
    if info.just_pressed then
        touch_start_x, touch_start_y = info.x, info.y
        touch_target = resolve_touch_target(info.x, info.y)
        touch_tracking = touch_target == nil and info.y >= 315
        carousel_offset = 0
    end

    if touch_tracking and info.pressed then
        carousel_offset = clamp(info.x - touch_start_x, -90, 90)
    end

    if info.just_released then
        local travel = info.x - touch_start_x
        local vertical = info.y - touch_start_y
        if touch_target ~= nil and math.abs(travel) < 16 and math.abs(vertical) < 18 then
            local _, midpoint = resolve_touch_target(touch_start_x, touch_start_y)
            change_meter(touch_target, touch_start_x < midpoint and -1 or 1)
        elseif touch_tracking and math.abs(travel) >= SWIPE_THRESHOLD then
            if travel < 0 then
                selected = skill_index(selected + 1)
                carousel_offset = travel + CAROUSEL_RADIUS * CAROUSEL_STEP
            else
                selected = skill_index(selected - 1)
                carousel_offset = travel - CAROUSEL_RADIUS * CAROUSEL_STEP
            end
        end
        touch_target, touch_tracking = nil, false
    end

    if not touch_tracking then
        carousel_offset = carousel_offset * 0.68
        if math.abs(carousel_offset) < 0.8 then carousel_offset = 0 end
    end
end

local function update_particles()
    for i = 1, #floor_particles do
        local particle = floor_particles[i]
        particle.progress = particle.progress + particle.speed
        if particle.progress >= 1 then
            particle.progress = particle.progress - 1
            particle.target_x = math.random(-85, W + 85)
        end
    end
    for i = 1, #wall_particles do
        local particle = wall_particles[i]
        particle.progress = particle.progress + particle.speed
        if particle.progress >= 1 then particle.progress = particle.progress - 1 end
    end
end

local function draw_wall_lights(now_ms)
    for i = 1, #wall_lights do
        local lamp = wall_lights[i]
        local visibility = math.sin(now_ms * lamp.rate + lamp.phase)
        if visibility > -0.12 then
            local center_gap = 18 + (1 - lamp.depth) * 36
            local wall_width = W * 0.5 - center_gap
            local x = lamp.side < 0 and
                center_gap + lamp.lane * wall_width or
                W - center_gap - lamp.lane * wall_width
            local y = 84 + lamp.depth * 139
            local length = lamp.length * (0.45 + lamp.depth * 0.72)
            local dim = visibility < 0.3
            local color = dim and
                (lamp.color == COLOR.orange and COLOR.orange_mid or
                (lamp.color == COLOR.violet and COLOR.violet_mid or COLOR.cyan_dark)) or lamp.color
            thick_line(x, y - length * 0.5, x, y + length * 0.5,
                visibility > 0.72 and 2 or 1, color)
        end
    end
end

local arena_camera = { cx = 184, horizon = 187, fx = 420, fy = 624, depth = 8 }
local function arena_project(world_x, world_z)
    return arena_camera.cx + arena_camera.fx * world_x / world_z,
        arena_camera.horizon + arena_camera.fy / world_z
end

local function arena_polar(radius, angle)
    return arena_project(radius * math.cos(angle),
        arena_camera.depth + radius * math.sin(angle))
end

local function draw_arena_lights(now_ms)
    for i = 1, #arena_lights do
        local lamp = arena_lights[i]
        local visibility = math.sin(now_ms * lamp.rate + lamp.phase)
        if visibility > -0.18 then
            local x0, y0 = arena_polar(lamp.radius, lamp.angle - lamp.span)
            local x1, y1 = arena_polar(lamp.radius, lamp.angle + lamp.span)
            if y0 > 182 and y0 < 358 and y1 > 182 and y1 < 358 then
                local perspective = clamp((y0 + y1) * 0.5 - 188, 0, 150) / 150
                local color = visibility > 0.45 and lamp.color or
                    (lamp.color == COLOR.violet and COLOR.violet_mid or COLOR.cyan_dark)
                thick_line(x0, y0, x1, y1, 1 + math.floor(perspective * 2), color)
            end
        end
    end
end

local function floor_particle_point(particle, progress)
    local t = clamp(progress, 0, 1)
    local eased = t * t
    return CX + (particle.target_x - CX) * eased,
        238 + (H + 42 - 238) * eased
end

local function wall_particle_point(particle, progress)
    local t = clamp(progress, 0, 1)
    local join_t = 0.46
    if t <= join_t then
        local q = t / join_t
        local eased = q * (2 - q)
        return CX + (particle.edge_x - CX) * eased, 238 - 27 * eased
    end
    local q = (t - join_t) / (1 - join_t)
    local one = 1 - q
    local control_x = particle.edge_x + particle.side * 34
    local control_y = 172
    return one * one * particle.edge_x + 2 * one * q * control_x + q * q * particle.top_x,
        one * one * 211 + 2 * one * q * control_y + q * q * (-22)
end

local function draw_space_particles()
    for i = 1, #floor_particles do
        local particle = floor_particles[i]
        local p = particle.progress
        local trail = 0.035 + p * 0.09
        local x0, y0 = floor_particle_point(particle, math.max(0, p - trail))
        local x1, y1 = floor_particle_point(particle, p)
        local thickness = 1 + math.floor(p * p * 4)
        thick_line(x0, y0, x1, y1, thickness,
            p > 0.28 and particle.color or COLOR.cyan_dark)
        if thickness >= 3 then safe_line(x0, y0, x1, y1, COLOR.cyan_hot) end
    end
    for i = 1, #wall_particles do
        local particle = wall_particles[i]
        local p = particle.progress
        local trail = 0.045 + p * 0.055
        local x0, y0 = wall_particle_point(particle, math.max(0, p - trail))
        local x1, y1 = wall_particle_point(particle, p)
        local thickness = 1 + math.floor(p * 2.4)
        thick_line(x0, y0, x1, y1, thickness,
            p > 0.93 and COLOR.violet_dark or particle.color)
    end
end

local function draw_background(now_ms)
    display.clear(COLOR.black)
    draw_wall_lights(now_ms)
    draw_arena_lights(now_ms)
    draw_space_particles()
end

local function draw_opponent(now_ms)
    local bob = math.sin(now_ms * 0.0041) * 1.6
    local sway = math.sin(now_ms * 0.0022) * 1.15
    display.draw_asset("@qi-duel/opponent.a4", iround(109.5 + sway),
        iround(109 + bob))
end

local function draw_player_hands(now_ms)
    local breathe = math.sin(now_ms * 0.0033)
    local spread = math.sin(now_ms * 0.0018)
    display.draw_asset("@qi-duel/hand-left.a4",
        iround(-17 - spread * 2), iround(176 + breathe * 2.5))
    display.draw_asset("@qi-duel/hand-right.a4",
        iround(242 + spread * 2), iround(176 - breathe * 2.5))
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
    local color = enemy and COLOR.orange or COLOR.cyan
    local mid = enemy and COLOR.orange_mid or COLOR.cyan_mid

    for i = 1, 5 do
        local filled = enemy and i > 5 - health or (not enemy and i <= health)
        local points = health_cell_points(panel_x, panel_y, side, i)
        polygon_fill(points, filled and color or COLOR.near_black)
        polygon_outline(points, filled and color or mid, 1)
    end

    local fx = meter_fx[side]
    if fx ~= nil then
        local age = now_ms - fx.started
        if age >= 0 and age < 480 then
            local pulse = math.sin(math.pi * age / 480)
            local points = health_cell_points(panel_x, panel_y, side, fx.index)
            polygon_outline(points, COLOR.white, 1 + math.floor(pulse * 2))
            local center_x = (points[1][1] + points[2][1] + points[3][1] + points[4][1]) * 0.25
            local center_y = (points[1][2] + points[2][2] + points[3][2] + points[4][2]) * 0.25
            local spark = 5 + age / 70
            safe_line(center_x - spark, center_y, center_x - 3, center_y, color)
            safe_line(center_x + 3, center_y, center_x + spark, center_y, color)
        end
    end
end

local function draw_hud(now_ms)
    display.draw_asset("@qi-duel/player-hud.a4", PLAYER_HUD_X, PLAYER_HUD_Y)
    display.draw_asset("@qi-duel/enemy-hud.a4", ENEMY_HUD_X, ENEMY_HUD_Y)
    draw_health_panel("player", PLAYER_HUD_X, PLAYER_HUD_Y, player_hp, now_ms)
    draw_health_panel("enemy", ENEMY_HUD_X, ENEMY_HUD_Y, enemy_hp, now_ms)
end

local function orbit_point(cx, cy, radius, angle)
    return cx + math.sin(angle) * radius, cy - math.cos(angle) * radius
end

local function draw_carousel_frame()
    display.draw_asset("@qi-duel/carousel.a4", 0, 267)
end

local function draw_charge_cell(index, lit, now_ms)
    local angle = -CHARGE_SPAN * 0.5 + (index - 1) * CHARGE_SPAN / 9
    local x, y = orbit_point(CHARGE_CX, CHARGE_CY, CHARGE_RADIUS, angle)
    local scale = 1
    local fx = meter_fx.charge
    if fx ~= nil and fx.index == index then
        local age = now_ms - fx.started
        if age >= 0 and age < 520 then scale = 1 + math.sin(math.pi * age / 520) * 0.18 end
    end
    local points = octagon_points(x, y, 25, 15, angle, scale)
    polygon_fill(points, lit and COLOR.cyan or COLOR.near_black)
    if lit then polygon_outline(points, COLOR.cyan_mid, 3) end
    polygon_outline(points, lit and COLOR.cyan_hot or COLOR.cyan_mid, 1)
    if fx ~= nil and fx.index == index then
        local age = now_ms - fx.started
        if age >= 0 and age < 520 then
            local spark = 10 + age / 40
            safe_line(x - spark, y, x - 8, y, COLOR.cyan_hot)
            safe_line(x + 8, y, x + spark, y, COLOR.cyan_hot)
        end
    end
end

local function draw_charge_cells(now_ms)
    for i = 1, 10 do draw_charge_cell(i, i <= qi, now_ms) end
end

local function draw_skill_symbol(skill, x, y, size, active, relative)
    local suffix = ""
    if not active then suffix = relative < 0 and "-left" or "-right" end
    display.draw_asset("@qi-duel/" .. skill.asset .. suffix .. ".a4",
        iround(x - size * 0.5), iround(y - size * 0.5), {
            width = size, height = size, opacity = active and 255 or 150,
        })
end

local function draw_skills()
    for relative = -1, 1 do
        local angle = relative * CAROUSEL_STEP + carousel_offset / CAROUSEL_RADIUS
        local x, y = orbit_point(CAROUSEL_CX, CAROUSEL_CY, CAROUSEL_RADIUS, angle)
        local active = relative == 0 and math.abs(carousel_offset) < 42
        draw_skill_symbol(SKILLS[skill_index(selected + relative)], x, y,
            active and 88 or 52, active, relative)
    end
end

local function draw_carousel(now_ms)
    draw_carousel_frame()
    draw_charge_cells(now_ms)
    draw_skills()
end

local function render(now_ms)
    local draw_started_ms = system.millis()
    draw_background(now_ms)
    draw_opponent(now_ms)
    draw_player_hands(now_ms)
    draw_hud(now_ms)
    draw_carousel(now_ms)
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

if W ~= 368 or H ~= 448 then
    print(string.format("[qi-duel] ERROR: expected 368x448, got %dx%d", W, H))
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
print(string.format("[qi-duel] ready screen=%dx%d target_fps=30 skills=4", W, H))

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

    handle_touch(info)
    update_particles()
    local update_finished_ms = system.millis()
    local draw_ms, present_ms = render(frame_started_ms)
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
