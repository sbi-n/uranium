#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

#include "Luau/Compiler.h"
#include "src/decompiler/decompiler.h"
#include "src/decompiler/ir.h"

int main(int argc, char** argv)
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

    try
    {
        bool sourceInput = false, irOutput = false;
        std::string path;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg = argv[i];
            if (arg == "--source") sourceInput = true;
            else if (arg == "--ir") irOutput = true;
            else if (arg == "--help" || arg == "-h")
            {
                std::cout << "Usage: uranium [--source] [--ir] <file|->\n"
                    "  --source  Compile Luau source before decompiling\n"
                    "  --ir      Print lifted IR instead of source\n"
                    "  -         Read standard input\n"
                    "With no arguments, decompile the built-in example.\n";
                return 0;
            }
            else if (!path.empty() || (arg.starts_with('-') && arg != "-")) throw std::runtime_error("invalid arguments; use --help");
            else path = arg;
        }
        std::string bytecode;
        if (argc == 1) bytecode = Luau::compile(source);
        else
        {
            if (path.empty()) throw std::runtime_error("missing input file; use --help");
            std::string input;
            if (path == "-") input.assign(std::istreambuf_iterator<char>(std::cin), {});
            else
            {
                std::ifstream file(path, std::ios::binary);
                if (!file) throw std::runtime_error("could not open " + path);
                input.assign(std::istreambuf_iterator<char>(file), {});
                if (file.bad()) throw std::runtime_error("could not read " + path);
            }
            bytecode = sourceInput ? Luau::compile(input) : std::move(input);
        }
        std::cout << (irOutput ? dump(lift(bytecode)) : decompile(bytecode));
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
