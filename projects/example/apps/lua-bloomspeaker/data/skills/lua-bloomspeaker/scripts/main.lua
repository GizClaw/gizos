local display = require("display")
local delay = require("delay")
local system = require("system")
local intercom = require("intercom")
local audio_ok, audio = pcall(require, "audio")
local touch_ok, touch = pcall(require, "lcd_touch")

if not audio_ok then audio = nil end
if not touch_ok then touch = nil end

local width = display.width
local height = display.height
local center_x = width // 2
local center_y = height // 2
local TWO_PI = math.pi * 2
local PARTICLE_COUNT = 96
local AUDIO_ACTIVE_LIMIT = 36
local PAIRING_ACTIVE_LIMIT = 32
local FRAME_YIELD_MS = 0
local LONG_PRESS_MS = 1000
local LONG_PRESS_MOVE_PX = 18
local SNAPSHOT_INTERVAL_MS = 33
local CONNECT_COLLAPSE_MS = 220
local MODE_CROSSFADE_MS = 420
local MODE_FADE_OUT_SECONDS = 0.38
local SHUTDOWN_FADE_MS = 520
local PARTICLE_GROW_FRACTION = 0.58
local TALKING_PARTICLE_GROW_FRACTION = 0.46
local PARTICLE_LAUNCH_DRAG = 2.20
local PARTICLE_FADE_DRAG = 5.80
local PARTICLE_MAX_RADIUS = 10
local PAIRING_HEARTBEAT_SECONDS = 1.0
local PAIRING_BLOOM_SPEED = 270
local PAIRING_BLOOM_LIFE = 0.78
local PAIRING_BLOOM_SPREAD = 10
local TALKING_LOCAL_ACTIVE_LIMIT = 10
local TALKING_REMOTE_ACTIVE_LIMIT = 10
local TALKING_TOP_Y = height * 0.18
local TALKING_BOTTOM_Y = height * 0.82
local IDLE_RENDER_WIDTH = math.min(width, 160)
local IDLE_RENDER_X = (width - IDLE_RENDER_WIDTH) // 2
local IDLE_RENDER_HEIGHT = math.min(height, 160)
local IDLE_RENDER_Y = (height - IDLE_RENDER_HEIGHT) // 2
local IDLE_PARTICLE_MIN_X = IDLE_RENDER_X + PARTICLE_MAX_RADIUS + 2
local IDLE_PARTICLE_MAX_X = IDLE_RENDER_X + IDLE_RENDER_WIDTH
    - PARTICLE_MAX_RADIUS - 3
local IDLE_PARTICLE_MIN_Y = IDLE_RENDER_Y + PARTICLE_MAX_RADIUS + 2
local IDLE_PARTICLE_MAX_Y = IDLE_RENDER_Y + IDLE_RENDER_HEIGHT
    - PARTICLE_MAX_RADIUS - 3
-- Size the pairing dirty rectangle from the worst possible particle, rather
-- than clipping particles at an arbitrary square. spawn_particle can add 28%
-- speed and 25% lifetime; motion uses launch drag until peak size and stronger
-- fade drag afterwards. The two integrated exponential segments therefore
-- give the largest retained trail radius for the actual heartbeat settings.
local pairing_max_life = PAIRING_BLOOM_LIFE * 1.25
local pairing_grow_time = pairing_max_life * PARTICLE_GROW_FRACTION
local pairing_fade_time = pairing_max_life - pairing_grow_time
local pairing_max_speed = PAIRING_BLOOM_SPEED * 1.28
local pairing_launch_distance = pairing_max_speed
    * (1 - math.exp(-PARTICLE_LAUNCH_DRAG * pairing_grow_time))
    / PARTICLE_LAUNCH_DRAG
local pairing_fade_speed = pairing_max_speed
    * math.exp(-PARTICLE_LAUNCH_DRAG * pairing_grow_time)
local pairing_fade_distance = pairing_fade_speed
    * (1 - math.exp(-PARTICLE_FADE_DRAG * pairing_fade_time))
    / PARTICLE_FADE_DRAG
local PAIRING_RENDER_RADIUS = math.ceil(
    pairing_launch_distance + pairing_fade_distance
    + PAIRING_BLOOM_SPREAD * 0.5 + PARTICLE_MAX_RADIUS + 4)
local PAIRING_RENDER_WIDTH = math.min(width, PAIRING_RENDER_RADIUS * 2)
local PAIRING_RENDER_HEIGHT = math.min(height, PAIRING_RENDER_RADIUS * 2)
local PAIRING_RENDER_X = (width - PAIRING_RENDER_WIDTH) // 2
local PAIRING_RENDER_Y = (height - PAIRING_RENDER_HEIGHT) // 2
local TALKING_RENDER_WIDTH = math.min(width, 128)
local TALKING_RENDER_X = (width - TALKING_RENDER_WIDTH) // 2
local TALKING_PARTICLE_MIN_X = TALKING_RENDER_X + PARTICLE_MAX_RADIUS + 2
local TALKING_PARTICLE_MAX_X = TALKING_RENDER_X + TALKING_RENDER_WIDTH
    - PARTICLE_MAX_RADIUS - 3

local function rgb(r, g, b)
    return { r = r, g = g, b = b }
end

