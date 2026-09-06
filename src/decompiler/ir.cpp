#include "ir.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"
#include "lgc.h"
#include "lstate.h"
#include "lua.h"
#include "lualib.h"

namespace
{
std::string stringValue(const TString* value)
{
    return {getstr(value), value->len};
}

std::string quote(std::string_view value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value)
    {
        switch (c)
        {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 32 || c == 127)
                out << '\\' << std::setfill('0') << std::setw(3) << unsigned(c);
            else
                out << c;
        }
    }
    out << '"';
    return out.str();
}

const char* opName(IROp op)
{
    static constexpr const char* names[] = {
        "MOVE", "ADD", "SUB", "MUL", "DIV", "IDIV", "MOD", "POW", "AND", "OR",
        "CONCAT", "NOT", "MINUS", "LENGTH", "GETGLOBAL", "SETGLOBAL", "GETUPVAL", "SETUPVAL",
        "CLOSEUPVALS", "GETIMPORT", "GETTABLE", "SETTABLE", "NEWTABLE", "DUPTABLE", "SETLIST",
        "CLOSURE", "DUPCLOSURE", "NAMECALL", "CALL", "VARARGS", "JUMP", "BRANCH", "RETURN",
        "FORNPREP", "FORNLOOP", "FORGPREP", "FORGLOOP", "NEWCLASS", "NEWCLASSMEMBER",
    };
    static_assert(std::size(names) == size_t(IROp::NEWCLASSMEMBER) + 1);
    return names[size_t(op)];
}

std::string operandText(const IROperand& operand)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, IRRegister>) out << 'r' << unsigned(value.index);
        else if constexpr (std::is_same_v<T, IRUpvalue>) out << 'u' << unsigned(value.index);
        else if constexpr (std::is_same_v<T, IRBlockRef>) out << 'b' << value.ref;
        else if constexpr (std::is_same_v<T, IRFunctionRef>) out << 'f' << value.ref;
        else if constexpr (std::is_same_v<T, IRTableRef>) out << 't' << value.ref;
        else if constexpr (std::is_same_v<T, IRClassRef>) out << 'c' << value.ref;
        else if constexpr (std::is_same_v<T, IRImmediate>) out << '#' << value.value;
        else if constexpr (std::is_same_v<T, IRRegisterRange>)
        {
            if (value.count == 0) out << "[]";
            else if (value.count == -1) out << "[r" << value.start << "..top]";
            else out << "[r" << value.start << "..r" << value.start + value.count - 1 << ']';
        }
        else if constexpr (std::is_same_v<T, IRCondition>)
        {
            static constexpr const char* names[] = {"truthy", "falsy", "eq", "le", "lt", "not_eq", "not_le", "not_lt", "proto_mismatch"};
            out << names[size_t(value)];
        }
        else if constexpr (std::is_same_v<T, IRCapture>)
        {
            static constexpr const char* names[] = {"value r", "ref r", "upvalue u"};
            out << names[size_t(value.kind)] << unsigned(value.index);
        }
        else if constexpr (std::is_same_v<T, IRConstantNil>) out << "nil";
        else if constexpr (std::is_same_v<T, IRConstantBool>) out << (value.value ? "true" : "false");
        else if constexpr (std::is_same_v<T, IRConstantNumber>) out << value.value;
        else if constexpr (std::is_same_v<T, IRConstantInteger>) out << value.value << 'i';
        else if constexpr (std::is_same_v<T, IRConstantString>) out << quote(value.value);
        else if constexpr (std::is_same_v<T, IRConstantVector>)
        {
            out << "vector(";
            for (size_t i = 0; i < value.value.size(); ++i)
                out << (i ? ", " : "") << value.value[i];
            out << ')';
        }
    }, operand);
    return out.str();
}

struct Decoded
{
    uint32_t pc;
    uint32_t length;
    LuauOpcode op;
    Instruction word;
    uint32_t aux;
    const Proto* child;
};

using FunctionIds = std::unordered_map<const Proto*, uint32_t>;

struct FunctionLifter
{
    lua_State* state;
    const Proto* proto;
    const FunctionIds& ids;
    IRFunction function{};
    std::unordered_map<const LuaTable*, uint32_t> tableIds{};
    std::unordered_map<const LuauClass*, uint32_t> classIds{};

    [[noreturn]] void fail(uint32_t pc, const std::string& message) const
    {
        throw std::runtime_error("function f" + std::to_string(function.id) + " pc " + std::to_string(pc) + ": " + message);
    }

