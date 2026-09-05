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

int main() {
    const char *source = R"(
    print("HELLO")

    return 1, 2, 3
    )";

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    luaL_sandbox(L);

    lua_State *T = lua_newthread(L);
    luaL_sandboxthread(T);

    size_t size;
    char *bytecode = luau_compile(source, strlen(source), nullptr, &size);
    int result = luau_load(T, "=stdin", bytecode, size, 0);

    if (result != 0) {
        std::cerr << lua_tostring(T, -1) << std::endl;
        return 0;
    };

    TValue *value = T->top - 1;
    Closure *closure = clvalue(value);

    if (closure->isC) {
        std::cerr << "Unexpected behavior ( tried to disassemble c closure. )" << std::endl;
        return 0;
    }

    Proto *proto = closure->l.p;

    std::cout << "debugname: " << (proto->debugname ? getstr(proto->debugname) : "??") << std::endl;
    std::cout << "sizecode: " << proto->sizecode << '\n';

    for (int pc = 0; pc < proto->sizecode;) {
        Instruction insn = proto->code[pc];
        uint8_t op = LUAU_INSN_OP(insn);
        auto opcode = static_cast<LuauOpcode>(op);

        // Luau::Bytecode::getLuauOpcodeName(opcode)
        std::cout
                << luaG_getline(proto, pc)
                << ": "
                << Luau::Bytecode::getLuauOpcodeName(opcode)
                << '\n';

        pc += Luau::getOpLength(opcode);
    }

    lua_close(L);
}