local BLACK = rgb(0, 0, 0)

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function pixel(value, low, high)
    return math.floor(clamp(value, low, high) + 0.5)
end

local function scaled_color(color, amount)
    return rgb(
        math.floor(color.r * amount + 0.5),
        math.floor(color.g * amount + 0.5),
        math.floor(color.b * amount + 0.5))
end

local function reactive_rms(value)
    -- AEC-cleaned speech is deliberately quiet at the PCM boundary. Keep a
    -- small floor for codec hiss, then use a soft square-root knee so ordinary
    -- speech opens the bloom without making silence continuously burst.
    return clamp(math.sqrt(math.max(0, (value or 0) - 0.00018) * 36), 0, 1)
end

local function reactive_peak(value)
    return clamp(math.sqrt(math.max(0, (value or 0) - 0.00035) * 18), 0, 1)
end

local function ambient_bloom_drive(value)
    -- The audio front end deliberately stays sensitive, but the resting bloom
    -- should not inherit that amplified noise floor. Keep idle motion compact
    -- until the smoothed RMS rises clearly above the measured quiet baseline.
    return clamp(((value or 0) - 0.28) / 0.72, 0, 1)
end

local function audio_burst_drive(rms_energy, transient_energy)
    -- Peak is useful for attack timing, but must not dominate the bloom size:
    -- the measured idle pair (roughly 0.24 RMS / 0.50 peak after mapping) now
    -- remains closed, while ordinary speech crosses the knee quickly.
    local combined = (rms_energy or 0) * 0.85
        + (transient_energy or 0) * 0.35
    return clamp((combined - 0.38) / 0.62, 0, 1)
end

-- SonicVis does not draw a geometric tail. It covers the previous canvas with
-- 15% black on every 60 Hz frame, so old particle heads decay in place and
-- naturally form a soft motion history. The native framebuffer fade below
-- reproduces that compositing model without storing Lua-side trail points.
local REFERENCE_FRAME_FADE = 0.15
local THEMES = {
    local_receive = {
        rgb(105, 208, 255), rgb(102, 153, 255), rgb(111, 111, 255),
        rgb(145, 105, 255), rgb(184, 104, 255), rgb(224, 103, 239),
        rgb(255, 104, 198), rgb(255, 112, 157), rgb(255, 128, 127),
        rgb(255, 153, 112), rgb(136, 221, 255), rgb(200, 170, 255),
    },
    remote = {
        rgb(85, 242, 194), rgb(66, 232, 165), rgb(54, 217, 139),
        rgb(42, 207, 114), rgb(92, 242, 139), rgb(125, 255, 155),
        rgb(154, 245, 140), rgb(56, 224, 195), rgb(40, 205, 181),
        rgb(102, 245, 209), rgb(164, 255, 193), rgb(195, 255, 224),
    },
    pairing = {
        rgb(255, 230, 109), rgb(255, 216, 77), rgb(255, 200, 61),
        rgb(255, 183, 46), rgb(255, 169, 40), rgb(255, 146, 46),
        rgb(255, 240, 138), rgb(255, 244, 176), rgb(255, 209, 102),
        rgb(249, 228, 91), rgb(231, 255, 112), rgb(255, 242, 204),
    },
}
local THEME_LOCAL = 1
local THEME_REMOTE = 2
local THEME_PAIRING = 3
local THEME_SOURCES = {
    [THEME_LOCAL] = THEMES.local_receive,
    [THEME_REMOTE] = THEMES.remote,
    [THEME_PAIRING] = THEMES.pairing,
}
local FADE_STEPS = 16
local theme_heads = {}
for theme = THEME_LOCAL, THEME_PAIRING do
    theme_heads[theme] = {}
    local source = THEME_SOURCES[theme]
    for color_index = 1, #source do
        theme_heads[theme][color_index] = {}
        for level = 1, FADE_STEPS do
            -- Reference head alpha is remaining_life * 0.9.
            theme_heads[theme][color_index][level] = scaled_color(
                source[color_index], level / FADE_STEPS * 0.9)
        end
    end
end

local particle_x, particle_y = {}, {}
local particle_vx, particle_vy = {}, {}
local particle_life, particle_max_life = {}, {}
local particle_size, particle_color, particle_theme = {}, {}, {}
local particle_generation = {}
local particle_arc_active = {}
local particle_arc_start_x, particle_arc_start_y = {}, {}
local particle_arc_control_x, particle_arc_control_y = {}, {}
local particle_arc_target_x, particle_arc_target_y = {}, {}
for i = 1, PARTICLE_COUNT do
    particle_x[i], particle_y[i] = center_x, center_y
    particle_vx[i], particle_vy[i] = 0, 0
    particle_life[i], particle_max_life[i] = 0, 1
    particle_size[i], particle_color[i] = 1, 1
    particle_theme[i] = THEME_LOCAL
    particle_generation[i] = 0
    particle_arc_active[i] = false
    particle_arc_start_x[i], particle_arc_start_y[i] = center_x, center_y
    particle_arc_control_x[i], particle_arc_control_y[i] = center_x, center_y
    particle_arc_target_x[i], particle_arc_target_y[i] = center_x, center_y