    const TValue& constant(int64_t index) const
    {
        if (index < 0 || index >= proto->sizek)
            throw std::runtime_error("constant index out of range");
        return proto->k[index];
    }

    IRRegister reg(unsigned index) const
    {
        if (index >= proto->maxstacksize)
            throw std::runtime_error("register index out of range");
        return {uint8_t(index)};
    }

    IRUpvalue upval(unsigned index) const
    {
        if (index >= proto->nups)
            throw std::runtime_error("upvalue index out of range");
        return {uint8_t(index)};
    }

    IRRegisterRange range(unsigned start, int count) const
    {
        if (count < -1 || start > proto->maxstacksize || (count > 0 && start + count > proto->maxstacksize))
            throw std::runtime_error("register range out of bounds");
        return {uint16_t(start), count};
    }

    IROperand value(const TValue& v)
    {
        switch (ttype(&v))
        {
        case LUA_TNIL: return IRConstantNil{};
        case LUA_TBOOLEAN: return IRConstantBool{bool(bvalue(&v))};
        case LUA_TNUMBER: return IRConstantNumber{nvalue(&v)};
        case LUA_TINTEGER: return IRConstantInteger{lvalue(&v)};
        case LUA_TSTRING: return IRConstantString{stringValue(tsvalue(&v))};
        case LUA_TVECTOR:
        {
            IRConstantVector result;
            for (int i = 0; i < LUA_VECTOR_SIZE; ++i)
                result.value.push_back(vvalue(&v)[i]);
            return result;
        }
        case LUA_TFUNCTION:
            if (clvalue(&v)->isC) break;
            return IRFunctionRef{ids.at(clvalue(&v)->l.p)};
        case LUA_TTABLE:
        {
            const LuaTable* table = hvalue(&v);
            auto [it, inserted] = tableIds.emplace(table, uint32_t(function.tables.size()));
            uint32_t id = it->second;
            if (!inserted) return IRTableRef{id};
            function.tables.emplace_back();
            IRTable result;
            for (int i = 0; i < table->sizearray; ++i)
                if (!ttisnil(&table->array[i]))
                    result.entries.emplace_back(IRConstantNumber{double(i + 1)}, value(table->array[i]));
            for (int i = 0; i < sizenode(table); ++i)
            {
                const LuaNode& node = table->node[i];
                if (ttisnil(&node.val)) continue;
                TValue key;
                getnodekey(state, &key, &node);
                IROperand k = value(key);
                result.entries.emplace_back(std::move(k), value(node.val));
            }
            std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
                return operandText(a.first) < operandText(b.first);
            });
            function.tables[id] = std::move(result);
            return IRTableRef{id};
        }
        case LUA_TCLASS:
        {
            const LuauClass* shape = classvalue(&v);
            auto [it, inserted] = classIds.emplace(shape, uint32_t(function.classes.size()));
            if (inserted)
            {
                IRClass result{stringValue(shape->name), {}, {}};
                for (uint32_t i = 0; i < shape->numberofallmembers; ++i)
                    (i < shape->numberofinstancemembers ? result.fields : result.methods).push_back(stringValue(shape->offsettomember[i]));
                function.classes.push_back(std::move(result));
            }
            return IRClassRef{it->second};
        }
        }
        throw std::runtime_error("unsupported constant type " + std::to_string(ttype(&v)));
    }

    std::vector<Decoded> decode() const
    {
        std::vector<Decoded> result;
        for (uint32_t pc = 0; pc < uint32_t(proto->sizecode);)
        {
            Instruction word = proto->code[pc];
            auto op = LuauOpcode(LUAU_INSN_OP(word));
            if (op >= LOP__COUNT) fail(pc, "unknown opcode");
            uint32_t length = Luau::getOpLength(op);
            if (length > uint32_t(proto->sizecode) - pc) fail(pc, "missing AUX word");
            uint32_t aux = length == 2 ? proto->code[pc + 1] : 0;
            const Proto* child = nullptr;
            if (op == LOP_NEWCLOSURE)
            {
                int index = LUAU_INSN_D(word);
                if (index < 0 || index >= proto->sizep) fail(pc, "child function index out of range");
                child = proto->p[index];
            }
            else if (op == LOP_DUPCLOSURE)
            {
                const TValue& v = constant(LUAU_INSN_D(word));
                if (!ttisfunction(&v) || clvalue(&v)->isC) fail(pc, "expected a Luau closure constant");
                child = clvalue(&v)->l.p;
            }
            if (child)
            {
                length += child->nups;
                if (length > uint32_t(proto->sizecode) - pc) fail(pc, "missing closure captures");
                for (uint32_t i = 1; i < length; ++i)
                {
                    Instruction capture = proto->code[pc + i];
                    unsigned kind = LUAU_INSN_A(capture);
                    if (LUAU_INSN_OP(capture) != LOP_CAPTURE || kind > LCT_UPVAL || (op == LOP_DUPCLOSURE && kind == LCT_REF))
                        fail(pc + i, "invalid closure capture");
                }
            }
            result.push_back({pc, length, op, word, aux, child});
            pc += length;
        }
        return result;
    }

    IRFunction run()
    {
        function.id = ids.at(proto);
        if (proto->debugname) function.debugname = stringValue(proto->debugname);
        function.maxstacksize = proto->maxstacksize;
        function.upvalueCount = proto->nups;
        function.isVararg = proto->is_vararg != 0;
        for (unsigned i = 0; i < proto->numparams; ++i)
        {
            IRParameter parameter{reg(i), std::nullopt};
            for (int j = 0; j < proto->sizelocvars; ++j)
            {
                const LocVar& local = proto->locvars[j];
                if (local.reg == i && local.startpc == 0)
                {
                    parameter.debugname = stringValue(local.varname);
                    break;
                }
            }
            function.parameters.push_back(std::move(parameter));
        }

        const auto code = decode();
        if (code.empty()) fail(0, "empty function");
        std::set<uint32_t> starts;
        std::set<uint32_t> leaders{0};
        auto hasTarget = [](const Decoded& insn) {
            return Luau::isJumpD(insn.op) || insn.op == LOP_JUMPX || (insn.op == LOP_LOADB && LUAU_INSN_C(insn.word));
        };
        for (const auto& insn : code)
        {
            starts.insert(insn.pc);
            if (hasTarget(insn))
            {
                int target = Luau::getJumpTarget(insn.word, insn.pc);
                if (target < 0 || target >= proto->sizecode) fail(insn.pc, "jump target out of range");
                leaders.insert(uint32_t(target));
            }
            if ((hasTarget(insn) || insn.op == LOP_RETURN) && insn.pc + insn.length < uint32_t(proto->sizecode))
                leaders.insert(insn.pc + insn.length);
        }
        std::unordered_map<uint32_t, uint32_t> blockIds;
        for (uint32_t pc : leaders)
        {
            if (!starts.count(pc)) fail(pc, "jump into AUX or CAPTURE data");
            uint32_t id = uint32_t(function.blocks.size());
            blockIds.emplace(pc, id);
            function.blocks.push_back({id, pc, uint32_t(proto->sizecode), {}});
            if (id) function.blocks[id - 1].endpc = pc;
        }
        auto blockRef = [&](uint32_t pc) -> IRBlockRef {
            auto it = blockIds.find(pc);
            if (it == blockIds.end()) fail(pc, "missing successor block");
            return {it->second};
        };

        size_t blockIndex = 0;
        for (const auto& insn : code)
        {
            while (insn.pc >= function.blocks[blockIndex].endpc) ++blockIndex;
            IRBlock& block = function.blocks[blockIndex];
            const auto [pc, length, op, word, aux, child] = insn;
            unsigned a = LUAU_INSN_A(word), b = LUAU_INSN_B(word), c = LUAU_INSN_C(word);
            int d = LUAU_INSN_D(word);
            uint32_t next = pc + length;
            bool terminated = false;
            auto emit = [&](IROp irOp, std::vector<IROperand> operands) -> IRInstruction& {
                block.instructions.push_back({irOp, pc, std::move(operands)});
                return block.instructions.back();
            };
            auto k = [&](int64_t index) { return value(constant(index)); };
            auto target = [&] { return blockRef(uint32_t(Luau::getJumpTarget(word, pc))); };
            auto branch = [&](IRCondition condition, std::vector<IROperand> operands) {
                operands.insert(operands.begin(), condition);
                operands.push_back(target());
                operands.push_back(blockRef(next));
                emit(IROp::BRANCH, std::move(operands));
                terminated = true;
            };
            static constexpr IROp arithmetic[] = {IROp::ADD, IROp::SUB, IROp::MUL, IROp::DIV, IROp::MOD, IROp::POW};
            switch (op)
            {
            // Fast paths are redundant with the following fallback bytecode, including CALL.
            case LOP_NOP: case LOP_COVERAGE: case LOP_PREPVARARGS:
            case LOP_FASTCALL: case LOP_FASTCALL1: case LOP_FASTCALL2:
            case LOP_FASTCALL2K: case LOP_FASTCALL3: case LOP_FASTPCALL:
                break;
            case LOP_LOADNIL: emit(IROp::MOVE, {reg(a), IRConstantNil{}}); break;
            case LOP_LOADB:
                emit(IROp::MOVE, {reg(a), IRConstantBool{b != 0}});
                if (c) { emit(IROp::JUMP, {target()}); terminated = true; }
                break;
            case LOP_LOADN: emit(IROp::MOVE, {reg(a), IRConstantNumber{double(d)}}); break;
            case LOP_LOADK: emit(IROp::MOVE, {reg(a), k(d)}); break;
            case LOP_LOADKX: emit(IROp::MOVE, {reg(a), k(aux)}); break;
            case LOP_MOVE: emit(IROp::MOVE, {reg(a), reg(b)}); break;
            case LOP_GETGLOBAL: emit(IROp::GETGLOBAL, {reg(a), k(aux)}); break;
            case LOP_SETGLOBAL: emit(IROp::SETGLOBAL, {k(aux), reg(a)}); break;
            case LOP_GETUPVAL: emit(IROp::GETUPVAL, {reg(a), upval(b)}); break;
            case LOP_SETUPVAL: emit(IROp::SETUPVAL, {upval(b), reg(a)}); break;
            case LOP_CLOSEUPVALS: emit(IROp::CLOSEUPVALS, {reg(a)}); break;
            case LOP_GETIMPORT:
            {
                unsigned count = aux >> 30;
                if (!count) fail(pc, "empty import path");
                auto& imported = emit(IROp::GETIMPORT, {reg(a)});
                for (unsigned i = 0; i < count; ++i)
                    imported.operands.push_back(k((aux >> (20 - 10 * i)) & 1023));
                break;
            }
            case LOP_GETTABLE: emit(IROp::GETTABLE, {reg(a), reg(b), reg(c)}); break;
            case LOP_SETTABLE: emit(IROp::SETTABLE, {reg(b), reg(c), reg(a)}); break;
            case LOP_GETTABLEKS: case LOP_GETUDATAKS:
                emit(IROp::GETTABLE, {reg(a), reg(b), k(op == LOP_GETUDATAKS ? aux & 0xffff : aux)}); break;
            case LOP_SETTABLEKS: case LOP_SETUDATAKS:
                emit(IROp::SETTABLE, {reg(b), k(op == LOP_SETUDATAKS ? aux & 0xffff : aux), reg(a)}); break;
            case LOP_GETTABLEN: emit(IROp::GETTABLE, {reg(a), reg(b), IRConstantNumber{double(c + 1)}}); break;
            case LOP_SETTABLEN: emit(IROp::SETTABLE, {reg(b), IRConstantNumber{double(c + 1)}, reg(a)}); break;
            case LOP_NEWTABLE:
                if (b > 32) fail(pc, "table capacity out of range");
                emit(IROp::NEWTABLE, {reg(a), IRImmediate{b ? int64_t(1) << (b - 1) : 0}, IRImmediate{aux}});
                break;
            case LOP_DUPTABLE: emit(IROp::DUPTABLE, {reg(a), k(d)}); break;
            case LOP_SETLIST: emit(IROp::SETLIST, {reg(a), IRImmediate{aux}, range(b, int(c) - 1)}); break;
            case LOP_NEWCLOSURE: case LOP_DUPCLOSURE:
            {
                auto& closure = emit(op == LOP_NEWCLOSURE ? IROp::CLOSURE : IROp::DUPCLOSURE, {reg(a), IRFunctionRef{ids.at(child)}});
                for (uint32_t i = 1; i < length; ++i)
                {
                    Instruction capture = proto->code[pc + i];
                    auto kind = IRCapture::Kind(LUAU_INSN_A(capture));
                    unsigned index = LUAU_INSN_B(capture);
                    if (kind == IRCapture::Kind::UPVALUE) upval(index); else reg(index);
                    closure.operands.push_back(IRCapture{kind, uint8_t(index)});
                }
                break;
            }
            case LOP_NAMECALL: case LOP_NAMECALLUDATA:
                reg(a + 1);
                emit(IROp::NAMECALL, {reg(a), reg(b), k(op == LOP_NAMECALLUDATA ? aux & 0xffff : aux)});
                break;
            case LOP_CALL: case LOP_CALLFB:
                emit(IROp::CALL, {range(a, int(c) - 1), reg(a), range(a + 1, int(b) - 1)}); break;
            case LOP_GETVARARGS: emit(IROp::VARARGS, {range(a, int(b) - 1)}); break;
            case LOP_RETURN:
                emit(IROp::RETURN, {range(a, int(b) - 1)});
                terminated = true;
                break;
            case LOP_JUMP: case LOP_JUMPBACK: case LOP_JUMPX:
                emit(IROp::JUMP, {target()});
                terminated = true;
                break;
            case LOP_JUMPIF: case LOP_JUMPIFNOT:
                branch(op == LOP_JUMPIF ? IRCondition::TRUTHY : IRCondition::FALSY, {reg(a)}); break;
            case LOP_JUMPIFEQ: case LOP_JUMPIFLE: case LOP_JUMPIFLT:
            case LOP_JUMPIFNOTEQ: case LOP_JUMPIFNOTLE: case LOP_JUMPIFNOTLT:
            {
                static constexpr IRCondition conditions[] = {IRCondition::EQ, IRCondition::LE, IRCondition::LT,
                    IRCondition::NOT_EQ, IRCondition::NOT_LE, IRCondition::NOT_LT};
                branch(conditions[op - LOP_JUMPIFEQ], {reg(a), reg(aux & 255)});
                break;
            }
            case LOP_JUMPXEQKNIL: case LOP_JUMPXEQKB: case LOP_JUMPXEQKN: case LOP_JUMPXEQKS:
            {
                IROperand rhs = IRConstantNil{};
                if (op == LOP_JUMPXEQKB) rhs = IRConstantBool{bool(aux & 1)};
                else if (op != LOP_JUMPXEQKNIL) rhs = k(aux & 0xffffff);
                branch(aux >> 31 ? IRCondition::NOT_EQ : IRCondition::EQ, {reg(a), std::move(rhs)});
                break;
            }
            case LOP_ADD: case LOP_SUB: case LOP_MUL: case LOP_DIV: case LOP_MOD: case LOP_POW:
                emit(arithmetic[op - LOP_ADD], {reg(a), reg(b), reg(c)}); break;
            case LOP_ADDK: case LOP_SUBK: case LOP_MULK: case LOP_DIVK: case LOP_MODK: case LOP_POWK:
                emit(arithmetic[op - LOP_ADDK], {reg(a), reg(b), k(c)}); break;
            case LOP_SUBRK: case LOP_DIVRK:
                emit(op == LOP_SUBRK ? IROp::SUB : IROp::DIV, {reg(a), k(b), reg(c)}); break;
            case LOP_IDIV: emit(IROp::IDIV, {reg(a), reg(b), reg(c)}); break;
            case LOP_IDIVK: emit(IROp::IDIV, {reg(a), reg(b), k(c)}); break;
            case LOP_AND: case LOP_OR:
                emit(op == LOP_AND ? IROp::AND : IROp::OR, {reg(a), reg(b), reg(c)}); break;
            case LOP_ANDK: case LOP_ORK:
                emit(op == LOP_ANDK ? IROp::AND : IROp::OR, {reg(a), reg(b), k(c)}); break;
            case LOP_CONCAT: emit(IROp::CONCAT, {reg(a), range(b, int(c) - int(b) + 1)}); break;
            case LOP_NOT: case LOP_MINUS: case LOP_LENGTH:
            {
                static constexpr IROp unary[] = {IROp::NOT, IROp::MINUS, IROp::LENGTH};
                emit(unary[op - LOP_NOT], {reg(a), reg(b)});
                break;
            }
            case LOP_FORNPREP: case LOP_FORNLOOP:
                emit(op == LOP_FORNPREP ? IROp::FORNPREP : IROp::FORNLOOP, {reg(a), target(), blockRef(next)});
                terminated = true;
                break;
            case LOP_FORGPREP: case LOP_FORGPREP_NEXT: case LOP_FORGPREP_INEXT:
                emit(IROp::FORGPREP, {reg(a), target()});
                terminated = true;
                break;
            case LOP_FORGLOOP:
                emit(IROp::FORGLOOP, {reg(a), IRImmediate{aux & 255}, target(), blockRef(next)});
                terminated = true;
                break;
            case LOP_CMPPROTO: branch(IRCondition::PROTO_MISMATCH, {reg(a), IRImmediate{aux}}); break;
            case LOP_NEWCLASS:
                emit(IROp::NEWCLASS, {reg(a), k(aux), b == 255 ? IROperand{IRConstantNil{}} : IROperand{reg(b)}, IRConstantBool{bool(c & 1)}});
                break;
            case LOP_NEWCLASSMEMBER: emit(IROp::NEWCLASSMEMBER, {reg(a), k(aux), reg(c)}); break;
            case LOP_BREAK: case LOP_NATIVECALL: case LOP_CAPTURE: case LOP__COUNT:
                fail(pc, "unexpected runtime opcode or standalone CAPTURE");
            default:
                fail(pc, "unsupported opcode " + std::to_string(op));
            }
            if (next == block.endpc && !terminated)
            {
                if (next == uint32_t(proto->sizecode)) fail(pc, "function falls off the end");
                emit(IROp::JUMP, {blockRef(next)});
            }
        }
        return std::move(function);
    }
};
} // namespace

