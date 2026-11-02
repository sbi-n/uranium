#include "ir.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Compiler.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<const IRInstruction*> instructions(const IRContext& ir, IROp op)
{
    std::vector<const IRInstruction*> result;
    for (const auto& function : ir.functions)
        for (const auto& block : function.blocks)
            for (const auto& insn : block.instructions)
                if (insn.op == op) result.push_back(&insn);
    return result;
}

void verify(const IRContext& ir)
{
    check(!ir.functions.empty(), "missing entry function");
    for (size_t f = 0; f < ir.functions.size(); ++f)
    {
        const auto& function = ir.functions[f];
        check(function.id == f && !function.blocks.empty(), "invalid function identity");
        for (size_t b = 0; b < function.blocks.size(); ++b)
        {
            const auto& block = function.blocks[b];
            check(block.id == b && block.startpc < block.endpc, "invalid block range");
            check(block.startpc == (b ? function.blocks[b - 1].endpc : 0), "gap between blocks");
            check(!block.instructions.empty(), "block has no terminator");
            for (size_t i = 0; i < block.instructions.size(); ++i)
            {
                const auto& insn = block.instructions[i];
                check(insn.pc >= block.startpc && insn.pc < block.endpc, "instruction outside its block");
                unsigned edges = 0;
                for (const auto& operand : insn.operands)
                {
                    if (auto ref = std::get_if<IRBlockRef>(&operand))
                    {
                        check(ref->ref < function.blocks.size(), "dangling block reference");
                        ++edges;
                    }
                    if (auto ref = std::get_if<IRFunctionRef>(&operand)) check(ref->ref < ir.functions.size(), "dangling function reference");
                    if (auto ref = std::get_if<IRTableRef>(&operand)) check(ref->ref < function.tables.size(), "dangling table reference");
                    if (auto ref = std::get_if<IRClassRef>(&operand)) check(ref->ref < function.classes.size(), "dangling class reference");
                }
                bool terminal = edges != 0 || insn.op == IROp::RETURN;
                check(terminal == (i + 1 == block.instructions.size()), "misplaced or missing terminator");
                if (insn.op == IROp::BRANCH || insn.op == IROp::FORNPREP || insn.op == IROp::FORNLOOP || insn.op == IROp::FORGLOOP)
                    check(edges == 2, "conditional terminator needs both successors");
                if (insn.op == IROp::JUMP || insn.op == IROp::FORGPREP) check(edges == 1, "unconditional terminator needs one successor");
                if (insn.op == IROp::CLOSURE || insn.op == IROp::DUPCLOSURE)
                {
                    auto child = std::get<IRFunctionRef>(insn.operands[1]);
                    check(insn.operands.size() == 2u + ir.functions[child.ref].upvalueCount, "capture count mismatch");
                    for (size_t j = 2; j < insn.operands.size(); ++j)
                        check(std::holds_alternative<IRCapture>(insn.operands[j]), "capture lost its kind");
                }
            }
        }
    }
}

IRContext compile(const std::string& source, int optimization = 1, int debug = 2)
{
    Luau::CompileOptions options;
    options.optimizationLevel = optimization;
    options.debugLevel = debug;
    options.coverageLevel = 2;
    IRContext ir = lift(Luau::compile(source, options));
    verify(ir);
    return ir;
}

std::string finish(Luau::BytecodeBuilder& builder, uint32_t main, uint8_t stack)
{
    builder.endFunction(stack, 0);
    builder.setMainFunction(main);
    builder.finalize();
    return builder.getBytecode();
}

