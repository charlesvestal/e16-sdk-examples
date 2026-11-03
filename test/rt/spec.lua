-- test/rt/spec.lua: check/done helpers shared by the runtime specs.
local S = {fails = 0}
function S.check(ok, msg)
  if ok then print("ok   " .. msg) else S.fails = S.fails + 1; print("FAIL: " .. msg) end
end
function S.raises(f, msg) S.check(not pcall(f), msg) end
function S.done()
  print(S.fails == 0 and "\nALL PASSED" or ("\n" .. S.fails .. " FAILED"))
  os.exit(S.fails == 0 and 0 or 1)
end
return S
