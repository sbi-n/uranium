#include "decompiler.h"
#include "ir.h"

void decompile(char *bytecode, int size)
{
    IRContext *ir = lift(bytecode, size);
}