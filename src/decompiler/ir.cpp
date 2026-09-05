#include <iostream>
#include <vector>

#include "Luau/Compiler.h"
#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"
#include "Luau/BytecodeDump.h"
#include "ldebug.h"
#include "lstate.h"
#include "lua.h"
#include "lualib.h"
#include "luacode.h"

int lift(char *bytecode, int size)
{

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    luaL_sandbox(L);

    lua_State *T = lua_newthread(L);
    luaL_sandboxthread(T);

    int result = luau_load(T, "=stdin", bytecode, size, 0);

    if (result != 0)
    {
        std::cerr << lua_tostring(T, -1) << std::endl;
        return 0;
    };

    TValue *value = T->top - 1;
    Closure *closure = clvalue(value);

    if (closure->isC)
    {
        std::cerr << "Unexpected behavior ( tried to disassemble c closure. )" << std::endl;
        return 0;
    }

    Proto *proto = closure->l.p;

    // 프로토 깊이우선탐색(아마도)
    std::vector<Proto *> protos;
    std::vector<Proto *> searchingProtos;

    searchingProtos.push_back(proto);
    while (!searchingProtos.empty())
    {
        Proto *currentProto = searchingProtos.back();
        searchingProtos.pop_back();

        protos.push_back(currentProto);

        for (int i = 0; i < currentProto->sizep; i++)
        {
            searchingProtos.push_back(currentProto->p[i]);
        }
    }

    int steps = 0;
    while (!protos.empty())
    {
        Proto *p = protos.back();
        protos.pop_back();

        std::cout << "Function " << steps
                  << " (" << (p->debugname ? getstr(p->debugname) : "??") << "):"
                  << "\n";

        steps++;

        for (int pc = 0; pc < p->sizecode;)
        {
            Instruction insn = p->code[pc];
            uint8_t op = LUAU_INSN_OP(insn);
            auto opcode = static_cast<LuauOpcode>(op);

            // Luau::Bytecode::getLuauOpcodeName(opcode)
            std::cout
                << luaG_getline(p, pc)
                << ": "
                << Luau::Bytecode::getLuauOpcodeName(opcode)
                << '\n';

            pc += Luau::getOpLength(opcode);
        }
    }

    lua_close(L);
}