end
local local_cursor, remote_cursor = 1, 65
local ambient_elapsed, remote_ambient_elapsed, burst_elapsed = 0, 0, 0
local pairing_heartbeat_elapsed = 0
local remote_burst_elapsed = 0
local audio_spawn_tokens = 12
local remote_spawn_tokens = 12
local particle_active = 0
local particle_active_local64, particle_active_remote32 = 0, 0
local energize_cursor = 1

local ui_state = "idle"
local state_entered_ms = 0
local visual_generation = 1
local transition_started_ms = -MODE_CROSSFADE_MS
local transition_active = false
local last_snapshot_ms = -SNAPSHOT_INTERVAL_MS
local remote_energy, remote_peak = 0, 0
local remote_level_raw, remote_peak_raw = 0, 0
local native_audio_owned = false
local native_level, native_peak = 0, 0

local touch_ready = false
local touch_down_x, touch_down_y = 0, 0
local touch_tracking, touch_cancelled, touch_fired = false, false, false
local touch_error_reported = false
local touch_pairing_enabled = true
do
    local ok, enabled = pcall(intercom.touch_pairing_enabled)
    if ok then touch_pairing_enabled = enabled ~= false end
end

local mic = nil
local energy, peak_energy, frequency_energy = 0, 0, 0
local last_audio_ms = 0
local audio_error_reported = false

local function init_audio()
    local sample_ok, _, _, _, _, _, _, owns_native = pcall(intercom.sample)
    native_audio_owned = sample_ok and owns_native or false
    if native_audio_owned then
        print("[intercom-ui] microphone owned by native Opus engine")
        return
    end
    if not audio then
        print("[intercom-ui] WARN: audio unavailable; using idle animation")
        return
    end
    local opened, input, err = pcall(audio.new_input, {})
    if not opened or not input then
        print("[intercom-ui] WARN: microphone unavailable: "
            .. tostring(opened and err or input))
        return
    end
    mic = input
    local info = mic:info()
    print(string.format("[intercom-ui] microphone %dHz/%dch/%dbit",
        info.sample_rate, info.channels, info.bits_per_sample))
end

local function sample_audio(now_ms, dt)
    local target_energy, target_peak, target_frequency = 0, 0, 0
    if native_audio_owned then
        last_audio_ms = now_ms
        target_energy = reactive_rms(native_level)
        target_peak = reactive_peak(native_peak)
        target_frequency = target_energy * 0.25
    elseif mic then
        local rms, peak_or_error, brightness = mic:level(0)
        if rms then
            audio_error_reported = false
            last_audio_ms = now_ms
            local peak = peak_or_error or rms
            target_energy = reactive_rms(rms)
            target_peak = reactive_peak(peak)
            local high = clamp(((brightness or 0) - 0.010) * 4.8, 0, 1)
            target_frequency = high * clamp(
                target_energy * 1.7 + target_peak * 0.4, 0, 1)
        elseif peak_or_error and not tostring(peak_or_error):find("busy", 1, true) then
            if not audio_error_reported then
                print("[intercom-ui] WARN: microphone read failed: "
                    .. tostring(peak_or_error))
                audio_error_reported = true
            end
        end
    end
    if not native_audio_owned and (not mic or now_ms - last_audio_ms > 350) then
        local seconds = now_ms / 1000
        target_energy = 0.025 + 0.010 * (0.5 + 0.5 * math.sin(seconds * 1.3))
        target_peak = target_energy * 0.45
        target_frequency = target_energy * 0.25
    end
    local attack = 1 - math.exp(-dt * 28)
    local release = 1 - math.exp(-dt * 5.2)
    energy = energy + (target_energy - energy)
        * (target_energy > energy and attack or release)
    peak_energy = peak_energy + (target_peak - peak_energy)
        * (target_peak > peak_energy and attack or (1 - math.exp(-dt * 8.5)))
    frequency_energy = frequency_energy + (target_frequency - frequency_energy)
        * (target_frequency > frequency_energy and attack or (1 - math.exp(-dt * 6)))

    local remote_target_energy = reactive_rms(remote_level_raw)
    local remote_target_peak = reactive_peak(remote_peak_raw)
    remote_energy = remote_energy + (remote_target_energy - remote_energy)
        * (remote_target_energy > remote_energy and attack or release)
    remote_peak = remote_peak + (remote_target_peak - remote_peak)
        * (remote_target_peak > remote_peak and attack
            or (1 - math.exp(-dt * 8.5)))
end

local function active_in_range(first, last)
    if transition_active then
        local active = 0
        for i = first, last do
            if particle_life[i] > 0
                and particle_generation[i] == visual_generation then
                active = active + 1
            end
        end
        return active
    end
    if first == 1 and last == PARTICLE_COUNT then return particle_active end
    if first == 1 and last == 64 then return particle_active_local64 end
    if first == 65 and last == PARTICLE_COUNT then
        return particle_active_remote32
    end
    local active = 0
    for i = first, last do
        if particle_life[i] > 0 then active = active + 1 end
    end
    return active
end