void sourceCases()
{
    const char* cases[] = {
        "return",
        R"(return function(a, b)
            return a+b, a-b, a*b, a/b, a//b, a%b, a^b, a+2, a-2, a*2, a/2,
                a//2, a%2, a^2, 2-a, 2/a, a and b, a or b, a and 2, a or 2,
                not a, -a, #b, a .. b .. 'end'
        end)",
        R"(return function(a, b)
            if a == b then a = 1 elseif a ~= b then a = 2 end
            if a < b or a <= b or a > b or a >= b then b = 3 end
            if a == nil or a ~= true or a == 42 or a == 'value' then b = 4 end
            while a < b do a += 1; if a == 9 then break end; continue end
            repeat b -= 1 until b <= a
            return a, b, a == b
        end)",
        R"(return function(t, n, step)
            local sum = 0
            for i = 1, n, step do sum += i end
            for k, v in pairs(t) do sum += v end
            for i, v in ipairs(t) do sum += v end
            for k, v in t do sum += v end
            return sum
        end)",
        R"(return function(t, key, ...)
            local list = {1, 2, ...}
            local record = {hello = true, value = 3, name = 'record'}
            t[key], t.field, t[3] = list, record, 5
            global = t[key]
            return t:method(t.field, t[3], ...)
        end)",
        R"(return function(x, ...)
            local a, b, c = ...
            f()
            f(a, b)
            return f(x, ...)
        end)",
        R"(local mutable = 0
            local fixed = input
            local function outer(x)
                mutable += x
                return function() return mutable, fixed end
            end
            mutable += 1
            return outer
        )",
        R"(return math.abs(x), math.max(x, y), math.max(x, 1), math.clamp(x, y, z),
            math.sin(x), pcall(f, x), xpcall(f, handler, x), game.Workspace.Part)",
        "return 'a\\000b\\n\\\"\\\\', -32768, 1.25, true, nil",
    };
    for (int optimization = 0; optimization <= 2; ++optimization)
        for (int debug : {0, 2})
            for (const char* source : cases) compile(source, optimization, debug);

    const auto ir = compile(cases[3]);
    for (IROp op : {IROp::FORNPREP, IROp::FORNLOOP, IROp::FORGPREP, IROp::FORGLOOP})
        check(!instructions(ir, op).empty(), "loop operation missing");

    const auto closures = compile(cases[6]);
    bool reference = false, inherited = false, byValue = false;
    for (const auto& function : closures.functions)
        for (const auto& block : function.blocks)
            for (const auto& insn : block.instructions)
                for (const auto& operand : insn.operands)
                    if (auto capture = std::get_if<IRCapture>(&operand))
                    {
                        reference |= capture->kind == IRCapture::Kind::REFERENCE;
                        inherited |= capture->kind == IRCapture::Kind::UPVALUE;
                        byValue |= capture->kind == IRCapture::Kind::VALUE;
                    }
    check(reference && inherited && byValue, "missing a closure capture mode");
    check(!instructions(closures, IROp::CLOSEUPVALS).empty(), "upvalues were not closed");

    const auto packs = compile(cases[5]);
    bool open = false, empty = false;
    for (const auto* call : instructions(packs, IROp::CALL))
    {
        auto results = std::get<IRRegisterRange>(call->operands[0]);
        auto args = std::get<IRRegisterRange>(call->operands[2]);
        open |= results.count == -1 && args.count == -1;
        empty |= results.count == 0 && args.count == 0;
    }
    check(open && empty, "MULTRET must be distinct from zero arguments/results");
    check(dump(compile(cases[8])).find("a\\000b\\n\\\"\\\\") != std::string::npos, "binary string was truncated or escaped incorrectly");
    check(dump(compile(cases[4])) == dump(compile(cases[4])), "table dump depends on VM lifetime or hash order");
}

void constants()
{
    Luau::BytecodeBuilder builder;
    auto main = builder.beginFunction(0);
    int string = builder.addConstantString({"a\0b", 3});
    int vector = builder.addConstantVectorf(1, 2, 3, 0);
    int integer = builder.addConstantInteger(std::numeric_limits<int64_t>::min());
    builder.emitAD(LOP_LOADN, 0, -123);
    builder.emitABC(LOP_LOADKX, 1, 0, 0);
    builder.emitAux(string);
    builder.emitAD(LOP_LOADK, 2, vector);
    builder.emitAD(LOP_LOADK, 3, integer);
    builder.emitABC(LOP_RETURN, 0, 5, 0);
    auto ir = lift(finish(builder, main, 4));
    verify(ir);
    auto moves = instructions(ir, IROp::MOVE);
    check(std::get<IRConstantNumber>(moves[0]->operands[1]).value == -123, "LOADN lost its sign");
    check(std::get<IRConstantString>(moves[1]->operands[1]).value == std::string("a\0b", 3), "embedded NUL lost");
    check(std::get<IRConstantVector>(moves[2]->operands[1]).value[2] == 3, "vector lost a component");
    check(std::get<IRConstantInteger>(moves[3]->operands[1]).value == std::numeric_limits<int64_t>::min(), "integer lost precision");

    Luau::BytecodeBuilder tables;
    main = tables.beginFunction(0);
    Luau::BytecodeBuilder::TableShape shape{};
    shape.length = 2;
    shape.hasConstants = true;
    shape.keys[0] = tables.addConstantString({"answer", 6});
    shape.constants[0] = tables.addConstantNumber(42);
    shape.keys[1] = tables.addConstantString({"flag", 4});
    shape.constants[1] = tables.addConstantBoolean(false);
    int table = tables.addConstantTable(shape);
    tables.emitAD(LOP_DUPTABLE, 0, table);
    tables.emitABC(LOP_RETURN, 0, 2, 0);
    ir = lift(finish(tables, main, 1));
    verify(ir);
    check(dump(ir).find("[\"answer\"] = 42, [\"flag\"] = false") != std::string::npos, "table template values lost");
}

