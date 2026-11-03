# Lua 5.4 core and the four libraries the stock firmware opens (base, math, string, table).
#
# 32-bit integers and floats, like the stock firmware. luaconf.h defines
# LUA_32BITS as 0 unconditionally, and Lua includes it with quotes (so a copy
# elsewhere on the include path loses to the original). Build from copies of
# the sources with luaconf.h edited, so the submodule stays untouched.
set(LUA_SRC ${CMAKE_CURRENT_LIST_DIR}/../external/lua)
set(LUA_DIR ${CMAKE_BINARY_DIR}/lua32)
file(GLOB lua_files CONFIGURE_DEPENDS ${LUA_SRC}/*.c ${LUA_SRC}/*.h)
file(COPY ${lua_files} DESTINATION ${LUA_DIR})
file(READ ${LUA_DIR}/luaconf.h luaconf)
string(REPLACE "#define LUA_32BITS\t0" "#define LUA_32BITS\t1" luaconf "${luaconf}")
if(NOT luaconf MATCHES "#define LUA_32BITS\t1")
    message(FATAL_ERROR "Couldn't set LUA_32BITS in luaconf.h; check the Lua version")
endif()
file(WRITE ${LUA_DIR}/luaconf.h "${luaconf}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LUA_SRC}/luaconf.h)

set(LUA_CORE lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
    lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib lmathlib lstrlib ltablib)
if(E16_HOST)
    # The host interpreter (lua.c) opens every library and the test suites use os.exit.
    # Scripts under test still get only base/math/string/table: rt.c opens those explicitly.
    list(APPEND LUA_CORE loslib liolib lcorolib ldblib lutf8lib loadlib linit)
endif()
list(TRANSFORM LUA_CORE PREPEND ${LUA_DIR}/)
list(TRANSFORM LUA_CORE APPEND .c)
add_library(lua STATIC ${LUA_CORE})
target_include_directories(lua PUBLIC ${LUA_DIR})
