-- test/host/e16mock.lua: a drop-in for external/oxi-e16-lua/test/e16mock.lua
-- that runs the scripts on the native runtime (src/rt, through the host
-- binary's `rt` module, test/host/rt_module.c) instead of the Lua mock.
-- test/run_suites.sh puts it where the suites dofile("test/e16mock.lua").
--
-- Everything the scripts call (midi, leds, var, clock, ...) is the runtime's
-- C implementation. This file is only harness glue:
--
--  * M's functions drive the runtime: load (with the scene's wiring parsed
--    from the script's --@assign lines), turn/press/release (test ids back
--    to physical encoders), show, run, extStart/extStop/extContinue.
--  * M's fields (now, rate, sent, rings, labels, title, store, page,
--    listening, res, bpm, tp, cerrs, hold) read the runtime's state when
--    accessed. The ones the suites write -- sent (cleared), page (set
--    silently, as the mock's M.page = n does), store (vars seeded,
--    edited, cleared) -- are pushed back into the runtime.
--  * The suites also reach into the script's globals directly (calling
--    controller.onEncoderTurn{...} or page.onVarChange, reading
--    store.data, swapping midi.sendCC for a counting stub). In the mock
--    the test and the script share a lua_State; here the script has its
--    own, so the harness gets proxy tables for those globals that read
--    and write the script's through rt.get/rt.set (values copied across,
--    functions bridged both ways; see rt_module.c).
--  * collectgarbage and math.randomseed apply to the script's state too,
--    so "no per-tick garbage" measures the script (plus runtime) heap and
--    a seed reaches the script's math.random, as with one shared state.
local rt = require("rt")

local M = {fails = 0}

function M.check(ok, msg)
  if ok then print("ok   " .. msg) else M.fails = M.fails + 1; print("FAIL: " .. msg) end
end

function M.done()
  print(M.fails == 0 and "\nALL PASSED" or ("\n" .. M.fails .. " FAILED"))
  os.exit(M.fails == 0 and 0 or 1)
end

-- Each script's make_scene flags (oxi-e16-lua's README): pad pages and the
-- settings page. Stock scenes have 12 pages.
local SCENES = {
  euclid = {pad = 1, settings = 2},
  lfo    = {pad = 4, settings = 5},
  chords = {pad = 11, settings = 12},
  modseq = {pad = 1, settings = 2},
  tb3po  = {pad = 1, settings = 2},
}
local scene = {pad = 1, settings = 2}

-- --- the script's globals, as seen from the test ---

local function proxy(name)
  return setmetatable({}, {
    __index = function(_, k) return rt.get(name .. "." .. k) end,
    __newindex = function(_, k, v) rt.set(name .. "." .. k, v) end,
  })
end
for _, n in ipairs{"midi", "leds", "slots", "page", "controller", "var", "system", "clock", "store"} do
  _G[n] = proxy(n)
end

-- collectgarbage() also collects the script's state, and "count" reports
-- the script's heap: the suites' "no per-tick garbage" checks measure it.
--
-- In the mock, collectgarbage() runs on the one state the test and the
-- script share, whose stack the test's own frames keep in use, so a full
-- collect leaves it alone. The script's own state is idle when the test
-- calls it, and Lua's GC shrinks an idle thread's stack and CallInfo list;
-- the next callback grows them back, a one-off ~0.8 KB that doesn't grow
-- with time (docs/compat-notes.md). So after collecting, the shim runs one
-- call chain deeper than any callback, so the baseline the suite takes
-- next already has the stack it needs, as the mock's does.
local WARM = "local function f(n) local a, b, c, d, e, g, h, i, j, k = 1 " ..
  "if n > 0 then return 1 + f(n - 1) end return 0 end f(24)"
local hostgc, hostseed = collectgarbage, math.randomseed
function collectgarbage(opt, ...)
  opt = opt or "collect"
  if opt == "count" then return rt.gc("count") end
  if opt == "collect" or opt == "stop" or opt == "restart" then rt.gc(opt) end
  if opt == "collect" and rt.loaded() then rt.eval(WARM) end
  return hostgc(opt, ...)
end
function math.randomseed(...)
  local args = table.pack(...)
  if rt.loaded() then
    local list = {}
    for i = 1, args.n do list[i] = ("%d"):format(args[i]) end
    rt.eval("math.randomseed(" .. table.concat(list, ",") .. ")")
  end
  return hostseed(...)
end

-- --- M.store: the var slots ---

local storeproxy = setmetatable({}, {
  __index = function(_, k) return rt.vars()[k] end,
  __newindex = function(_, k, v) rt.var_put(k, v) end,
  __pairs = function() return next, rt.vars(), nil end,
})

-- --- M.sent: mirrored incrementally (suites index it in loops) ---

