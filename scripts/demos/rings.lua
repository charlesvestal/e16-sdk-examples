-- Rings demo: a rainbow comet circling every ring at 50 frames a second,
-- each LED set on its own with the launcher's ring.* API. Each ring starts
-- a little further round and a little further along the rainbow.
--
-- Turn encoder 1 to change the speed, encoder 2 the tail length.
--
-- On firmware without ring.* the script only says so on the labels.
--@assign id=1 abbr="Spd" name="Speed" l=0 h=127 manual=true g=1
--@assign id=2 abbr="Tail" name="Tail length" l=0 h=127 manual=true g=2

local HAS_RING = launcher ~= nil and launcher.has("ring")

-- 48 hues around the colour wheel, full brightness (ring.* scales them
-- down to the LEDs' brightness cap).
local HR, HG, HB = {}, {}, {}
for i = 0, 47 do
  local h = i / 8                      -- 0-6: six sectors
  local s, f = math.floor(h), h - math.floor(h)
  local up, down = math.floor(255 * f), math.floor(255 * (1 - f))
  local r = ({255, down, 0, 0, up, 255})[s + 1]
  local g = ({up, 255, 255, down, 0, 0})[s + 1]
  local b = ({0, 0, up, 255, 255, down})[s + 1]
  HR[i], HG[i], HB[i] = r, g, b
end

local pos, speed, tail = 0, 4, 6       -- pos in 1/8 LED steps
local frames = 0

function system.update()
  if not HAS_RING then return end
  pos = pos + speed
  local head0 = pos // 8
  local hue0 = pos // 16
  for r = 1, 16 do
    local head = (head0 + r) % 16
    local hue = (hue0 + r * 3) % 48
    local cr, cg, cb = HR[hue], HG[hue], HB[hue]
    for led = 1, 16 do
      local d = (head - (led - 1)) % 16  -- LEDs behind the head
      if d < tail then
        local k = (tail - d) / tail
        k = k * k
        ring.set(r, led, cr * k, cg * k, cb * k)
      else
        ring.set(r, led, 0, 0, 0)
      end
    end
  end
  ring.show()
  frames = frames + 1
  if frames % 500 == 0 then print("rings: " .. frames .. " frames") end
end

function controller.onEncoderTurn(e)
  if e.increment == 0 then return end
  local d = e.increment > 0 and 1 or -1
  if e.id == 1 then
    speed = math.max(0, math.min(16, speed + d))
  elseif e.id == 2 then
    tail = math.max(1, math.min(16, tail + d))
  end
end

function page.onInit()
  if not HAS_RING then
    slots.update(1, "need")
    slots.update(2, "ring")
    return
  end
  controller.set(1, {manual = true, v = 8192})
  controller.set(2, {manual = true, v = 8192})
  system.setUpdateRate(20)
end
