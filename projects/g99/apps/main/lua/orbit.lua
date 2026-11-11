-- Orbit: the built-in G99 sample app. Geometry only; no packaged data.
-- OK reverses the ship, UP/DOWN change orbit. Collect stars, avoid meteors.
-- Main owns the HOME key and stops this job when it is pressed.

local runtime = require("runtime")
local display = require("display")
local delay = require("delay")
local system = require("system")

local a = type(args) == "table" and args or {}
local function int_arg(key, default)
  local value = tonumber(a[key])
  if value then
    return math.floor(value)
  end
  return default
end

local FRAME_MS = 33
local W = display.width
local H = display.height
if W <= 0 or H <= 0 then
  return "invalid display"
end
local K = math.min(W, H) / 240
local CX = W // 2
local CY = H // 2

local function px(v)
  return math.max(1, math.floor(v * K + 0.5))
end

local function rgb(r, g, b)
  return { r = r, g = g, b = b }
end

local SKY = rgb(16, 10, 40)
local RING = rgb(74, 52, 140)
local PLANET = rgb(155, 92, 255)
local PLANET_HI = rgb(201, 167, 255)
local SHIP = rgb(241, 237, 248)
local STAR = rgb(255, 209, 102)
local METEOR = rgb(255, 92, 122)
local SCORE = rgb(98, 227, 160)

local ORBITS = { px(58), px(86) }
local PLANET_R = px(24)

local buttons = {}
for _, name in ipairs({ "up", "down", "ok" }) do
  local id = int_arg(name, 0)
  if id > 0 then
    local component = runtime.components.get(id)
    if component then
      buttons[name] = { component = component, last = 0 }
    end
  end
end

local function pressed(name)
  local button = buttons[name]
  if not button then
    return false
  end
  local level = button.component.get_key_level()
  if level == nil then
    return false
  end
  local edge = level == 1 and button.last ~= 1
  button.last = level
  return edge
end

-- system.time() is nil until the wall clock is set (a device without
-- network time); the monotonic clock still varies the seed per run.
math.randomseed((system.time() or system.millis() or 0) + W * 7 + H * 13)

local ship = { angle = 0, dir = 1, orbit = 1 }
local stars = {}
local meteors = {}
local score = 0
local best = 0
local tick = 0

local function spawn_star()
  stars[#stars + 1] = {
    angle = math.random() * 2 * math.pi,
    orbit = math.random(1, 2),
  }
end

local function spawn_meteor()
  meteors[#meteors + 1] = {
    angle = math.random() * 2 * math.pi,
    dist = math.max(W, H) * 0.5 + px(10),
    speed = (0.7 + math.random() * 0.6) * K,
  }
end

local function polar(angle, radius)
  return CX + math.floor(math.cos(angle) * radius + 0.5),
    CY + math.floor(math.sin(angle) * radius + 0.5)
end

local function clamp_x(x)
  return math.max(0, math.min(W - 1, x))
end

local function clamp_y(y)
  return math.max(0, math.min(H - 1, y))
end

local function draw_ring(radius)
  local dot = px(1)
  for i = 0, 47 do
    local x, y = polar(i * math.pi / 24, radius)
    display.fill_circle(clamp_x(x), clamp_y(y), dot, RING)
  end
end

local function draw_ship()
  local radius = ORBITS[ship.orbit]
  local x, y = polar(ship.angle, radius)
  local heading = ship.angle + ship.dir * math.pi / 2
  local size = px(9)
  local function corner(offset, scale)
    return clamp_x(
      x + math.floor(math.cos(heading + offset) * size * scale + 0.5)
    ),
      clamp_y(y + math.floor(math.sin(heading + offset) * size * scale + 0.5))
  end
  local x0, y0 = corner(0, 1)
  local x1, y1 = corner(2.5, 0.8)
  local x2, y2 = corner(-2.5, 0.8)
  display.fill_triangle(x0, y0, x1, y1, x2, y2, SHIP)
end

local function render()
  display.clear(SKY)
  draw_ring(ORBITS[1])
  draw_ring(ORBITS[2])
  display.fill_circle(CX, CY, PLANET_R, PLANET)
  display.fill_circle(CX - px(7), CY - px(7), px(7), PLANET_HI)
  for _, star in ipairs(stars) do
    local x, y = polar(star.angle, ORBITS[star.orbit])
    display.fill_circle(clamp_x(x), clamp_y(y), px(4), STAR)
  end
  for _, meteor in ipairs(meteors) do
    local x, y = polar(meteor.angle, meteor.dist)
    if x >= 0 and x < W and y >= 0 and y < H then
      display.fill_circle(x, y, px(5), METEOR)
    end
  end
  draw_ship()
  local dot = px(3)
  for i = 1, math.min(best, 20) do
    local color = i <= score and SCORE or RING
    display.fill_circle(px(10) + (i - 1) * dot * 3, px(10), dot, color)
  end
  display.present()
end

local function hit(x0, y0, x1, y1, radius)
  local dx = x0 - x1
  local dy = y0 - y1
  return dx * dx + dy * dy <= radius * radius
end

local function update()
  if pressed("ok") then
    ship.dir = -ship.dir
  end
  if pressed("up") then
    ship.orbit = 2
  end
  if pressed("down") then
    ship.orbit = 1
  end
  ship.angle = ship.angle + ship.dir * 0.055
  local sx, sy = polar(ship.angle, ORBITS[ship.orbit])
  for i = #stars, 1, -1 do
    local x, y = polar(stars[i].angle, ORBITS[stars[i].orbit])
    if hit(sx, sy, x, y, px(11)) then
      table.remove(stars, i)
      score = score + 1
      best = math.max(best, score)
    end
  end
  for i = #meteors, 1, -1 do
    local meteor = meteors[i]
    meteor.dist = meteor.dist - meteor.speed
    local x, y = polar(meteor.angle, meteor.dist)
    if hit(sx, sy, x, y, px(12)) then
      score = 0
      table.remove(meteors, i)
    elseif meteor.dist < PLANET_R then
      table.remove(meteors, i)
    end
  end
  if #stars < 3 then
    spawn_star()
  end
  if tick % 45 == 0 and #meteors < 4 then
    spawn_meteor()
  end
  tick = tick + 1
end

display.begin_frame({ clear = true, color = SKY })
while true do
  update()
  render()
  delay.delay_ms(FRAME_MS)
end