local sent, nsent = {}, 0
local function syncsent()
  local n = rt.sent_count()
  if n < nsent then sent, nsent = {}, 0 end
  if n > nsent then
    for _, m in ipairs(rt.sent(nsent + 1)) do sent[#sent + 1] = m end
    nsent = n
  end
  return sent
end

local FIELDS = {
  now = function() return rt.now() end,
  rate = function() return rt.rate() end,
  sent = syncsent,
  rings = function() return rt.rings() end,
  labels = function() return rt.labels() end,
  title = function() return rt.title() end,
  store = function() return storeproxy end,
  page = function() return rt.page() end,
  listening = function() return (rt.clock_info()) end,
  res = function() return select(2, rt.clock_info()) end,
  bpm = function() return select(3, rt.clock_info()) end,
  tp = function() return select(4, rt.clock_info()) end,
  cerrs = function() return select(5, rt.clock_info()) end,
  hold = function() return select(6, rt.clock_info()) end,
}

setmetatable(M, {
  __index = function(_, k)
    local f = FIELDS[k]
    if f then return f() end
  end,
  __newindex = function(t, k, v)
    if k == "sent" then
      assert(type(v) == "table" and next(v) == nil, "M.sent can only be cleared")
      rt.clear_sent()
      sent, nsent = {}, 0
    elseif k == "page" then
      rt.set_page(v)
    elseif k == "store" then
      rt.vars_clear()
      for name, x in pairs(v) do rt.var_put(name, x) end
    elseif FIELDS[k] then
      error("M." .. k .. " is read-only here")
    else
      rawset(t, k, v)
    end
  end,
})

-- --- loading ---

function M.load(path)
  local f = assert(io.open(path, "rb"))
  local src = f:read("a")
  f:close()
  local name = path:match("([^/]+)%.lua$") or path
  scene = SCENES[name] or {pad = 1, settings = 2}
  local assigned = {}
  for line in src:gmatch("[^\n]+") do
    local id = line:match("^%-%-@assign%s+.-id=(%d+)")
    if id then assigned[#assigned + 1] = tonumber(id) end
  end
  rt.scene{pages = 12, pad_pages = scene.pad, settings_page = scene.settings, assigned = assigned}
  -- The mock has one var slot whatever loads next (the suites seed it
  -- before loading); the runtime keeps vars per scene name.
  rt.var_bind(name)
  if not rt.load(path) then error("load failed: " .. rt.log(), 2) end
end

-- --- events ---

-- The mock derives an event's page from its id (where()): turn ids 1-16 and
-- push ids 17-32 are page 1, 33-48 / 49-64 page 2. On the scene they come
-- from a pad page (pages 1..pad, telling them apart by page) or the
-- settings page. If the current page produces the id, the event goes
-- through the runtime's wiring; otherwise (the suites sometimes send an
-- event from a page that isn't shown, which no device can) it's delivered
-- to the script's callback the way the mock does, with the mock's fields.
local function where(id, base)
  local pg = id > base + 16 and 2 or 1
  return (id - base - 1) % 16 + 1, pg
end

local function onpage(id, push)
  local cur = rt.page()
  local enc = (id - 1) % 16 + 1
  if id <= 32 then
    if (push and id <= 16) or (not push and id > 16) then return nil end
    if cur >= 1 and cur <= scene.pad then return enc end
  elseif (push and id > 48) or (not push and id <= 48) then
    if cur == scene.settings then return enc end
  end
  return nil
end

local pressed = {}       -- push id -> {enc = n} (runtime) or {direct = true}

function M.turn(id, inc)
  local enc = onpage(id, false)
  if enc then return rt.turn(enc, inc) end
  local index, pg = where(id > 32 and id - 16 or id, 0)
  controller.onEncoderTurn{id = id, index = index, page = pg, increment = inc,
    value = 8192, scaled = 64, is_held = false}
end

function M.press(id)
  local enc = onpage(id, true)
  if enc then
    pressed[id] = {enc = enc, at = rt.now()}
    return rt.press(enc)
  end
  pressed[id] = {direct = true}
  local index, pg = where(id > 48 and id - 32 or id - 16, 0)
  controller.onEncoderPress{id = id, index = index, page = pg, value = 8192, scaled = 64}
end

-- The mock's release says how long the push lasted (held, default 100 ms)
-- without time passing; the runtime measures it. If the suite claims a
-- longer push than has elapsed, time is advanced to make it so.
function M.release(id, held)
  held = held or 100
  local p = pressed[id]
  pressed[id] = nil
  if p and p.enc then
    local elapsed = rt.now() - p.at
    if held > elapsed then rt.advance(held - elapsed) end
    return rt.release(p.enc)
  end
  local index, pg = where(id > 48 and id - 32 or id - 16, 0)
  local hold = M.hold
  local cb = controller
  if hold > 0 and held >= hold and cb.onEncoderHold then
    cb.onEncoderHold{id = id, index = index, page = pg, value = 8192, scaled = 64}
  end
  if cb.onEncoderRelease then
    cb.onEncoderRelease{id = id, index = index, page = pg, value = 8192, scaled = 64, held_ms = held}
  end
end

function M.show(pg) rt.show(pg) end
function M.run(ms) rt.advance(ms) end
function M.extStart(bpm) rt.ext_start(bpm) end
function M.extStop() rt.ext_stop() end
function M.extContinue() rt.ext_continue() end

-- Messages sent so far, filtered: M.msgs(0xB0, 74) = CCs 74 on channel 1
function M.msgs(status, d1)
  local r = {}
  for _, m in ipairs(syncsent()) do
    if (not status or m.st == status) and (not d1 or m.d1 == d1) then r[#r + 1] = m end
  end
  return r
end

return M
