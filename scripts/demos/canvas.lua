-- Canvas demo: a bouncing ball over a scrolling sine wave, drawn with the
-- launcher's draw.* API at 50 frames a second.
--
-- Press encoder 1 to switch fullscreen on or off (128x64 without the title
-- bar, 128x54 under it). Turn encoder 1 to change the wave's speed.
--
-- On firmware without draw.* (the stock E16, or an older launcher) the
-- script only says so on the labels: launcher.has() is the feature check,
-- and `launcher` itself doesn't exist on the stock firmware.
--@assign id=1  abbr="Spd" name="Wave speed" l=0 h=127 manual=true g=1
--@assign id=17 abbr="Full" name="Fullscreen" p=true g=1

local HAS_DRAW = launcher ~= nil and launcher.has("draw")

local W, H = 128, 54
local bx, by, vx, vy, R = 20, 12, 1.7, 1.1, 3
local phase, speed = 0, 4
local full = false
local frames = 0

local function frame()
  draw.clear()
  -- the wave: one segment per 4 columns, scrolling left
  local mid, amp = H * 0.62, H * 0.3
  local px, py
  for x = 0, W, 4 do
    local y = mid + amp * math.sin((x + phase) * 0.06)
    if px then draw.line(px, py, x, y) end
    px, py = x, y
  end
  -- the ball: bounce off the four edges
  bx, by = bx + vx, by + vy
  if bx < R or bx > W - 1 - R then vx = -vx; bx = bx < R and R or W - 1 - R end
  if by < R or by > H - 1 - R then vy = -vy; by = by < R and R or H - 1 - R end
  draw.rect(bx - R, by - R, 2 * R + 1, 2 * R + 1, true)
  draw.rect(bx - R + 1, by - R + 1, 2 * R - 1, 2 * R - 1, true, false)
  draw.pixel(bx, by)
  draw.text(1, 0, full and "128x64" or "128x54")
  draw.show()
  phase = phase + speed * 0.5
  frames = frames + 1
  if frames % 500 == 0 then print("canvas: " .. frames .. " frames") end
end

function system.update()
  if HAS_DRAW then frame() end
end

function controller.onEncoderTurn(e)
  if e.id ~= 1 or e.increment == 0 then return end
  speed = math.max(0, math.min(16, speed + (e.increment > 0 and 1 or -1)))
  slots.update(1, "S" .. speed)
end

function controller.onEncoderPress(e)
  if e.id ~= 17 or not HAS_DRAW then return end
  full = not full
  draw.fullscreen(full)
  H = draw.height()
  if by > H - 1 - R then by = H - 1 - R end
end

function page.onInit()
  if not HAS_DRAW then
    slots.update(1, "need")
    slots.update(2, "draw")
    return
  end
  controller.set(1, {manual = true, v = 8192})
  H = draw.height()
  system.setUpdateRate(20)
end
