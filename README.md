<div align="center">
<img src="assets/logo.svg" alt="fontlib" width="96">
</div>

# Uranium

a Luau bytecode disassembler/decompiler

## Quick Start

Download releases

- Luau compiled ( using wasm and spider )
- CLI
- Archive

## Build

Setup

```bash
brew install cmake ninja
```

Build

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target uranium ir_tests decompiler_tests -j 4
./build/uranium
ctest --test-dir build --output-on-failure
```

## IR

The C++ lifter accepts bytecode from the bundled Luau compiler and returns an owning value:

```cpp
#include "ir.h"
#include "Luau/Compiler.h"

IRContext ir = lift(Luau::compile("return 1 + input"));
std::string text = dump(ir);
```

Function `f0` is the entry point. Function references index `IRContext::functions`; block,
table, and class references index the current function's vectors. PCs count bytecode words,
and block ranges are `[startpc, endpc)`. Every block ends with an explicit terminator.
Load and arithmetic variants share IR operations; constants retain their Luau types.

| Operation | Operands, in order |
| --- | --- |
| `MOVE`, unary, binary | destination, source operand(s) |
| `GETGLOBAL`, `GETUPVAL`, `GETIMPORT` | destination, name/upvalue/import path components |
| `SETGLOBAL`, `SETUPVAL` | name/upvalue, value |
| `GETTABLE` / `SETTABLE` | destination, table, key / table, key, value |
| `NEWTABLE`, `DUPTABLE`, `SETLIST` | destination, hash capacity, array capacity / destination, template / table, first index, values |
| `CLOSURE`, `DUPCLOSURE` | destination, function, captures in upvalue order |
| `NAMECALL` | destination, receiver, method name; also copies receiver to destination + 1 |
| `CALL` | result range, function register, argument range |
| `VARARGS`, `RETURN` | result/return range |
| `CLOSEUPVALS` | first register whose captures must be closed |
| `JUMP`, `BRANCH` | target / condition, operand(s), taken target, fallthrough target |
| `FORNPREP`, `FORNLOOP` | loop base register, taken target, fallthrough target |
| `FORGPREP`, `FORGLOOP` | loop base, target / loop base, variable count, taken target, fallthrough target |
| `NEWCLASS`, `NEWCLASSMEMBER` | destination, shape, superclass or nil, open flag / class, name, value |

Register ranges use `count = -1` for the dynamic stack top and `0` for no values.
Captures distinguish values, register references, and inherited upvalues. Table templates
and class shapes are copied before the temporary VM closes. `IRImmediate` (`#` in dumps)
represents metadata, including the VM function ID for `PROTO_MISMATCH`.

Fast-call hints use their ordinary fallback instructions; coverage counters, NOPs, and
vararg stack setup are omitted. Runtime-only instructions and invalid instruction boundaries
raise `std::runtime_error`. Input must be compiler-produced bytecode: the Luau loader is
not a validator for arbitrary binary data.

## Decompiler

```cpp
#include "decompiler.h"

std::string source = decompile(bytecode);
```

Link against `uranium_decompiler`. The API returns source rather than writing to stdout;
loading and reconstruction failures throw `std::runtime_error`.

```bash
./build/uranium compiled.bytecode
./build/uranium --source example.luau
./build/uranium --source --ir example.luau
./build/uranium --source - < example.luau
```

The stages are independently available through `cfg.h`, `ssa.h`, and `ast.h`:

```cpp
auto cfg = buildCFG(lift(bytecode));
auto ssa = buildSSA(std::move(cfg));
auto ast = buildAST(ssa);
std::string source = printAST(ast);
```

- CFG: reachable edges, dominators, postdominators, dominance frontiers, and natural loops.
- SSA: register liveness, pruned phi placement, dominator-tree renaming, and explicit
  definitions/uses, including call result ranges and loop control registers.
- AST: lexical declarations, expressions, conditions, `if`/`elseif`, `while`, `repeat`,
  numeric/generic `for`, `break`/`continue`, tables, methods, closures, and multiple returns.
  Reference captures remain shared; value captures remain snapshots. Experimental class
  instructions reconstruct class declarations and require the corresponding bundled Luau flags.

Generated locals and parameters use `v_0`, `v_1`, ...; captured bindings use `uv_0`,
`uv_1`, ...; local functions use `f_0`, `f_1`, .... Each counter is independent and starts
at zero for the output chunk. Numbers follow **printed declaration order**, including
nested function bodies, rather than registers or prototype IDs. Captured functions retain
their `f_` binding, including recursive self references. Globals, property/method names,
and runtime class names retain their semantic names.

For example:

```lua
local uv_0 = 1
local function f_0(v_0)
    uv_0 = uv_0 + v_0
    return uv_0
end
return f_0(2)
```

The decompiler recovers source structure from compiler-produced bytecode. Comments,
type annotations, original local names, and code removed or transformed by compiler
optimizations cannot be recovered exactly. Unrepresentable control flow is reported as
an error instead of emitting incomplete source. Tests recompile and execute recovered
chunks, compare results with the originals, and cover optimization levels 0–2 with and
without debug information.

## Abstract

IR -> CFG -> SSA -> AST

Logo author: commons:User:Pumbaa (original work by commons:User:Greg Robson) - http://commons.wikimedia.org/wiki/Category:Electron_shell_diagrams (corresponding labeled version), CC BY-SA 2.0 uk, https://commons.wikimedia.org/w/index.php?curid=22302376
