-- test/rt/draw_spec.lua: draw.* (the script canvas), argument checks,
-- clipping at every edge, text, fullscreen, release.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

rt.load_string("t", [[
function page.onInit() end
function probe(f) local ok, err = pcall(f); return ok end
]])

-- --- pixel ---
rt.eval("draw.pixel(10, 20)")
S.check(rt.canvas_pixel(10, 20) == true, "pixel default on")
rt.eval("draw.pixel(10, 20, false)")
S.check(rt.canvas_pixel(10, 20) == false, "pixel off")
rt.eval("draw.pixel(11.9, 21.9, true)")
S.check(rt.canvas_pixel(11, 21) == true, "coordinates are floored like leds")
S.check(rt.canvas_pixel(12, 21) == false, "nothing else was touched by a floored write")

-- --- clipping, all four edges, including negative coordinates ---
rt.eval("draw.clear()")
rt.eval("draw.pixel(0, 0); draw.pixel(127, 0); draw.pixel(0, 53); draw.pixel(127, 53)")
S.check(rt.canvas_pixel(0, 0) and rt.canvas_pixel(127, 0) and rt.canvas_pixel(0, 53)
  and rt.canvas_pixel(127, 53), "the four corners of the 128x54 canvas are reachable")
rt.eval("draw.pixel(-1, 0); draw.pixel(128, 0); draw.pixel(0, -1); draw.pixel(0, 54)")
S.check(rt.canvas_pixel(-1, 0) == false and rt.canvas_pixel(128, 0) == false
  and rt.canvas_pixel(0, 54) == false, "off-canvas pixels are clipped silently, not an error")
S.check(rt.canvas_pixel(0, 0) == true, "the clipped writes didn't touch (0,0) either")

-- --- line ---
rt.eval("draw.clear(); draw.line(2, 2, 2, 10)")
local all_set = true
for y = 2, 10 do
  if not rt.canvas_pixel(2, y) then all_set = false end
end
S.check(all_set, "a vertical line sets every pixel on it")
rt.eval("draw.clear(); draw.line(-5, 5, 5, 5)")
S.check(rt.canvas_pixel(0, 5) == true and rt.canvas_pixel(5, 5) == true,
  "a line partly off-canvas still draws its in-bounds half")

-- --- rect ---
rt.eval("draw.clear(); draw.rect(10, 10, 4, 3)") -- border only (fill defaults false)
S.check(rt.canvas_pixel(10, 10) and rt.canvas_pixel(13, 10) and rt.canvas_pixel(10, 12)
  and rt.canvas_pixel(13, 12), "rect border corners are set")
S.check(rt.canvas_pixel(11, 11) == false, "rect border doesn't fill the middle")
rt.eval("draw.clear(); draw.rect(10, 10, 4, 3, true)")
S.check(rt.canvas_pixel(11, 11) == true, "rect fill=true fills the middle")
rt.eval("draw.rect(10, 10, 4, 3, true, false)")
S.check(rt.canvas_pixel(11, 11) == false, "rect on=false clears pixels instead of setting them")
rt.eval("draw.clear(); draw.rect(-2, -2, 6, 6, true)")
S.check(rt.canvas_pixel(0, 0) == true, "a filled rect straddling the top-left corner still draws in-bounds")

