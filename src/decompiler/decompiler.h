#pragma once

#include <string>
#include <string_view>

// Returns a standalone Luau chunk, with deterministic names in declaration order.
std::string decompile(std::string_view bytecode);
