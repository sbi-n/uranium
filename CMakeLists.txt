cmake_minimum_required(VERSION 4.1)
project(uranium LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Luau is pinned to a release tag. To upgrade, change the tag and the archive hash:
#   curl -sL https://github.com/luau-lang/luau/archive/refs/tags/<tag>.tar.gz | shasum -a 256
# Set FETCHCONTENT_SOURCE_DIR_LUAU=/path/to/luau to build against a local checkout.
set(URANIUM_LUAU_TAG 0.748)
set(URANIUM_LUAU_SHA256 810b13d1480a7a2efa66fe356d7e551d2ba3548823a69a4df5a6597f2beb35e5)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(luau
    URL https://github.com/luau-lang/luau/archive/refs/tags/${URANIUM_LUAU_TAG}.tar.gz
    URL_HASH SHA256=${URANIUM_LUAU_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_MakeAvailable(luau)

# Release archives bundle the Luau license next to the binary.
configure_file(${luau_SOURCE_DIR}/LICENSE.txt ${CMAKE_BINARY_DIR}/LUAU-LICENSE.txt COPYONLY)

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

add_executable(uranium main.cpp)
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
