cmake_minimum_required(VERSION 4.1)
project(uranium)

set(CMAKE_CXX_STANDARD 20)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C ON CACHE BOOL "" FORCE)

add_executable(uranium main.cpp
        main.cpp
        src/decompiler/ir.cpp
        src/decompiler/cfg.cpp
        src/decompiler/ssa.cpp
        src/decompiler/ast.cpp
        src/decompiler/decompiler.cpp
        src/disassembler/disassembler.cpp
        src/decompiler/ir.h
)

add_subdirectory(extern/luau)

target_link_libraries(uranium
        Luau.Common
        Luau.CLI.lib
        Luau.Ast
        Luau.Compiler
        Luau.Config
        Luau.CodeGen
        Luau.VM
        Luau.VM.Internals
        Luau.Require
        Luau.Bytecode
)
