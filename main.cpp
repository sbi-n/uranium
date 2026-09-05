#include <iostream>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"
#include "extern/luau/VM/src/ldebug.h"
#include "extern/luau/VM/src/lstate.h"
#include "Luau/Compiler.h"
#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"
#include "Luau/BytecodeDump.h"
#include "src/decompiler/ir.h"

int main()
{
    const char *source = R"(
    print("HELLO")

    local function hello()
        local function world()
            print("World")
        end

        local function shit()
        end

        return world
    end

    local function greet()
        print("Nice to meet you")
    end

    return 1, 2, 3
    )";

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    luaL_sandbox(L);

    lua_State *T = lua_newthread(L);
    luaL_sandboxthread(T);

    size_t size;
    char *bytecode = luau_compile(source, strlen(source), nullptr, &size);

    lift(bytecode, size);

    lua_close(L);
}
