local display = require("display")
local delay = require("delay")
local system = require("system")
local audio_ok, audio = pcall(require, "audio")

if not audio_ok then audio = nil end

local width = display.width
local height = display.height
local center_x = width // 2
local center_y = height // 2
local TWO_PI = math.pi * 2
local PARTICLE_COUNT = 96
local AUDIO_ACTIVE_LIMIT = 64
local FRAME_YIELD_MS = 0

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
local ACTIVE_THEME_NAME = "local_receive"
local particle_source = THEMES[ACTIVE_THEME_NAME]
local FADE_STEPS = 16
local particle_head = {}
for color_index = 1, #particle_source do
    particle_head[color_index] = {}
    for level = 1, FADE_STEPS do
        -- Reference head alpha is remaining_life * 0.9.
        particle_head[color_index][level] = scaled_color(
            particle_source[color_index], level / FADE_STEPS * 0.9)
    end
end

local particle_x, particle_y = {}, {}
local particle_vx, particle_vy = {}, {}
local particle_life, particle_max_life = {}, {}
local particle_size, particle_color = {}, {}
for i = 1, PARTICLE_COUNT do
    particle_x[i], particle_y[i] = center_x, center_y
    particle_vx[i], particle_vy[i] = 0, 0
    particle_life[i], particle_max_life[i] = 0, 1
    particle_size[i], particle_color[i] = 1, 1
end
local particle_cursor = 1
local ambient_elapsed, burst_elapsed = 0, 0
local audio_spawn_tokens = 12
local particle_active = 0
local energize_cursor = 1

local mic = nil
local energy, peak_energy, frequency_energy = 0, 0, 0
local last_audio_ms = 0
local audio_error_reported = false

local function init_audio()
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
    if mic then
        local rms, peak_or_error, brightness = mic:level(0)
        if rms then
            audio_error_reported = false
            last_audio_ms = now_ms
            local peak = peak_or_error or rms
            target_energy = clamp(math.sqrt(math.max(0, rms - 0.0007) * 19), 0, 1)
            target_peak = clamp(math.sqrt(math.max(0, peak - 0.0012) * 14), 0, 1)
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
    if not mic or now_ms - last_audio_ms > 350 then
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
end