IRContext lift(std::string_view bytecode)
{
    if (bytecode.empty()) throw std::runtime_error("empty bytecode");
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    if (!state) throw std::runtime_error("could not create Luau state");
    if (luau_load(state.get(), "=stdin", bytecode.data(), bytecode.size(), 0) != 0)
    {
        const char* error = lua_tostring(state.get(), -1);
        throw std::runtime_error(error ? error : "could not load Luau bytecode");
    }
    const TValue* loaded = state->top - 1;
    if (!ttisfunction(loaded) || clvalue(loaded)->isC)
        throw std::runtime_error("expected a Luau closure");

    FunctionIds ids;
    std::vector<const Proto*> protos;
    auto add = [&](const Proto* proto) {
        if (ids.emplace(proto, uint32_t(protos.size())).second) protos.push_back(proto);
    };
    add(clvalue(loaded)->l.p);
    for (size_t i = 0; i < protos.size(); ++i)
    {
        const Proto* proto = protos[i];
        for (int j = 0; j < proto->sizep; ++j) add(proto->p[j]);
        for (int j = 0; j < proto->sizek; ++j)
            if (ttisfunction(&proto->k[j]) && !clvalue(&proto->k[j])->isC) add(clvalue(&proto->k[j])->l.p);
    }
    IRContext context;
    for (const Proto* proto : protos)
        context.functions.push_back(FunctionLifter{state.get(), proto, ids}.run());
    return context;
}

