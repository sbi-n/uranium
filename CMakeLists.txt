cmake_minimum_required(VERSION 4.1)
project(uranium)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C ON CACHE BOOL "" FORCE)

add_subdirectory(extern/luau EXCLUDE_FROM_ALL)

add_library(uranium_ir src/decompiler/ir.cpp)
target_include_directories(uranium_ir PUBLIC src/decompiler)
target_link_libraries(uranium_ir PRIVATE Luau.Common Luau.VM Luau.VM.Internals)

add_library(uranium_decompiler
        src/decompiler/cfg.cpp
        src/decompiler/ssa.cpp
        src/decompiler/ast.cpp
        src/decompiler/decompiler.cpp
)
target_link_libraries(uranium_decompiler PUBLIC uranium_ir)

add_executable(uranium main.cpp src/disassembler/disassembler.cpp)
target_link_libraries(uranium PRIVATE uranium_decompiler Luau.Compiler)

include(CTest)
if(BUILD_TESTING)
    add_executable(ir_tests tests/ir.cpp)
    target_link_libraries(ir_tests PRIVATE uranium_ir Luau.Compiler Luau.Bytecode)
    add_test(NAME ir COMMAND ir_tests)
    add_executable(decompiler_tests tests/decompiler.cpp)
    target_link_libraries(decompiler_tests PRIVATE uranium_decompiler Luau.Compiler Luau.VM)
    add_test(NAME decompiler COMMAND decompiler_tests)
endif()
