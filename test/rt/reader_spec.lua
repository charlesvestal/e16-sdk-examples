-- test/rt/reader_spec.lua: rt_load_reader(), the streaming load disk
-- scripts use, behaves exactly like rt_load() (rt.load_string) however
-- the source is cut into pieces.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

-- Everything a script's load leaves behind that a test can see.
local function snapshot(ok)
  local labels = {}
  for i = 1, 16 do labels[i] = rt.labels()[i] or "" end
  return {ok = ok, log = rt.log(), title = rt.title() or "", labels = table.concat(labels, "|"),
          page = rt.page()}
end

local function via_string(name, src)
  return snapshot(rt.load_string(name, src))
end

local function via_reader(name, src, chunk)
  local ok, pieces = rt.load_chunked(name, src, chunk)
  local snap = snapshot(ok)
  snap.pieces = pieces
  return snap
end

local function same(a, b)
  return a.ok == b.ok and a.log == b.log and a.title == b.title and a.labels == b.labels and a.page == b.page
end

-- Tokens that straddle 7-byte pieces: long strings and comments with
-- levels, escapes, hex floats, CRLF line ends, a multi-byte UTF-8 string.
local TRICKY = table.concat({
  "-- header comment\r\n",
  "--[==[ a long\r\ncomment ]] still in it ]==]\r\n",
  "local long = [[\nline one\nline two]]\r\n",
  "local esc = \"\\x41\\u{48}\\065\\z\n      tail\\n\"\r\n",
  "local num = 0x1p4 + 1e2 + 0xFF + 3.25\r\n",
  "local utf = \"caf\195\169\"\r\n",
  "function page.onInit()\r\n",
  "  page.setTitle(string.sub(esc, 1, 3) .. #long)\r\n",
  "  slots.update(1, 'Ab'); slots.update(16, tostring(math.floor(num) % 1000))\r\n",
  "  print(long, esc, num, #utf)\r\n",
  "end\r\n",
})

local base = via_string("t", TRICKY)
S.check(base.ok, "the tricky script loads from a string")
S.check(base.title == "AHA" .. #"line one\nline two", "it ran (title '" .. base.title .. "')")
for _, chunk in ipairs({1, 2, 7, 64, 512, #TRICKY + 10}) do
  local r = via_reader("t", TRICKY, chunk)
  S.check(same(base, r), ("%d-byte pieces: same result as load_string"):format(chunk))
end
local r7 = via_reader("t", TRICKY, 7)
S.check(r7.pieces == math.ceil(#TRICKY / 7), "the reader handed out " .. r7.pieces .. " pieces of 7 bytes")

-- A syntax error: same message, same line number, same chunk name.
local BAD = "function page.onInit()\n  local x = 1\n  if x then\n    x = = 2\n  end\nend\n"
local bs = via_string("bad", BAD)
local br = via_reader("bad", BAD, 7)
S.check(not bs.ok and not br.ok, "a syntax error fails both loads")
S.check(bs.log == br.log and br.log:find("bad:4:", 1, true) ~= nil,
        "same syntax error, at line 4 (" .. br.log:gsub("\n.*", "") .. ")")

-- An error in page.onInit: same message and traceback.
local BOOM = "local function f()\n  error('boom')\nend\nfunction page.onInit()\n  f()\nend\n"
local es = via_string("boom", BOOM)
local er = via_reader("boom", BOOM, 7)
S.check(not es.ok and not er.ok and es.log == er.log and er.log:find("boom:2: boom", 1, true) ~= nil,
        "an onInit error reads the same, traceback included")

-- An error at the top level of the chunk.
local TOP = "local a = nil\nlocal b = a.x\nfunction page.onInit() end\n"
local ts, tr = via_string("top", TOP), via_reader("top", TOP, 7)
S.check(not ts.ok and same(ts, tr) and tr.log:find("top:2:", 1, true) ~= nil, "a top-level error reads the same")

-- Precompiled bytecode is refused either way ("t" mode).
local bin = string.dump(function() end)
local bs2, br2 = via_string("bin", bin), via_reader("bin", bin, 7)
S.check(not bs2.ok and not br2.ok and br2.log:find("binary", 1, true) ~= nil, "bytecode is refused by the reader too")

-- An empty source: an empty chunk, then no page.onInit -- same either way.
S.check(same(via_string("e", ""), via_reader("e", "", 7)), "an empty source behaves the same")

-- A reload through the reader is a clean reload: state from the previous
-- script doesn't survive.
rt.load_chunked("t", "X = 5\nfunction page.onInit() end\n", 7)
rt.load_chunked("t", "function page.onInit() end\n", 7)
S.check(rt.eval("return X") == nil, "a reader reload starts a fresh state")

-- The real scripts, the largest (tb3po.lua, ~16 KB) included: 7-byte
-- pieces give the same load as one buffer.
for _, f in ipairs({"euclid", "lfo", "chords", "modseq", "tb3po"}) do
  local h = assert(io.open("external/oxi-e16-lua/" .. f .. ".lua", "rb"))
  local src = h:read("a")
  h:close()
  local s = via_string(f, src)
  local r = via_reader(f, src, 7)
  S.check(s.ok and r.ok and same(s, r), ("%s.lua (%d bytes, %d pieces): same as load_string"):format(f, #src, r.pieces))
  if r.ok then
    rt.turn(1, 1)
    rt.advance(50)
  end
end

S.done()
