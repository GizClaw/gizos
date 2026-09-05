local display = require("display")
local touch = require("lcd_touch")
local delay = require("delay")
local system = require("system")

local runtime_args = type(args) == "table" and args or {}
local HOVER_CONTROL = runtime_args.hover_control == "true" or
    runtime_args.hover_control == "1" or runtime_args.hover_control == true
local FRAME_MS = 33
local PERF_INTERVAL_MS = 1000
local BODY_COUNT = HOVER_CONTROL and 22 or 18
local BODY_GROUP_COUNT = 4
local BODY_EAT_INTERVAL_FRAMES = HOVER_CONTROL and 2 or 4
local MAX_FRAGMENTS = HOVER_CONTROL and 36 or 28
local MAX_PARTICLES = HOVER_CONTROL and 40 or 30
local STAR_COUNT = HOVER_CONTROL and 54 or 28
local MAX_SPEED = 5.2
local ACCELERATION = 0.34
local DRAG = 0.987
local DEAD_ZONE = 12
local RECYCLE_DISTANCE = 610
local PREY_NOTICE_DISTANCE = 175
local RIVAL_NOTICE_DISTANCE = 215
local DANGER_NOTICE_DISTANCE = 265

local width = display.width
local height = display.height
local center_x = width // 2
local center_y = height // 2 + 18

local function rgb(r, g, b)
    return { r = r, g = g, b = b }
end

local COLOR = {
    black = rgb(0, 0, 0),
    space = rgb(2, 7, 18),
    nebula_outer = rgb(5, 14, 32),
    nebula_mid = rgb(8, 21, 44),
    nebula_core = rgb(12, 29, 54),
    white = rgb(245, 250, 255),
    dim = rgb(92, 116, 148),
    panel = rgb(7, 16, 32),
    panel_border = rgb(43, 75, 110),
    cyan = rgb(63, 231, 234),
    cyan_dim = rgb(19, 113, 132),
    amber = rgb(255, 186, 68),
    red = rgb(255, 75, 91),
    green = rgb(96, 238, 148),
    violet = rgb(187, 105, 255),
    magenta = rgb(255, 73, 203),
}

local STAGES = {
    { name = "PEBBLE", radius = 7, threshold = 34,
      dark = rgb(63, 69, 79), body = rgb(143, 151, 162), light = rgb(224, 229, 235) },
    { name = "ASTEROID", radius = 11, threshold = 68,
      dark = rgb(69, 54, 47), body = rgb(151, 113, 82), light = rgb(230, 181, 119) },
    { name = "PLANET", radius = 16, threshold = 118,
      dark = rgb(18, 64, 91), body = rgb(38, 151, 174), light = rgb(113, 231, 190) },
    { name = "GIANT", radius = 22, threshold = 190,
      dark = rgb(104, 43, 62), body = rgb(222, 113, 83), light = rgb(255, 214, 128) },
    { name = "STAR", radius = 28, threshold = 285,
      dark = rgb(221, 78, 29), body = rgb(255, 163, 45), light = rgb(255, 245, 169) },
    { name = "VOID", radius = 33, threshold = 390,
      dark = rgb(26, 5, 42), body = rgb(2, 1, 7), light = rgb(153, 75, 255) },
}

local UPGRADE = {
    {
        name = "GROWTH",
        line1 = "+25% MASS",
        line2 = "+20 MAGNET",
        color = COLOR.cyan,
    },
    {
        name = "BARRIER",
        line1 = "-30% HIT",
        line2 = "+1 SHIELD",
        color = COLOR.green,
    },
    {
        name = "IMPACT",
        line1 = "+35% RAM",
        line2 = "20% RECYCLE",
        color = COLOR.magenta,
    },
}

local stars = {}
math.randomseed(system.time() + width * 17 + height * 31)
for i = 1, STAR_COUNT do
    stars[i] = {
        x = math.random(0, width - 1),
        y = math.random(0, height - 1),
        size = math.random(1, 2),
        layer = math.random(1, 3),
        bright = math.random(1, 5) == 1,
    }
end

local bodies = {}
local body_groups = {}
local fragments = {}
local particles = {}
local frame_count = 0
local body_eat_count = 0
local state = "title"
local message = ""
local message_ms = 0
local touch_error_reported = false
local hover_seen = false

local player = {
    x = 0,
    y = 0,
    vx = 0,
    vy = 0,
    tier = 1,
    mass = 0,
    integrity = 100,
    yield_multiplier = 1,
    magnet = 54,
    hit_multiplier = 1,
    ram_multiplier = 1,
    recycle = 0,
    shields = 0,
    growth_count = 0,
    barrier_count = 0,
    impact_count = 0,
    invulnerable_ms = 0,
}