local function spawn_particle(speed, size, life, spread, active_limit)
    if particle_active >= active_limit then return false end
    local i = particle_cursor
    for _ = 1, PARTICLE_COUNT do
        i = particle_cursor
        particle_cursor = particle_cursor % PARTICLE_COUNT + 1
        if particle_life[i] <= 0 then break end
    end
    local was_inactive = particle_life[i] <= 0
    local angle = math.random() * TWO_PI
    local launch_speed = speed * (0.72 + math.random() * 0.56)
    particle_x[i] = clamp(
        center_x + (math.random() - 0.5) * spread, 2, width - 3)
    particle_y[i] = clamp(
        center_y + (math.random() - 0.5) * spread, 2, height - 3)
    particle_vx[i] = math.cos(angle) * launch_speed
    particle_vy[i] = math.sin(angle) * launch_speed
    particle_life[i] = life * (0.75 + math.random() * 0.50)
    particle_max_life[i] = particle_life[i]
    particle_size[i] = clamp(math.floor(size + math.random() * 1.8), 1, 6)
    particle_color[i] = math.random(#particle_source)
    if was_inactive then particle_active = particle_active + 1 end
    return true
end

local function energize_particles(amount)
    local boosted, scanned = 0, 0
    while boosted < 8 and scanned < PARTICLE_COUNT do
        local i = energize_cursor
        energize_cursor = energize_cursor % PARTICLE_COUNT + 1
        scanned = scanned + 1
        if particle_life[i] > 0 then
            local vx, vy = particle_vx[i], particle_vy[i]
            local speed = math.sqrt(vx * vx + vy * vy)
            local boost = 1 + amount * 0.055
            if speed > 0 then boost = math.min(boost, 330 / speed) end
            particle_vx[i], particle_vy[i] = vx * boost, vy * boost
            particle_life[i] = math.max(
                particle_life[i], particle_max_life[i] * (0.22 + amount * 0.18))
            if amount > 0.55 and particle_size[i] < 6 and math.random() < 0.20 then
                particle_size[i] = particle_size[i] + 1
            end
            boosted = boosted + 1
        end
    end
end

local function update_particles(dt)
    local step = clamp(dt, 0.001, 0.040)
    -- SonicVis uses vx *= 0.975 per 60 Hz frame.
    local friction = math.exp(-step * 1.52)
    local burst = clamp(energy * 0.75 + peak_energy * 1.25, 0, 1)

    particle_active = 0
    for i = 1, PARTICLE_COUNT do
        if particle_life[i] > 0 then
            particle_x[i] = particle_x[i] + particle_vx[i] * step
            particle_y[i] = particle_y[i] + particle_vy[i] * step
            particle_vx[i], particle_vy[i] =
                particle_vx[i] * friction, particle_vy[i] * friction
            particle_life[i] = particle_life[i] - step
            if particle_life[i] <= 0 or particle_x[i] < 2
                or particle_x[i] > width - 3 or particle_y[i] < 2
                or particle_y[i] > height - 3 then
                particle_life[i] = 0
            else
                particle_active = particle_active + 1
            end
        end
    end

    -- Reference-density idle stream. The soft active limit prevents long
    -- sessions from turning repeated input into unbounded draw work.
    ambient_elapsed = ambient_elapsed + step
    if ambient_elapsed >= 0.05 then
        ambient_elapsed = ambient_elapsed - 0.05
        spawn_particle(18 + energy * 120, 1 + energy * 2, 1.5, 2,
            AUDIO_ACTIVE_LIMIT)
    end

    -- A token bucket keeps the first transient crisp but coalesces sustained
    -- audio. Once the budget is full, sound energizes existing particles
    -- instead of increasing the amount of AA geometry drawn every frame.
    audio_spawn_tokens = math.min(
        12, audio_spawn_tokens + step * (18 + burst * 32))
    burst_elapsed = burst > 0.10 and (burst_elapsed + step) or 0
    while burst_elapsed >= 1 / 30 do
        burst_elapsed = burst_elapsed - 1 / 30
        local count = 1 + math.floor(burst * 7)
        local spawned = 0
        while spawned < count and audio_spawn_tokens >= 1 do
            if not spawn_particle(30 + burst * 260, 1.5 + burst * 4.5,
                1.0 + burst * 1.3, 40, AUDIO_ACTIVE_LIMIT) then
                break
            end
            audio_spawn_tokens = audio_spawn_tokens - 1
            spawned = spawned + 1
        end
        if spawned < count then energize_particles(burst) end
    end

end

local function draw_particles(dt, render_frame)
    -- Convert the reference's 15%-per-60-Hz-frame overlay to a time-correct
    -- alpha so Desktop and AMOLED retain the same trail duration at any FPS.
    local fade_alpha = math.floor(
        (1 - (1 - REFERENCE_FRAME_FADE) ^ (dt * 60)) * 255 + 0.5)
    display.fade_to_black(clamp(fade_alpha, 1, 160))

    local active = particle_active
    local drawn = 0
    local light_stride = active >= 76 and 3 or (active >= 60 and 2 or 1)
    local light_phase = render_frame % light_stride
    for i = 1, PARTICLE_COUNT do
        if particle_life[i] > 0 then
            local fraction = clamp(
                particle_life[i] / particle_max_life[i], 0, 1)
            local fade = clamp(
                math.ceil(fraction * FADE_STEPS), 1, FADE_STEPS)
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
                    particle_head[particle_color[i]][fade])
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
display.begin_frame({ clear = true, color = BLACK })
print(string.format(
    "[intercom-ui] ready screen=%dx%d particles=%d audio_limit=%d theme=%s trail=framebuffer-fade uncapped=1",
    width, height, PARTICLE_COUNT, AUDIO_ACTIVE_LIMIT, ACTIVE_THEME_NAME))

local last_ms = system.millis()
local perf_started_ms = last_ms
local perf_frames, perf_render_ms, render_frame = 0, 0, 0
while true do
    local now_ms = system.millis()
    local elapsed_ms = now_ms - last_ms
    last_ms = now_ms
    local dt = clamp(elapsed_ms / 1000, 0.001, 0.05)
    sample_audio(now_ms, dt)
    update_particles(dt)
    render_frame = render_frame + 1
    local render_started_ms = system.millis()
    local active, drawn = draw_particles(dt, render_frame)
    display.present()
    local rendered_ms = system.millis()
    perf_render_ms = perf_render_ms + rendered_ms - render_started_ms
    perf_frames = perf_frames + 1
    if now_ms - perf_started_ms >= 2000 then
        local fps = perf_frames * 1000 / math.max(1, now_ms - perf_started_ms)
        print(string.format(
            "[intercom-ui] perf mode=particles fps=%.1f render_ms=%.1f active=%d drawn=%d energy=%.3f peak=%.3f hf=%.3f",
            fps, perf_render_ms / math.max(1, perf_frames), active, drawn,
            energy, peak_energy, frequency_energy))
        perf_started_ms, perf_frames, perf_render_ms = now_ms, 0, 0
    end
    delay.delay_ms(FRAME_YIELD_MS)
end