-- --- text (the screen's font, as on the device) ---
local function any_in(x0, y0, x1, y1)
  for y = y0, y1 do
    for x = x0, x1 do
      if rt.canvas_pixel(x, y) then return true end
    end
  end
  return false
end
rt.eval("draw.clear(); draw.text(0, 0, 'A')")
S.check(any_in(0, 0, 6, 7), "text draws a glyph at its top-left")
S.check(not any_in(8, 0, 20, 10) and not any_in(0, 9, 10, 20), "and nothing outside the glyph's box")
S.check(rt.canvas_pixel(0, 0) == false, "real glyphs, not filled blocks: 'A' leaves its top-left corner blank")
rt.eval("draw.clear(); draw.text(0, 0, 'I')")
local narrow = 0
for x = 0, 7 do
  if any_in(x, 0, x, 7) then narrow = narrow + 1 end
end
S.check(narrow < 5, "proportional: 'I' is narrower than a 5-wide block (" .. narrow .. " columns)")
rt.eval("draw.clear(); draw.text(124, 0, 'AB')")
S.check(any_in(124, 0, 127, 7), "a character at the right edge still draws its in-bounds part")
S.check(rt.canvas_pixel(132, 0) == false, "text off the right edge is clipped, not an error")
S.check(rt.eval("return probe(function() draw.text(0, 0, 42) end)") == false,
  "draw.text rejects a non-string")

-- --- huge and non-finite coordinates ---
local t0 = os.clock()
rt.eval("draw.clear(); draw.rect(0, 0, 1e9, 1e9, true)")
local full = true
for y = 0, 53 do
  for x = 0, 127 do
    if not rt.canvas_pixel(x, y) then full = false end
  end
end
S.check(full, "draw.rect(0, 0, 1e9, 1e9, true) fills the whole canvas")
rt.eval("draw.clear(); draw.line(-1e9, -1e9, 1e9, 1e9)")
local diag = true
for i = 0, 53 do
  if not rt.canvas_pixel(i, i) then diag = false end
end
S.check(diag and not rt.canvas_pixel(1, 0) and not rt.canvas_pixel(0, 1),
  "draw.line(-1e9, -1e9, 1e9, 1e9) is the diagonal through (0,0)")
rt.eval("draw.clear(); draw.rect(-1e12, 10, 2e12, 1)")
S.check(rt.canvas_pixel(0, 10) and rt.canvas_pixel(127, 10), "a huge outline's visible edge is drawn")
rt.eval("draw.clear(); draw.line(-1e9, 5, -1e9, 1e9); draw.rect(1e9, 1e9, 5, 5, true)")
S.check(not any_in(0, 0, 127, 53), "shapes entirely off the canvas draw nothing")
rt.eval("draw.clear(); draw.text(-1e9, 0, string.rep('W', 100))")
S.check(not any_in(0, 0, 127, 53), "text far off the left edge draws nothing")
local dt = os.clock() - t0
S.check(dt < 0.5, "all of those return quickly (" .. string.format("%.3f", dt) .. " s)")
for _, call in ipairs({"draw.pixel(0/0, 0)", "draw.line(0, 0, 1/0, 5)", "draw.rect(0, 0, -1/0, 5)",
                       "draw.text(0/0, 0, 'x')"}) do
  S.check(rt.eval("return probe(function() " .. call .. " end)") == false, call .. " is an error")
end

-- --- fullscreen ---
S.check(rt.canvas_height() == 54, "canvas height is 54 by default")
rt.eval("draw.fullscreen(true)")
S.check(rt.canvas_height() == 64, "draw.fullscreen(true) grows the canvas to 64")
rt.eval("draw.clear(); draw.pixel(0, 60)")
S.check(rt.canvas_pixel(0, 60) == true, "row 60 is reachable fullscreen")
rt.eval("draw.fullscreen(false)")
S.check(rt.canvas_height() == 54, "draw.fullscreen(false) shrinks it back to 54")
S.check(rt.canvas_pixel(0, 60) == false, "row 60 isn't shown once fullscreen is off again")
S.check(rt.eval("return probe(function() draw.fullscreen(1) end)") == false,
  "draw.fullscreen rejects a non-boolean")

-- --- show ---
local shows0 = rt.canvas_shows()
rt.eval("draw.show()")
S.check(rt.canvas_shows() == shows0 + 1, "draw.show() is recorded")

-- --- release ---
rt.eval("draw.fullscreen(true); draw.pixel(5, 5); draw.release()")
S.check(rt.canvas_pixel(5, 5) == false, "draw.release() clears the canvas")
S.check(rt.canvas_fullscreen() == false, "draw.release() also turns fullscreen off")

-- --- argument errors ---
S.check(rt.eval("return probe(function() draw.pixel('x', 0) end)") == false,
  "draw.pixel rejects a non-number x")
S.check(rt.eval("return probe(function() draw.line(0, 0, 0) end)") == false,
  "draw.line requires all four coordinates")
S.check(rt.eval("return probe(function() draw.rect(0, 0, 'w', 1) end)") == false,
  "draw.rect rejects a non-number w")

-- --- width ---
S.check(rt.eval("return draw.width()") == 128, "draw.width() is 128")

-- --- a fresh load starts with a blank, non-fullscreen canvas ---
rt.eval("draw.fullscreen(true); draw.pixel(1, 1)")
rt.load_string("t2", "function page.onInit() end")
S.check(rt.canvas_fullscreen() == false, "a new load isn't fullscreen")
S.check(rt.canvas_pixel(1, 1) == false, "a new load starts with a cleared canvas")

S.done()