void controlFlow()
{
    Luau::BytecodeBuilder builder;
    auto main = builder.beginFunction(0);
    auto name = builder.addConstantString({"ignored", 7});
    builder.emitABC(LOP_LOADB, 0, 1, 2); // Skip the entire two-word GETGLOBAL.
    builder.emitABC(LOP_GETGLOBAL, 0, 0, 0);
    builder.emitAux(name);
    builder.emitABC(LOP_RETURN, 0, 2, 0);
    auto ir = lift(finish(builder, main, 1));
    verify(ir);
    const auto& blocks = ir.functions[0].blocks;
    check(blocks.size() == 3 && blocks[1].startpc == 1 && blocks[1].endpc == 3, "AUX counted as an instruction");
    check(std::get<IRBlockRef>(blocks[0].instructions.back().operands[0]).ref == 2, "LOADB skip lost");

    Luau::BytecodeBuilder comparisons;
    main = comparisons.beginFunction(2);
    comparisons.emitAD(LOP_JUMPIFNOTLE, 0, 2);
    comparisons.emitAux(1);
    comparisons.emitABC(LOP_RETURN, 0, 2, 0);
    comparisons.emitABC(LOP_RETURN, 1, 2, 0);
    ir = lift(finish(comparisons, main, 2));
    verify(ir);
    auto branch = instructions(ir, IROp::BRANCH).front();
    check(std::get<IRCondition>(branch->operands[0]) == IRCondition::NOT_LE, "negated comparison changed NaN semantics");
    check(std::get<IRBlockRef>(branch->operands[3]).ref == 2 && std::get<IRBlockRef>(branch->operands[4]).ref == 1,
        "taken and fallthrough edges swapped");

    Luau::BytecodeBuilder large;
    main = large.beginFunction(0);
    large.emitE(LOP_JUMPX, 70000);
    for (int i = 0; i < 70000; ++i) large.emitABC(LOP_LOADNIL, 0, 0, 0);
    large.emitABC(LOP_RETURN, 0, 1, 0);
    ir = lift(finish(large, main, 1));
    verify(ir);
    check(ir.functions[0].blocks.back().endpc == 70002, "PC truncated to 16 bits");
}

void errors()
{
    auto rejects = [](std::string_view bytecode, std::string_view expected) {
        try { lift(bytecode); }
        catch (const std::runtime_error& error)
        {
            check(std::string_view(error.what()).find(expected) != std::string_view::npos, "unexpected error message");
            return;
        }
        throw std::runtime_error("bad bytecode accepted");
    };
    rejects("", "empty bytecode");
    rejects(Luau::compile("local ="), "Expected");
    rejects(std::string(1, char(255)), "version");

    struct Patch : Luau::BytecodeEncoder
    {
        std::function<void(uint32_t*, size_t)> apply;
        void encode(uint32_t* code, size_t count) override { apply(code, count); }
    } patch;
    auto patched = [&](std::function<void(uint32_t*, size_t)> mutate) {
        patch.apply = std::move(mutate);
        Luau::BytecodeBuilder builder(&patch);
        auto main = builder.beginFunction(0);
        int name = builder.addConstantString({"x", 1});
        builder.emitAD(LOP_JUMP, 0, 2);
        builder.emitABC(LOP_GETGLOBAL, 0, 0, 0);
        builder.emitAux(name);
        builder.emitABC(LOP_RETURN, 0, 1, 0);
        return finish(builder, main, 1);
    };
    rejects(patched([](auto code, auto) { code[0] = LOP_JUMP | (1u << 16); }), "AUX or CAPTURE");
    rejects(patched([](auto code, auto) { code[0] = LOP_JUMP | (0xfffeu << 16); }), "out of range");
    rejects(patched([](auto code, auto) { code[0] = 255; }), "unknown opcode");
    rejects(patched([](auto code, auto count) { code[count - 1] = LOP_LOADKX; }), "missing AUX");
    rejects(patched([](auto code, auto) { code[0] = LOP_CAPTURE; }), "standalone CAPTURE");
}
} // namespace

int main()
{
    Luau::assertHandler() = [](const char* expression, const char* file, int line, const char*) {
        std::cerr << file << ':' << line << ": assertion failed: " << expression << '\n';
        return 1;
    };
    try
    {
        sourceCases();
        constants();
        controlFlow();
        errors();
        std::cout << "IR tests passed (54 compiler cases, constants, control flow, errors)\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
