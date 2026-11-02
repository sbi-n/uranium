#include "decompiler.h"
#include "ast.h"

std::string decompile(std::string_view bytecode)
{
    return printAST(buildAST(buildSSA(buildCFG(lift(bytecode)))));
}