std::string dump(const IRContext& context)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    for (const auto& function : context.functions)
    {
        out << "function f" << function.id;
        if (function.debugname) out << ' ' << quote(*function.debugname);
        out << '(';
        for (size_t i = 0; i < function.parameters.size(); ++i)
        {
            const auto& parameter = function.parameters[i];
            out << (i ? ", " : "") << operandText(parameter.reg);
            if (parameter.debugname) out << ':' << quote(*parameter.debugname);
        }
        if (function.isVararg) out << (function.parameters.empty() ? "..." : ", ...");
        out << ") [stack=" << unsigned(function.maxstacksize) << ", upvalues=" << unsigned(function.upvalueCount) << "]\n";
        for (size_t i = 0; i < function.tables.size(); ++i)
        {
            out << "  t" << i << " = {";
            const auto& entries = function.tables[i].entries;
            for (size_t j = 0; j < entries.size(); ++j)
                out << (j ? ", " : "") << '[' << operandText(entries[j].first) << "] = " << operandText(entries[j].second);
            out << "}\n";
        }
        for (size_t i = 0; i < function.classes.size(); ++i)
        {
            const auto& shape = function.classes[i];
            out << "  c" << i << " = class " << quote(shape.name) << " {";
            for (const auto& field : shape.fields) out << " field " << quote(field);
            for (const auto& method : shape.methods) out << " method " << quote(method);
            out << " }\n";
        }
        for (const auto& block : function.blocks)
        {
            out << "  b" << block.id << " [" << block.startpc << ", " << block.endpc << "):\n";
            for (const auto& insn : block.instructions)
            {
                out << "    " << insn.pc << ": " << opName(insn.op);
                for (size_t i = 0; i < insn.operands.size(); ++i)
                    out << (i ? ", " : " ") << operandText(insn.operands[i]);
                out << '\n';
            }
        }
        out << '\n';
    }
    return out.str();
}