local function clamp(value, low, high)
    if value < low then
        return low
    end
    if value > high then
        return high
    end
    return value
end

local function length(x, y)
    return math.sqrt(x * x + y * y)
end

local function player_radius()
    local stage = STAGES[player.tier]
    local progress = clamp(player.mass / stage.threshold, 0, 1)
    return stage.radius + progress * 2.5
end

local function random_body_tier()
    local roll = math.random(-1, 1)
    return clamp(player.tier + roll, 1, #STAGES)
end

local function reset_body_groups()
    body_groups = {}
    for i = 1, BODY_GROUP_COUNT do
        local heading = math.random() * math.pi * 2
        local spawn_angle = math.random() * math.pi * 2
        local spawn_distance = math.random(160, 360)
        body_groups[i] = {
            heading = heading,
            turn_rate = (math.random() - 0.5) * 0.005,
            turn_ms = math.random(1400, 3000),
            spawn_x = player.x + math.cos(spawn_angle) * spawn_distance,
            spawn_y = player.y + math.sin(spawn_angle) * spawn_distance,
        }
    end
end

local function place_body(body, first_spawn)
    if body.group_id == nil or not first_spawn then
        body.group_id = math.random(1, BODY_GROUP_COUNT)
    end
    local group = body_groups[body.group_id]
    body.tier = random_body_tier()
    local base = STAGES[body.tier].radius
    body.radius = base * (0.52 + math.random() * 0.64)
    if first_spawn then
        body.x = group.spawn_x + (math.random() - 0.5) * 110
        body.y = group.spawn_y + (math.random() - 0.5) * 110
    else
        local distance = math.random(350, 500)
        local lane = (math.random() - 0.5) * 260
        body.x = player.x - math.cos(group.heading) * distance -
            math.sin(group.heading) * lane
        body.y = player.y - math.sin(group.heading) * distance +
            math.cos(group.heading) * lane
    end
    body.heading_offset = (math.random() - 0.5) * 0.30
    body.heading = group.heading + body.heading_offset
    body.vx = math.cos(body.heading) * (0.2 + math.random() * 0.45)
    body.vy = math.sin(body.heading) * (0.2 + math.random() * 0.45)
    body.drive = 0.016 + math.random() * 0.024
    body.speed_limit = 0.72 + body.tier * 0.12 + math.random() * 0.52
    body.ai_ms = math.random(350, 1450)
    body.temper = math.random()
    body.spin = math.random(0, 1) == 0 and -1 or 1
    body.phase = math.random() * math.pi * 2
end

local function reset_bodies()
    bodies = {}
    reset_body_groups()
    for i = 1, BODY_COUNT do
        local body = {
            group_id = (i - 1) % BODY_GROUP_COUNT + 1,
        }
        place_body(body, true)
        bodies[i] = body
    end
end

local function reset_game()
    body_eat_count = 0
    player.x = 0
    player.y = 0
    player.vx = 0
    player.vy = 0
    player.tier = 1
    player.mass = 0
    player.integrity = 100
    player.yield_multiplier = 1
    player.magnet = 54
    player.hit_multiplier = 1
    player.ram_multiplier = 1
    player.recycle = 0
    player.shields = 0
    player.growth_count = 0
    player.barrier_count = 0
    player.impact_count = 0
    player.invulnerable_ms = 0
    fragments = {}
    particles = {}
    message = ""
    message_ms = 0
    reset_bodies()
end

local function add_particle(x, y, vx, vy, color, ttl, size)
    if #particles >= MAX_PARTICLES then
        table.remove(particles, 1)
    end
    particles[#particles + 1] = {
        x = x,
        y = y,
        vx = vx,
        vy = vy,
        color = color,
        ttl = ttl,
        size = size,
    }
end

local function burst(x, y, color, count)
    for i = 1, count do
        local angle = math.random() * math.pi * 2
        local speed = 0.7 + math.random() * 2.1
        add_particle(x, y, math.cos(angle) * speed, math.sin(angle) * speed,
            color, math.random(280, 620), math.random(1, 3))
    end
end

local function add_fragments(body, total_mass)
    local count = clamp(math.floor(body.radius / 2), 4, 7)
    for i = 1, count do
        if #fragments >= MAX_FRAGMENTS then
            table.remove(fragments, 1)
        end
        local angle = math.random() * math.pi * 2
        local speed = 0.9 + math.random() * 1.5
        fragments[#fragments + 1] = {
            x = body.x,
            y = body.y,
            vx = math.cos(angle) * speed,
            vy = math.sin(angle) * speed,
            mass = total_mass / count,
            ttl = 5200,
        }
    end
end

local function show_message(text, duration_ms)
    message = text
    message_ms = duration_ms or 900
end

local function classify_body(body)
    local radius = player_radius()
    local safe_ratio = 0.72 + (player.ram_multiplier - 1) * 0.18
    if body.tier < player.tier or body.radius <= radius * safe_ratio then
        return "prey"
    end
    if body.radius <= radius * 1.08 then
        return "rival"
    end
    return "danger"
end

local function shatter_body(body, value, label, color)
    add_fragments(body, value)
    burst(body.x, body.y, color, 12)
    show_message(label, 620)
    player.vx = player.vx * 0.76
    player.vy = player.vy * 0.76
    place_body(body, false)
end

local function collide_body(body)
    local classification = classify_body(body)
    local stage = STAGES[body.tier]
    if classification == "prey" then
        local value = math.max(5, body.radius * 1.35) * player.yield_multiplier
        shatter_body(body, value, "SHATTER", stage.light)
        return
    end

    local dx = body.x - player.x
    local dy = body.y - player.y
    local distance = math.max(0.01, length(dx, dy))
    local nx = dx / distance
    local ny = dy / distance
    local player_attack = math.max(0, player.vx * nx + player.vy * ny)
    local body_attack = math.max(0, -(body.vx * nx + body.vy * ny))
    local ram_score = player_attack * player.ram_multiplier
    local ram_threshold = 2.75 + body.radius * 0.035

    if classification == "rival" and ram_score >= ram_threshold then
        local value = math.max(7, body.radius * 1.55) * player.yield_multiplier
        shatter_body(body, value, "RAM BREAK", COLOR.amber)
        return
    end
    if classification == "danger" and
        body.radius <= player_radius() * 1.28 and
        ram_score >= ram_threshold + 1.65 then
        local value = math.max(9, body.radius * 1.7) * player.yield_multiplier
        shatter_body(body, value, "POWER HIT", COLOR.magenta)
        return
    end

    local separation = player_radius() + body.radius + 3
    body.x = player.x + nx * separation
    body.y = player.y + ny * separation
    if player.invulnerable_ms > 0 then
        body.vx = body.vx + nx * 1.25
        body.vy = body.vy + ny * 1.25
        return
    end

    if player.shields > 0 then
        player.shields = player.shields - 1
        burst(player.x, player.y, COLOR.green, 16)
        show_message("SHIELD HELD", 800)
        body.vx = nx * 2.2
        body.vy = ny * 2.2
        player.invulnerable_ms = 420
        return
    end

    local danger_scale = classification == "danger" and 1.35 or 1
    local damage = (18 + body.radius * 0.92 + body_attack * 8) * danger_scale
    damage = damage * player.hit_multiplier
    player.integrity = player.integrity - damage
    if player.recycle > 0 then
        player.mass = player.mass + damage * player.recycle
    end
    burst(player.x, player.y, COLOR.red, 18)
    body.vx = nx * (1.55 + body_attack * 0.3)
    body.vy = ny * (1.55 + body_attack * 0.3)
    player.invulnerable_ms = 720
    if player.integrity <= 0 then
        player.integrity = 0
        state = "dead"
        show_message("", 0)
    else
        show_message(classification == "danger" and "CRITICAL HIT" or "COLLISION", 900)
        player.vx = player.vx - nx * (1.15 + body_attack * 0.45)
        player.vy = player.vy - ny * (1.15 + body_attack * 0.45)
    end
end

local function apply_upgrade(index)
    if index == 1 then
        player.yield_multiplier = player.yield_multiplier * 1.25
        player.magnet = player.magnet + 20
        player.growth_count = player.growth_count + 1
    elseif index == 2 then
        player.hit_multiplier = player.hit_multiplier * 0.70
        player.shields = player.shields + 1
        player.barrier_count = player.barrier_count + 1
    else
        player.ram_multiplier = player.ram_multiplier * 1.35
        player.recycle = math.min(0.6, player.recycle + 0.20)
        player.impact_count = player.impact_count + 1
    end

    player.tier = player.tier + 1
    player.mass = 0
    player.integrity = math.min(100, player.integrity + 35)
    fragments = {}
    reset_bodies()
    state = "playing"
    show_message("EVOLVED: " .. STAGES[player.tier].name, 1300)
end

local function enter_evolution()
    if player.tier >= #STAGES then
        state = "complete"
        burst(player.x, player.y, COLOR.violet, 32)
    else
        player.mass = STAGES[player.tier].threshold
        player.vx = player.vx * 0.3
        player.vy = player.vy * 0.3
        state = "evolve"
    end
end

local function update_player(info)
    if info.pressed or (HOVER_CONTROL and hover_seen) then
        local dx = info.x - center_x
        local dy = info.y - center_y
        local distance = length(dx, dy)
        if distance > DEAD_ZONE then
            local strength = clamp((distance - DEAD_ZONE) / 110, 0, 1)
            player.vx = player.vx + dx / distance * ACCELERATION * strength
            player.vy = player.vy + dy / distance * ACCELERATION * strength
        end
    end
    player.vx = player.vx * DRAG
    player.vy = player.vy * DRAG
    local speed = length(player.vx, player.vy)
    if speed > MAX_SPEED then
        player.vx = player.vx / speed * MAX_SPEED
        player.vy = player.vy / speed * MAX_SPEED
    end
    player.x = player.x + player.vx
    player.y = player.y + player.vy
    player.integrity = math.min(100, player.integrity + 0.025)
    player.invulnerable_ms = math.max(0, player.invulnerable_ms - FRAME_MS)
end

local function difficulty_scale()
    local stage_pressure = (player.tier - 1) * 0.09
    local time_pressure = math.min(0.30, frame_count / 5400 * 0.30)
    return 1 + stage_pressure + time_pressure
end

local function update_body_groups()
    for i = 1, #body_groups do
        local group = body_groups[i]
        group.turn_ms = group.turn_ms - FRAME_MS
        if group.turn_ms <= 0 then
            group.turn_rate = (math.random() - 0.5) * 0.005
            group.turn_ms = math.random(1400, 3000)
        end
        group.heading = group.heading + group.turn_rate
    end
end

local function update_body_motion(body, classification, to_player_x, to_player_y,
    distance, capture_distance)
    body.ai_ms = body.ai_ms - FRAME_MS
    if body.ai_ms <= 0 then
        body.heading_offset = (math.random() - 0.5) * 0.36
        body.ai_ms = math.random(700, 1800)
    end

    local group = body_groups[body.group_id]
    body.heading = group.heading + body.heading_offset

    local desired_x = math.cos(body.heading)
    local desired_y = math.sin(body.heading)
    local drive = body.drive
    if distance > 0.5 then
        local target_x = to_player_x / distance
        local target_y = to_player_y / distance
        if classification == "prey" and distance < capture_distance then
            desired_x = target_x
            desired_y = target_y
            drive = drive * 3.2
        elseif classification == "prey" and distance < PREY_NOTICE_DISTANCE then
            desired_x = desired_x * 0.25 - target_x * 0.90
            desired_y = desired_y * 0.25 - target_y * 0.90
            drive = drive * 1.65
        elseif classification == "rival" and distance < RIVAL_NOTICE_DISTANCE and
            body.temper > 0.28 then
            local approach = distance > 125 and 0.52 or -0.22
            desired_x = desired_x * 0.18 + target_x * approach -
                target_y * body.spin * 0.88
            desired_y = desired_y * 0.18 + target_y * approach +
                target_x * body.spin * 0.88
            drive = drive * (1.30 + body.temper * 0.45)
        elseif classification == "danger" and distance < DANGER_NOTICE_DISTANCE and
            body.temper > 0.42 then
            local lead_x = player.x + player.vx * 10 - body.x
            local lead_y = player.y + player.vy * 10 - body.y
            local lead_distance = math.max(0.5, length(lead_x, lead_y))
            desired_x = desired_x * 0.22 + lead_x / lead_distance
            desired_y = desired_y * 0.22 + lead_y / lead_distance
            drive = drive * (1.45 + body.temper * 0.65)
        end
    end

    local desired_length = math.max(0.01, length(desired_x, desired_y))
    body.vx = (body.vx + desired_x / desired_length * drive) * 0.986
    body.vy = (body.vy + desired_y / desired_length * drive) * 0.986
    local speed = length(body.vx, body.vy)
    local limit = body.speed_limit * difficulty_scale()
    if speed > limit then
        body.vx = body.vx / speed * limit
        body.vy = body.vy / speed * limit
    end
end

local function body_consumes(eater, prey)
    local prey_stage = STAGES[prey.tier]
    local combined_area = eater.radius * eater.radius +
        prey.radius * prey.radius * 0.24
    eater.radius = math.sqrt(combined_area)
    if eater.tier < #STAGES and
        eater.radius >= STAGES[eater.tier + 1].radius * 0.82 then
        eater.tier = eater.tier + 1
        eater.radius = math.max(eater.radius, STAGES[eater.tier].radius * 0.76)
    end
    eater.radius = math.min(eater.radius, STAGES[eater.tier].radius * 1.38)
    eater.speed_limit = math.max(0.72, eater.speed_limit * 0.985)
    body_eat_count = body_eat_count + 1
    burst(prey.x, prey.y, prey_stage.light, 4)
    place_body(prey, false)
end

local function update_body_collisions()
    if frame_count % BODY_EAT_INTERVAL_FRAMES ~= 0 then
        return
    end
    for i = 1, #bodies - 1 do
        local first = bodies[i]
        for j = i + 1, #bodies do
            local second = bodies[j]
            local dx = second.x - first.x
            local dy = second.y - first.y
            local eat_distance = (first.radius + second.radius) * 0.72
            if dx * dx + dy * dy <= eat_distance * eat_distance then
                local first_strength = first.radius + first.tier * 2.5
                local second_strength = second.radius + second.tier * 2.5
                if first_strength > second_strength * 1.10 then
                    body_consumes(first, second)
                elseif second_strength > first_strength * 1.10 then
                    body_consumes(second, first)
                else
                    local distance = math.max(0.01, length(dx, dy))
                    local nx = dx / distance
                    local ny = dy / distance
                    local overlap = eat_distance - distance + 1
                    first.x = first.x - nx * overlap * 0.5
                    first.y = first.y - ny * overlap * 0.5
                    second.x = second.x + nx * overlap * 0.5
                    second.y = second.y + ny * overlap * 0.5
                    first.vx = first.vx - nx * 0.18
                    first.vy = first.vy - ny * 0.18
                    second.vx = second.vx + nx * 0.18
                    second.vy = second.vy + ny * 0.18
                end
            end
        end
    end
end

local function update_bodies()
    local radius = player_radius()
    local capture_padding = math.min(52, player.magnet * 0.55)
    update_body_groups()
    for i = 1, #bodies do
        local body = bodies[i]
        local to_player_x = player.x - body.x
        local to_player_y = player.y - body.y
        local distance = length(to_player_x, to_player_y)
        local capture_distance = radius + body.radius + capture_padding
        update_body_motion(body, classify_body(body), to_player_x, to_player_y,
            distance, capture_distance)
        body.x = body.x + body.vx
        body.y = body.y + body.vy
        body.phase = body.phase + 0.025
        local dx = body.x - player.x
        local dy = body.y - player.y
        local distance_squared = dx * dx + dy * dy
        if distance_squared > RECYCLE_DISTANCE * RECYCLE_DISTANCE then
            place_body(body, false)
        end
    end
    update_body_collisions()
    for i = 1, #bodies do
        local body = bodies[i]
        local dx = body.x - player.x
        local dy = body.y - player.y
        local distance_squared = dx * dx + dy * dy
        if distance_squared <= (radius + body.radius) * (radius + body.radius) then
            collide_body(body)
            if state == "dead" then
                return
            end
        end
    end
end

local function update_fragments()
    for i = #fragments, 1, -1 do
        local fragment = fragments[i]
        local dx = player.x - fragment.x
        local dy = player.y - fragment.y
        local distance = length(dx, dy)
        if distance < player.magnet and distance > 0.5 then
            local pull = 0.18 + (1 - distance / player.magnet) * 0.46
            fragment.vx = fragment.vx + dx / distance * pull
            fragment.vy = fragment.vy + dy / distance * pull
        end
        fragment.vx = fragment.vx * 0.97
        fragment.vy = fragment.vy * 0.97
        fragment.x = fragment.x + fragment.vx
        fragment.y = fragment.y + fragment.vy
        fragment.ttl = fragment.ttl - FRAME_MS
        local collect_radius = player_radius() + 5
        if distance <= collect_radius then
            player.mass = player.mass + fragment.mass
            add_particle(fragment.x, fragment.y, 0, -0.8, COLOR.cyan, 280, 2)
            table.remove(fragments, i)
        elseif fragment.ttl <= 0 then
            table.remove(fragments, i)
        end
    end
end

local function update_particles()
    for i = #particles, 1, -1 do
        local particle = particles[i]
        particle.x = particle.x + particle.vx
        particle.y = particle.y + particle.vy
        particle.vx = particle.vx * 0.96
        particle.vy = particle.vy * 0.96
        particle.ttl = particle.ttl - FRAME_MS
        if particle.ttl <= 0 then
            table.remove(particles, i)
        end
    end
end

local function update_playing(info)
    update_player(info)
    update_bodies()
    if state ~= "playing" then
        return
    end
    update_fragments()
    update_particles()
    if message_ms > 0 then
        message_ms = math.max(0, message_ms - FRAME_MS)
    end
    if player.mass >= STAGES[player.tier].threshold then
        enter_evolution()
    end
end

local function screen_position(x, y)
    return math.floor(x - player.x + center_x), math.floor(y - player.y + center_y)
end

local function draw_ring(cx, cy, radius, color)
    display.draw_circle(cx, cy, radius, color)
end

local function draw_background()
    display.clear(COLOR.space)
    local nebula_x = math.floor((width * 0.72 - player.x * 0.035) % (width + 140)) - 70
    local nebula_y = math.floor((height * 0.30 - player.y * 0.025) % (height + 160)) - 80
    display.fill_circle(nebula_x, nebula_y, 72, COLOR.nebula_outer)
    display.fill_circle(nebula_x + 8, nebula_y - 4, 53, COLOR.nebula_mid)
    display.fill_circle(nebula_x + 15, nebula_y - 9, 31, COLOR.nebula_core)

    for i = 1, #stars do
        local star = stars[i]
        local parallax = star.layer * 0.035
        local x = math.floor((star.x - player.x * parallax) % width)
        local y = math.floor((star.y - player.y * parallax) % height)
        local color = star.bright and COLOR.white or COLOR.dim
        display.fill_rect(x, y, star.size, star.size, color)
    end
end

local function draw_planet(cx, cy, radius, stage)
    radius = math.max(2, math.floor(radius))
    if stage.name == "STAR" then
        draw_ring(cx, cy, radius + 7 + math.floor(math.sin(frame_count * 0.08) * 2), stage.dark)
        display.fill_circle(cx, cy, radius, stage.body)
        display.fill_circle(cx - radius // 4, cy - radius // 4,
            math.max(2, radius * 2 // 3), stage.light)
        return
    end
    if stage.name == "VOID" then
        draw_ring(cx, cy, radius + 10, COLOR.magenta)
        draw_ring(cx, cy, radius + 7, COLOR.violet)
        display.fill_circle(cx, cy, radius, COLOR.black)
        display.fill_circle(cx - radius // 3, cy - radius // 3, 3, stage.light)
        return
    end
    display.fill_circle(cx, cy, radius, stage.dark)
    display.fill_circle(cx - radius // 5, cy - radius // 5,
        math.max(2, radius * 4 // 5), stage.body)
    display.fill_circle(cx - radius // 3, cy - radius // 3,
        math.max(1, radius // 4), stage.light)
end

local function draw_bodies()
    for i = 1, #bodies do
        local body = bodies[i]
        local x, y = screen_position(body.x, body.y)
        local margin = body.radius + 18
        if x >= -margin and y >= -margin and x <= width + margin and y <= height + margin then
            local classification = classify_body(body)
            local ring_color = COLOR.red
            if classification == "prey" then
                ring_color = COLOR.cyan_dim
            elseif classification == "rival" then
                ring_color = COLOR.amber
            end
            local pulse = classification == "danger" and math.floor(2 + math.sin(body.phase) * 2) or 1
            local speed = length(body.vx, body.vy)
            if speed > 0.35 then
                local tail = math.floor(body.radius + 8 + speed * 2)
                display.draw_line(x, y,
                    math.floor(x - body.vx / speed * tail),
                    math.floor(y - body.vy / speed * tail), ring_color)
            end
            draw_ring(x, y, math.floor(body.radius) + 4 + pulse, ring_color)
            draw_planet(x, y, body.radius, STAGES[body.tier])
        end
    end
end

local function draw_fragments()
    for i = 1, #fragments do
        local fragment = fragments[i]
        local x, y = screen_position(fragment.x, fragment.y)
        if x >= 0 and y >= 0 and x < width and y < height then
            display.fill_rect(x - 2, y - 2, 4, 4, COLOR.cyan)
        end
    end
end

local function draw_particles()
    for i = 1, #particles do
        local particle = particles[i]
        local x, y = screen_position(particle.x, particle.y)
        if x >= 0 and y >= 0 and x < width and y < height then
            display.fill_rect(x, y, particle.size, particle.size, particle.color)
        end
    end
end

local function draw_player()
    local radius = math.floor(player_radius())
    local stage = STAGES[player.tier]
    draw_ring(center_x, center_y, player.magnet, COLOR.cyan_dim)
    for i = 1, player.shields do
        local angle = frame_count * 0.045 + i * math.pi * 2 / player.shields
        local orbit = radius + 14
        local x = math.floor(center_x + math.cos(angle) * orbit)
        local y = math.floor(center_y + math.sin(angle) * orbit)
        display.fill_circle(x, y, 4, COLOR.green)
    end
    draw_planet(center_x, center_y, radius, stage)
    if player.invulnerable_ms > 0 and frame_count % 4 < 2 then
        draw_ring(center_x, center_y, radius + 6, COLOR.white)
    end

    local speed = length(player.vx, player.vy)
    if speed > 0.5 then
        local tail_x = center_x - player.vx / speed * (radius + 6)
        local tail_y = center_y - player.vy / speed * (radius + 6)
        display.draw_line(center_x, center_y, math.floor(tail_x), math.floor(tail_y), stage.light)
    end
end

local function draw_bar(x, y, bar_width, value, maximum, color)
    display.fill_round_rect(x, y, bar_width, 8, 4, COLOR.panel)
    local fill = math.floor(bar_width * clamp(value / maximum, 0, 1))
    if fill > 0 then
        display.fill_round_rect(x, y, math.max(8, fill), 8, 4, color)
    end
end

local function draw_hud()
    display.fill_round_rect(7, 7, width - 14, 54, 9, COLOR.panel)
    display.draw_round_rect(7, 7, width - 14, 54, 9, COLOR.panel_border)
    display.draw_text(17, 15, STAGES[player.tier].name, {
        color = COLOR.white,
        font_size = 16,
    })
    display.draw_text(width - 102, 15,
        string.format("G%d D%d I%d", player.growth_count, player.barrier_count, player.impact_count), {
            color = COLOR.dim,
            font_size = 11,
        })
    draw_bar(17, 38, width - 126, player.mass, STAGES[player.tier].threshold, COLOR.cyan)
    draw_bar(width - 99, 38, 82, player.integrity, 100, player.integrity < 35 and COLOR.red or COLOR.green)

    display.draw_text(8, height - 17, "CYAN EAT", { color = COLOR.cyan, font_size = 10 })
    display.draw_text_aligned(125, height - 21, 118, 15, "AMBER RAM", {
        color = COLOR.amber,
        font_size = 10,
        align = "center",
        valign = "middle",
    })
    display.draw_text_aligned(width - 100, height - 21, 92, 15, "RED EVADE", {
        color = COLOR.red,
        font_size = 10,
        align = "right",
        valign = "middle",
    })
    if message_ms > 0 and message ~= "" then
        display.draw_text_aligned(0, 76, width, 20, message, {
            color = COLOR.white,
            font_size = 15,
            align = "center",
            valign = "middle",
        })
    end
end

local function draw_modal(title, line1, line2, accent)
    local panel_width = width - 46
    local panel_height = 142
    local x = 23
    local y = center_y - panel_height // 2
    display.fill_round_rect(x, y, panel_width, panel_height, 13, COLOR.panel)
    display.draw_round_rect(x, y, panel_width, panel_height, 13, accent)
    display.draw_text_aligned(x, y + 18, panel_width, 28, title, {
        color = accent,
        font_size = 21,
        align = "center",
        valign = "middle",
    })
    display.draw_text_aligned(x + 8, y + 64, panel_width - 16, 18, line1, {
        color = COLOR.white,
        font_size = 13,
        align = "center",
        valign = "middle",
    })
    display.draw_text_aligned(x + 8, y + 92, panel_width - 16, 18, line2, {
        color = COLOR.dim,
        font_size = 12,
        align = "center",
        valign = "middle",
    })
end

local function upgrade_card_rect(index)
    local gap = 4
    local margin = 6
    local card_width = (width - margin * 2 - gap * 2) // 3
    return margin + (index - 1) * (card_width + gap), 126, card_width, 206
end

local function draw_evolution()
    display.fill_round_rect(5, 69, width - 10, 307, 12, COLOR.panel)
    display.draw_round_rect(5, 69, width - 10, 307, 12, COLOR.panel_border)
    display.draw_text_aligned(0, 79, width, 28, "CHOOSE EVOLUTION", {
        color = COLOR.white,
        font_size = 21,
        align = "center",
        valign = "middle",
    })
    for i = 1, #UPGRADE do
        local upgrade = UPGRADE[i]
        local x, y, card_width, card_height = upgrade_card_rect(i)
        display.fill_round_rect(x, y, card_width, card_height, 9, COLOR.space)
        display.draw_round_rect(x, y, card_width, card_height, 9, upgrade.color)
        display.fill_circle(x + card_width // 2, y + 40, 18, upgrade.color)
        display.draw_text_aligned(x, y + 70, card_width, 22, upgrade.name, {
            color = upgrade.color,
            font_size = 14,
            align = "center",
            valign = "middle",
        })
        display.draw_text_aligned(x + 3, y + 112, card_width - 6, 18, upgrade.line1, {
            color = COLOR.white,
            font_size = 11,
            align = "center",
            valign = "middle",
        })
        display.draw_text_aligned(x + 3, y + 141, card_width - 6, 18, upgrade.line2, {
            color = COLOR.dim,
            font_size = 10,
            align = "center",
            valign = "middle",
        })
    end
    display.draw_text_aligned(0, 345, width, 18, "TAP A CARD", {
        color = COLOR.cyan,
        font_size = 12,
        align = "center",
        valign = "middle",
    })
end

local function render()
    local draw_started_ms = system.millis()
    draw_background()
    draw_bodies()
    draw_fragments()
    draw_particles()
    draw_player()
    draw_hud()

    if state == "title" then
        local control_hint = HOVER_CONTROL and "MOVE POINTER TO STEER" or "HOLD TO ACCELERATE"
        draw_modal("COSMIC DRIFT", control_hint, "RAM AMBER. EVADE RED.", COLOR.cyan)
    elseif state == "dead" then
        draw_modal("STAR FRAGMENTS", "YOUR CORE COLLAPSED", "TAP TO RESTART", COLOR.red)
    elseif state == "complete" then
        draw_modal("COSMIC SOVEREIGN", "THE VOID ANSWERS TO YOU", "TAP FOR A NEW UNIVERSE", COLOR.violet)
    elseif state == "evolve" then
        draw_evolution()
    end
    local present_started_ms = system.millis()
    display.present()
    local present_finished_ms = system.millis()
    return present_started_ms - draw_started_ms,
        present_finished_ms - present_started_ms
end

local function handle_evolution_tap(x, y)
    for i = 1, #UPGRADE do
        local card_x, card_y, card_width, card_height = upgrade_card_rect(i)
        if x >= card_x and x < card_x + card_width and
            y >= card_y and y < card_y + card_height then
            apply_upgrade(i)
            return
        end
    end
end

local screen_created = true
local function cleanup()
    if screen_created then
        pcall(display.end_frame)
        pcall(display.deinit)
        screen_created = false
    end
end

if width <= 0 or height <= 0 then
    print("[cosmic-drift] ERROR: invalid display size")
    cleanup()
    return
end

local touch_ok, touch_info = pcall(touch.sync)
if not touch_ok then
    print("[cosmic-drift] ERROR: touch unavailable: " .. tostring(touch_info))
    cleanup()
    return
end

reset_game()
display.begin_frame({ clear = true, color = COLOR.space })
print(string.format("[cosmic-drift] ready screen=%dx%d", width, height))

local perf_window_started_ms = system.millis()
local perf_frames = 0
local perf_update_total_ms = 0
local perf_draw_total_ms = 0
local perf_present_total_ms = 0
local perf_frame_total_ms = 0
local perf_frame_max_ms = 0

while true do
    local frame_started_ms = system.millis()
    local polled, info = pcall(touch.poll)
    if not polled then
        if not touch_error_reported then
            print("[cosmic-drift] WARN: touch.poll failed: " .. tostring(info))
            touch_error_reported = true
        end
        info = { pressed = false, just_pressed = false, x = center_x, y = center_y }
    else
        touch_error_reported = false
    end
    if HOVER_CONTROL and (info.moved or info.just_pressed) then
        hover_seen = true
    end

    if state == "title" then
        update_particles()
        if info.just_pressed then
            state = "playing"
            show_message("DRIFT", 700)
        end
    elseif state == "playing" then
        update_playing(info)
    elseif state == "evolve" then
        update_particles()
        if info.just_pressed then
            handle_evolution_tap(info.x, info.y)
        end
    elseif state == "dead" or state == "complete" then
        update_particles()
        if info.just_pressed then
            reset_game()
            state = "playing"
            show_message("NEW UNIVERSE", 900)
        end
    end

    local update_finished_ms = system.millis()
    local draw_ms, present_ms = render()
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
            "H2_COSMIC_PERF state=%s frames=%d fps=%d.%d update_ms=%d draw_ms=%d present_ms=%d frame_ms=%d/%d bodies=%d body_eats=%d",
            state,
            perf_frames,
            fps_tenths // 10,
            fps_tenths % 10,
            perf_update_total_ms // perf_frames,
            perf_draw_total_ms // perf_frames,
            perf_present_total_ms // perf_frames,
            perf_frame_total_ms // perf_frames,
            perf_frame_max_ms,
            #bodies,
            body_eat_count))
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