local function spawn_particle(first, last, center_px, center_py, theme,
    speed, size, life, spread, active_limit)
    if active_in_range(first, last) >= active_limit then return false end
    local cursor = first == 1 and local_cursor or remote_cursor
    -- Pairing uses all 96 slots, while talking partitions them into 1..64 and
    -- 65..96. Normalize the shared local cursor before scanning the smaller
    -- range so it cannot place a local head in the remote partition.
    if cursor < first or cursor > last then cursor = first end
    local i = cursor
    for _ = first, last do
        i = cursor
        cursor = cursor >= last and first or cursor + 1
        if particle_life[i] <= 0 then break end
    end
    if first == 1 then local_cursor = cursor else remote_cursor = cursor end
    if particle_life[i] > 0 then return false end
    local was_inactive = particle_life[i] <= 0
    local angle = math.random() * TWO_PI
    local launch_speed = speed * (0.72 + math.random() * 0.56)
    particle_x[i] = clamp(
        center_px + (math.random() - 0.5) * spread, 2, width - 3)
    particle_y[i] = clamp(
        center_py + (math.random() - 0.5) * spread, 2, height - 3)
    particle_vx[i] = math.cos(angle) * launch_speed
    particle_vy[i] = math.sin(angle) * launch_speed
    particle_life[i] = life * (0.75 + math.random() * 0.50)
    particle_max_life[i] = particle_life[i]
    particle_size[i] = clamp(
        math.floor(size + math.random() * 1.8), 1, PARTICLE_MAX_RADIUS)
    particle_theme[i] = theme
    particle_generation[i] = visual_generation
    particle_color[i] = math.random(#THEME_SOURCES[theme])
    particle_arc_active[i] = false
    if was_inactive then
        particle_active = particle_active + 1
        if i <= 64 then
            particle_active_local64 = particle_active_local64 + 1
        else
            particle_active_remote32 = particle_active_remote32 + 1
        end
    end
    return true
end

local function energize_particles(amount, first, last)
    local boosted, scanned = 0, 0
    local wanted = math.min(8, last - first + 1)
    if energize_cursor < first or energize_cursor > last then
        energize_cursor = first
    end
    while boosted < wanted and scanned <= last - first do
        local i = energize_cursor
        energize_cursor = energize_cursor >= last and first or energize_cursor + 1
        scanned = scanned + 1
        if particle_life[i] > 0 and not particle_arc_active[i] then
            local vx, vy = particle_vx[i], particle_vy[i]
            local speed = math.sqrt(vx * vx + vy * vy)
            local boost = 1 + amount * 0.025
            if speed > 0 then boost = math.min(boost, 185 / speed) end
            particle_vx[i], particle_vy[i] = vx * boost, vy * boost
            if amount > 0.55 and particle_size[i] < PARTICLE_MAX_RADIUS
                and math.random() < 0.20 then
                particle_size[i] = particle_size[i] + 1
            end
            boosted = boosted + 1
        end
    end
end

local function state_is_pairing(state)
    return state == "pairing" or state == "claiming"
        or state == "connecting" or state == "securing"
end

local function state_uses_idle_corridor(state)
    return state == "idle" or state == "error"
end

local function visual_mode(state)
    if state_is_pairing(state) then return "pairing" end
    if state == "talking" then return "talking" end
    if state == "disconnecting" then return "disconnecting" end
    return "idle"
end

local function transition_in_progress(now_ms)
    return now_ms - transition_started_ms < MODE_CROSSFADE_MS
end

local function begin_visual_transition(now_ms)
    for i = 1, PARTICLE_COUNT do
        if particle_life[i] > 0 then
            particle_life[i] = math.min(
                particle_life[i], MODE_FADE_OUT_SECONDS)
            particle_arc_active[i] = false
        end
    end
    visual_generation = visual_generation + 1
    transition_started_ms = now_ms
    ambient_elapsed = 0.05
    remote_ambient_elapsed = 0.05
    pairing_heartbeat_elapsed = PAIRING_HEARTBEAT_SECONDS
end

local function clear_range(first, last)
    for i = first, last do
        particle_life[i] = 0
        particle_arc_active[i] = false
    end
end

local function apply_state_change(next_state, entered_ms, now_ms)
    if next_state == ui_state then return end
    local previous = ui_state
    if visual_mode(previous) ~= visual_mode(next_state) then
        begin_visual_transition(now_ms)
    end
    ui_state = next_state
    state_entered_ms = entered_ms
    print(string.format("[intercom-ui] state %s -> %s", previous, next_state))
end

local function poll_state(now_ms)
    if now_ms - last_snapshot_ms < SNAPSHOT_INTERVAL_MS then return end
    last_snapshot_ms = now_ms
    local ok, state, entered_ms, local_level, local_peak,
        remote_level, sampled_remote_peak, owns_native = pcall(intercom.sample)
    if not ok then return end
    apply_state_change(state, entered_ms or now_ms, now_ms)
    native_audio_owned = owns_native or false
    native_level = clamp(local_level or 0, 0, 1)
    native_peak = clamp(local_peak or 0, 0, 1)
    -- Preserve the received raw PCM levels. sample_audio applies exactly the
    -- same nonlinear gain and attack/release smoothing used by local input;
    -- position and color theme are the only visual differences between sides.
    remote_level_raw = clamp(remote_level or 0, 0, 1)
    remote_peak_raw = clamp(sampled_remote_peak or 0, 0, 1)
end

local function init_touch()
    if not touch_pairing_enabled then
        print("[intercom-ui] physical pairing button active; touch hold disabled")
        return
    end
    if not touch then
        print("[intercom-ui] WARN: touch unavailable; pairing gesture disabled")
        return
    end
    local ok, info = pcall(touch.sync)
    if not ok then
        print("[intercom-ui] WARN: touch sync failed: " .. tostring(info))
        return
    end
    touch_ready = true
    if info.pressed then
        touch_down_x, touch_down_y = info.x, info.y
        touch_tracking = true
    end
end

local function poll_long_press()
    if not touch_pairing_enabled then return end
    if not touch_ready then return end
    local ok, info = pcall(touch.poll)
    if not ok then
        if not touch_error_reported then
            print("[intercom-ui] WARN: touch poll failed: " .. tostring(info))
            touch_error_reported = true
        end
        return
    end
    touch_error_reported = false
    if info.just_pressed then
        touch_down_x, touch_down_y = info.x, info.y
        touch_tracking, touch_cancelled, touch_fired = true, false, false
    end
    if touch_tracking and info.pressed then
        local dx, dy = info.x - touch_down_x, info.y - touch_down_y
        if dx * dx + dy * dy > LONG_PRESS_MOVE_PX * LONG_PRESS_MOVE_PX then
            touch_cancelled = true
        end
        if not touch_cancelled and not touch_fired
            and (info.held_ms or 0) >= LONG_PRESS_MS then
            local fired, result = pcall(intercom.long_press)
            touch_fired = true
            if fired then
                print("[intercom-ui] long press -> " .. tostring(result))
            else
                print("[intercom-ui] WARN: long press failed: " .. tostring(result))
            end
        end
    end
    if (info.just_released or not info.pressed) and touch_fired then
        local released, result = pcall(intercom.hold_release)
        if released then
            print("[intercom-ui] hold release -> " .. tostring(result))
        else
            print("[intercom-ui] WARN: hold release failed: " .. tostring(result))
        end
    end
    if info.just_released or not info.pressed then
        touch_tracking, touch_cancelled, touch_fired = false, false, false
    end
end

local function lerp(a, b, t)
    return a + (b - a) * clamp(t, 0, 1)
end

local function layout_for_state(now_ms)
    local elapsed = math.max(0, now_ms - state_entered_ms)
    if ui_state == "talking" then
        local t = clamp(elapsed / CONNECT_COLLAPSE_MS, 0, 1)
        t = t * t * (3 - 2 * t)
        return center_x, lerp(center_y, TALKING_BOTTOM_Y, t),
            center_x, lerp(center_y, TALKING_TOP_Y, t), 1, 1
    end
    if ui_state == "disconnecting" then
        local t = clamp(elapsed / 420, 0, 1)
        t = t * t * (3 - 2 * t)
        return center_x, lerp(TALKING_BOTTOM_Y, center_y, t),
            center_x, lerp(TALKING_TOP_Y, center_y, t), 1, 1 - t
    end
    return center_x, center_y, center_x, center_y, 1, 0
end

local function begin_particle_arc(i, target_x, target_y)
    local start_x, start_y = particle_x[i], particle_y[i]
    local dx, dy = target_x - start_x, target_y - start_y
    local distance = math.max(1, math.sqrt(dx * dx + dy * dy))
    local vx, vy = particle_vx[i], particle_vy[i]
    local speed = math.sqrt(vx * vx + vy * vy)
    if speed > 0.001 then
        vx, vy = vx / speed, vy / speed
    else
        vx, vy = dx / distance, dy / distance
    end

    -- Preserve the radial launch direction at the hand-off point, then bend
    -- the route sideways before it converges on the opposite bloom. Using
    -- preallocated scalar arrays avoids creating tables in the frame loop.
    local handle = math.min(68, distance * 0.30)
    local normal_x, normal_y = -dy / distance, dx / distance
    local bend_sign = math.random() < 0.5 and -1 or 1
    local bend = distance * (0.045 + math.random() * 0.045) * bend_sign
    particle_arc_start_x[i], particle_arc_start_y[i] = start_x, start_y
    particle_arc_control_x[i] = clamp(
        start_x + vx * handle + normal_x * bend,
        TALKING_PARTICLE_MIN_X, TALKING_PARTICLE_MAX_X)
    particle_arc_control_y[i] = clamp(
        start_y + vy * handle + normal_y * bend, 2, height - 3)
    particle_arc_target_x[i], particle_arc_target_y[i] = target_x, target_y
    particle_arc_active[i] = true
end

local function update_particles(dt, now_ms, shutting_down)
    transition_active = transition_in_progress(now_ms)
    local step = clamp(dt, 0.001, 0.040)
    local launch_friction = math.exp(-step * PARTICLE_LAUNCH_DRAG)
    local fade_friction = math.exp(-step * PARTICLE_FADE_DRAG)
    local burst = audio_burst_drive(energy, peak_energy)
    local remote_burst = audio_burst_drive(remote_energy, remote_peak)
    local local_x, local_y, remote_x, remote_y =
        layout_for_state(now_ms)

    particle_active = 0
    particle_active_local64, particle_active_remote32 = 0, 0
    for i = 1, PARTICLE_COUNT do
        if particle_life[i] > 0 then
            local age = 1 - clamp(
                particle_life[i] / particle_max_life[i], 0, 1)
            if ui_state == "talking" then
                if age < TALKING_PARTICLE_GROW_FRACTION then
                    particle_x[i] = particle_x[i] + particle_vx[i] * step
                    particle_y[i] = particle_y[i] + particle_vy[i] * step
                    particle_vx[i], particle_vy[i] =
                        particle_vx[i] * launch_friction,
                        particle_vy[i] * launch_friction
                else
                    if not particle_arc_active[i] then
                        if i <= 64 then
                            begin_particle_arc(i, remote_x, remote_y)
                        else
                            begin_particle_arc(i, local_x, local_y)
                        end
                    end
                    local arc_t = clamp(
                        (age - TALKING_PARTICLE_GROW_FRACTION)
                        / (1 - TALKING_PARTICLE_GROW_FRACTION), 0, 1)
                    arc_t = arc_t * arc_t * (3 - 2 * arc_t)
                    local inverse = 1 - arc_t
                    particle_x[i] = inverse * inverse * particle_arc_start_x[i]
                        + 2 * inverse * arc_t * particle_arc_control_x[i]
                        + arc_t * arc_t * particle_arc_target_x[i]
                    particle_y[i] = inverse * inverse * particle_arc_start_y[i]
                        + 2 * inverse * arc_t * particle_arc_control_y[i]
                        + arc_t * arc_t * particle_arc_target_y[i]
                end
            else
                particle_arc_active[i] = false
                particle_x[i] = particle_x[i] + particle_vx[i] * step
                particle_y[i] = particle_y[i] + particle_vy[i] * step
                -- Outside a call, the stronger post-peak drag keeps each
                -- splash compact instead of sending it across the display.
                local friction = age < PARTICLE_GROW_FRACTION
                    and launch_friction or fade_friction
                particle_vx[i], particle_vy[i] =
                    particle_vx[i] * friction, particle_vy[i] * friction
            end
            if ui_state == "talking" then
                particle_x[i] = clamp(particle_x[i],
                    TALKING_PARTICLE_MIN_X, TALKING_PARTICLE_MAX_X)
            end
            particle_life[i] = particle_life[i] - step
            if ui_state == "disconnecting" and i >= 65 then
                local collapse = 8.5 * step
                particle_x[i] = lerp(particle_x[i], remote_x, collapse)
                particle_y[i] = lerp(particle_y[i], remote_y, collapse)
                particle_life[i] = particle_life[i] - step * 2.2
            end
            local outside_idle_corridor =
                state_uses_idle_corridor(ui_state)
                and not transition_in_progress(now_ms)
                and (particle_x[i] < IDLE_PARTICLE_MIN_X
                    or particle_x[i] > IDLE_PARTICLE_MAX_X
                    or particle_y[i] < IDLE_PARTICLE_MIN_Y
                    or particle_y[i] > IDLE_PARTICLE_MAX_Y)
            if particle_life[i] <= 0 or outside_idle_corridor
                or particle_x[i] < 2
                or particle_x[i] > width - 3 or particle_y[i] < 2
                or particle_y[i] > height - 3 then
                particle_life[i] = 0
            else
                particle_active = particle_active + 1
                if i <= 64 then
                    particle_active_local64 = particle_active_local64 + 1
                else
                    particle_active_remote32 = particle_active_remote32 + 1
                end
            end
        end
    end

    local talking = ui_state == "talking" or ui_state == "disconnecting"
    local pairing = state_is_pairing(ui_state)
    local local_last = talking and 64 or PARTICLE_COUNT
    local local_theme = pairing and THEME_PAIRING or THEME_LOCAL
    local local_limit = talking and TALKING_LOCAL_ACTIVE_LIMIT
        or AUDIO_ACTIVE_LIMIT

    local state_elapsed = math.max(0, now_ms - state_entered_ms)
    if ui_state == "talking" and state_elapsed < CONNECT_COLLAPSE_MS then
        for i = 1, 64 do
            if particle_life[i] > 0 then
                local pull = 10 * step
                particle_x[i] = lerp(particle_x[i], center_x, pull)
                particle_y[i] = lerp(particle_y[i], center_y, pull)
            end
        end
        return
    end

    if shutting_down then return end

    -- Pairing uses a deliberate 1 Hz heartbeat. Each beat replaces the small
    -- center heads with one full-strength radial bloom; its high launch speed
    -- reaches the visual's maximum radius near peak size, then the stronger
    -- post-peak drag makes it disappear before the next beat.
    if pairing then
        pairing_heartbeat_elapsed = pairing_heartbeat_elapsed + step
        if pairing_heartbeat_elapsed >= PAIRING_HEARTBEAT_SECONDS then
            pairing_heartbeat_elapsed = pairing_heartbeat_elapsed
                - PAIRING_HEARTBEAT_SECONDS
            clear_range(1, PARTICLE_COUNT)
            particle_active = 0
            particle_active_local64, particle_active_remote32 = 0, 0
            for _ = 1, 24 do
                spawn_particle(1, PARTICLE_COUNT, center_x, center_y,
                    THEME_PAIRING, PAIRING_BLOOM_SPEED, 8.5,
                    PAIRING_BLOOM_LIFE, PAIRING_BLOOM_SPREAD,
                    PAIRING_ACTIVE_LIMIT)
            end
        end
    else
        pairing_heartbeat_elapsed = 0
    end

    -- Keep a restrained center seed between pairing heartbeats. The
    -- conversation layout uses fixed 64/32 slots.
    ambient_elapsed = ambient_elapsed + step
    local ambient_interval = pairing and 0.12 or 0.05
    if ambient_elapsed >= ambient_interval then
        ambient_elapsed = ambient_elapsed - ambient_interval
        if pairing then
            spawn_particle(1, local_last, local_x, local_y, local_theme,
                28, 3.5, 0.72, 4, PAIRING_ACTIVE_LIMIT)
        else
            local drive = ambient_bloom_drive(energy)
            local size = talking and (3.0 + drive * 4.0)
                or (2.0 + drive * 3.0)
            spawn_particle(1, local_last, local_x, local_y, local_theme,
                18 + drive * 120, size, 1.5, 2, local_limit)
        end
    end

    -- A token bucket keeps the first transient crisp but coalesces sustained
    -- audio. Once the budget is full, sound energizes existing particles
    -- instead of increasing the amount of AA geometry drawn every frame.
    audio_spawn_tokens = math.min(
        12, audio_spawn_tokens + step * (18 + burst * 32))
    burst_elapsed = burst > 0.05 and (burst_elapsed + step) or 0
    while burst_elapsed >= 1 / 30 do
        burst_elapsed = burst_elapsed - 1 / 30
        local count = 1 + math.floor(burst * 7)
        local size = talking and (4.0 + burst * 6.0)
            or (3.0 + burst * 5.0)
        local spawned = 0
        while spawned < count and audio_spawn_tokens >= 1 do
            if not spawn_particle(1, local_last, local_x, local_y, local_theme,
                22 + burst * 170, size,
                0.62 + burst * 0.58, 24, local_limit) then
                break
            end
            audio_spawn_tokens = audio_spawn_tokens - 1
            spawned = spawned + 1
        end
        if spawned < count then energize_particles(burst, 1, local_last) end
    end

    if ui_state == "talking" then
        remote_ambient_elapsed = remote_ambient_elapsed + step
        if remote_ambient_elapsed >= 0.05 then
            remote_ambient_elapsed = remote_ambient_elapsed - 0.05
            local drive = ambient_bloom_drive(remote_energy)
            spawn_particle(65, PARTICLE_COUNT, remote_x, remote_y,
                THEME_REMOTE, 18 + drive * 120,
                3.0 + drive * 4.0, 1.5, 2,
                TALKING_REMOTE_ACTIVE_LIMIT)
        end
        remote_spawn_tokens = math.min(
            12, remote_spawn_tokens + step * (18 + remote_burst * 32))
        remote_burst_elapsed = remote_burst > 0.05
            and (remote_burst_elapsed + step) or 0
        while remote_burst_elapsed >= 1 / 30 do
            remote_burst_elapsed = remote_burst_elapsed - 1 / 30
            local count = 1 + math.floor(remote_burst * 7)
            local spawned = 0
            while spawned < count and remote_spawn_tokens >= 1 do
                if not spawn_particle(65, PARTICLE_COUNT, remote_x, remote_y,
                    THEME_REMOTE, 22 + remote_burst * 170,
                    4.0 + remote_burst * 6.0,
                    0.62 + remote_burst * 0.58,
                    24, TALKING_REMOTE_ACTIVE_LIMIT) then
                    break
                end
                remote_spawn_tokens = remote_spawn_tokens - 1
                spawned = spawned + 1
            end
            if spawned < count then
                energize_particles(remote_burst, 65, PARTICLE_COUNT)
            end
        end
    end

end

local function draw_particles(dt, render_frame, now_ms, shutting_down)
    -- Convert the reference's 15%-per-60-Hz-frame overlay to a time-correct
    -- alpha so Desktop and AMOLED retain the same trail duration at any FPS.
    local fade_alpha = math.floor(
        (1 - (1 - REFERENCE_FRAME_FADE) ^ (dt * 60)) * 255 + 0.5)
    if shutting_down or transition_in_progress(now_ms) then
        display.fade_to_black(clamp(math.max(fade_alpha,
            shutting_down and 36 or 1), 1, 160))
    elseif ui_state == "talking" then
        -- All call particles are constrained to this central corridor. Fading
        -- and transferring 128x448 instead of 368x448 is the main AMOLED FPS
        -- optimization; reducing the Lua pool alone cannot beat full-screen
        -- RGB565 transfer bandwidth.
        display.fade_rect_to_black(
            TALKING_RENDER_X, 0, TALKING_RENDER_WIDTH, height,
            clamp(fade_alpha, 1, 160))
    elseif state_uses_idle_corridor(ui_state) then
        -- The single bloom occupies only the center of the panel. Restricting
        -- fade and dirty upload to its 160x160 square avoids transferring the
        -- surrounding unchanged black pixels on every frame.
        display.fade_rect_to_black(
            IDLE_RENDER_X, IDLE_RENDER_Y,
            IDLE_RENDER_WIDTH, IDLE_RENDER_HEIGHT,
            clamp(fade_alpha, 1, 160))
    elseif state_is_pairing(ui_state) then
        -- This region is derived from the heartbeat's maximum speed, lifetime,
        -- drag, spread, and particle radius. No live particle is killed at its
        -- edge, so the optimization cannot reveal a square clipping boundary.
        display.fade_rect_to_black(
            PAIRING_RENDER_X, PAIRING_RENDER_Y,
            PAIRING_RENDER_WIDTH, PAIRING_RENDER_HEIGHT,
            clamp(fade_alpha, 1, 160))
    else
        display.fade_to_black(clamp(fade_alpha, 1, 160))
    end

    local active = particle_active
    local drawn = 0
    local light_stride = active >= 72 and 3 or (active >= 32 and 2 or 1)
    local light_phase = render_frame % light_stride
    local grow_fraction = ui_state == "talking"
        and TALKING_PARTICLE_GROW_FRACTION or PARTICLE_GROW_FRACTION
    for i = 1, PARTICLE_COUNT do
        if particle_life[i] > 0 then
            local remaining = clamp(
                particle_life[i] / particle_max_life[i], 0, 1)
            local age = 1 - remaining
            local fraction
            local opacity
            if age < grow_fraction then
                local grow = clamp(age / grow_fraction, 0, 1)
                fraction = grow * grow * (3 - 2 * grow)
                opacity = 0.30 + fraction * 0.70
            else
                local fade = clamp((age - grow_fraction)
                    / (1 - grow_fraction), 0, 1)
                local inverse = 1 - fade
                fraction = inverse ^ 1.45
                opacity = inverse ^ 1.70
            end
            if particle_generation[i] == visual_generation then
                opacity = opacity * clamp(
                    (now_ms - transition_started_ms) / MODE_CROSSFADE_MS,
                    0, 1)
            end
            local fade = clamp(
                math.ceil(opacity * FADE_STEPS), 1, FADE_STEPS)
            local radius = clamp(
                math.ceil(particle_size[i] * fraction), 1, particle_size[i])
            -- Retained framebuffer history makes it safe to interleave only
            -- the small heads under sustained load. Large/bright particles
            -- remain full-rate, while old positions continue to fade smoothly.
            if light_stride == 1 or radius >= 3
                or i % light_stride == light_phase then
                display.fill_circle_aa(
                    pixel(particle_x[i], 0, width - 1),
                    pixel(particle_y[i], 0, height - 1),
                    radius,
                    theme_heads[particle_theme[i]][particle_color[i]][fade])
                drawn = drawn + 1
            end
        end
    end
    return active, drawn
end

local screen_open = true
local function cleanup()
    if mic then pcall(mic.close, mic); mic = nil end
    if screen_open then
        pcall(display.end_frame)
        pcall(display.deinit)
        screen_open = false
    end
end

if width <= 0 or height <= 0 then
    print("[intercom-ui] ERROR: invalid display size")
    cleanup()
    return
end

math.randomseed(system.millis() + width * 17 + height * 31)
init_audio()
init_touch()
display.begin_frame({ clear = true, color = BLACK })
transition_started_ms = system.millis()
print(string.format(
    "[intercom-ui] ready screen=%dx%d particles=%d layout=64+32 pair_input=%s long_press_ms=%d trail=framebuffer-fade uncapped=1",
    width, height, PARTICLE_COUNT,
    touch_pairing_enabled and "touch" or "button", LONG_PRESS_MS))

local last_ms = system.millis()
local perf_started_ms = last_ms
local perf_frames, perf_render_ms, render_frame = 0, 0, 0
local shutting_down = false
local shutdown_started_ms = 0
while true do
    local now_ms = system.millis()
    local elapsed_ms = now_ms - last_ms
    last_ms = now_ms
    local dt = clamp(elapsed_ms / 1000, 0.001, 0.05)
    poll_long_press()
    poll_state(now_ms)
    if not shutting_down then
        local ok, requested = pcall(intercom.shutdown_requested)
        if ok and requested then
            shutting_down = true
            shutdown_started_ms = now_ms
            begin_visual_transition(now_ms)
            print("[intercom-ui] power-off fade started")
        end
    end
    sample_audio(now_ms, dt)
    update_particles(dt, now_ms, shutting_down)
    render_frame = render_frame + 1
    local render_started_ms = system.millis()
    local active, drawn = draw_particles(
        dt, render_frame, now_ms, shutting_down)
    display.present()
    local rendered_ms = system.millis()
    if shutting_down
        and rendered_ms - shutdown_started_ms >= SHUTDOWN_FADE_MS then
        display.clear(BLACK)
        display.present()
        print("[intercom-ui] power-off fade complete")
        cleanup()
        return
    end
    perf_render_ms = perf_render_ms + rendered_ms - render_started_ms
    perf_frames = perf_frames + 1
    if now_ms - perf_started_ms >= 2000 then
        local fps = perf_frames * 1000 / math.max(1, now_ms - perf_started_ms)
        print(string.format(
            "[intercom-ui] perf state=%s mode=particles fps=%.1f render_ms=%.1f active=%d local=%d remote_count=%d drawn=%d energy=%.3f peak=%.3f hf=%.3f remote=%.3f remote_peak=%.3f",
            ui_state, fps, perf_render_ms / math.max(1, perf_frames), active,
            particle_active_local64, particle_active_remote32, drawn,
            energy, peak_energy, frequency_energy,
            remote_energy, remote_peak))
        perf_started_ms, perf_frames, perf_render_ms = now_ms, 0, 0
    end
    delay.delay_ms(FRAME_YIELD_MS)
end
