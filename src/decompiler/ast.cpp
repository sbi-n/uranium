#include "ast.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace
{
using EK = ASTExpression::Kind;
using SK = ASTStatement::Kind;

ASTExpr expr(EK kind, std::string text = {}, std::vector<ASTExpr> children = {})
{
    auto e = std::make_shared<ASTExpression>();
    e->kind = kind; e->text = std::move(text); e->children = std::move(children);
    return e;
}
ASTExpr symbol(ASTSymbolId id) { auto e = expr(EK::SYMBOL); e->symbol = id; return e; }
ASTExpr literal(std::string text) { return expr(EK::LITERAL, std::move(text)); }
ASTStatement assign(std::vector<ASTExpr> lhs, std::vector<ASTExpr> rhs)
{
    ASTStatement s{SK::ASSIGN}; s.lhs = std::move(lhs); s.rhs = std::move(rhs); return s;
}
ASTExpr negate(ASTExpr e)
{
    if (e->kind == EK::UNARY && e->text == "not") return e->children[0];
    if (e->kind == EK::LITERAL && e->text == "true") return literal("false");
    if (e->kind == EK::LITERAL && e->text == "false") return literal("true");
    if (e->kind == EK::BINARY && (e->text == "==" || e->text == "~="))
        return expr(EK::BINARY, e->text == "==" ? "~=" : "==", e->children);
    // !(a <= b) is not a > b for NaN or arbitrary comparison metamethods.
    return expr(EK::UNARY, "not", {e});
}

std::string quote(std::string_view value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value)
        switch (c)
        {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 32 || c == 127) out << '\\' << std::setw(3) << std::setfill('0') << unsigned(c);
            else out << c;
        }
    out << '"'; return out.str();
}
std::string number(double value)
{
    if (std::isnan(value)) return "(0 / 0)";
    if (std::isinf(value)) return value < 0 ? "(-1 / 0)" : "(1 / 0)";
    if (value == 0 && std::signbit(value)) return "-0.0";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}
bool identifier(std::string_view name)
{
    auto alpha = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (name.empty() || !alpha(name[0])) return false;
    for (unsigned char c : name) if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
    static const std::set<std::string_view> keywords = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
        "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    return !keywords.count(name);
}
bool generatedName(std::string_view name)
{
    size_t prefix = name.starts_with("uv_") ? 3 : name.starts_with("v_") || name.starts_with("f_") ? 2 : 0;
    return prefix && name.size() > prefix && std::all_of(name.begin() + prefix, name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

struct DisjointSet
{
    std::vector<uint32_t> parents;
    explicit DisjointSet(size_t n) : parents(n) { std::iota(parents.begin(), parents.end(), 0); }
    uint32_t root(uint32_t x) { return parents[x] == x ? x : parents[x] = root(parents[x]); }
    void unite(uint32_t a, uint32_t b) { a = root(a); b = root(b); if (a != b) parents[std::max(a, b)] = std::min(a, b); }
};

struct Builder;
struct FunctionBuilder
{
    Builder& builder;
    const IRFunction& ir;
    const CFGFunction& cfg;
    const SSAFunction& ssa;
    std::shared_ptr<ASTFunction> function;
    std::vector<ASTSymbolId> upvalues;
    DisjointSet groups;
    std::map<uint32_t, ASTSymbolId> symbols;
    std::map<SSAValueId, ASTExpr> packs;
    std::vector<bool> visited;

    struct Loop
    {
        uint32_t header;
        uint32_t continuation;
        uint32_t exit;
        bool forLoop = false;
        bool repeat = false;
        uint32_t latch = 0;
    };

    FunctionBuilder(Builder& b, uint32_t proto, std::vector<ASTSymbolId> captures);
    [[noreturn]] void fail(uint32_t block, const std::string& message) const
    {
        throw std::runtime_error("AST: function " + std::to_string(ir.id) + " block " + std::to_string(block) + ": " + message);
    }
    SSAValueId use(uint32_t b, uint32_t i, uint16_t reg) const
    {
        for (auto u : ssa.blocks[b].instructions[i].uses) if (u.reg == reg) return u.value;
        fail(b, "missing SSA use for register " + std::to_string(reg));
    }
    SSAValueId definition(uint32_t b, uint32_t i, uint16_t reg) const
    {
        for (auto d : ssa.blocks[b].instructions[i].definitions) if (d.reg == reg) return d.value;
        fail(b, "missing SSA definition");
    }
    ASTSymbolId binding(SSAValueId value);
    ASTExpr operand(const IROperand& op, uint32_t b, uint32_t i);
    std::vector<ASTExpr> range(IRRegisterRange r, uint32_t b, uint32_t i);
    ASTExpr condition(uint32_t b);
    void instruction(ASTBlock& out, uint32_t b, uint32_t i);
    void instructions(ASTBlock& out, uint32_t b);
    void region(ASTBlock& out, uint32_t start, uint32_t stop, const Loop* loop = nullptr);
    void captureGroups();
    std::shared_ptr<ASTFunction> run();
};

struct Builder
{
    const SSAContext& ssa;
    explicit Builder(const SSAContext& ssa) : ssa(ssa) {}
    ASTContext ast;
    uint32_t nextFunction = 0;
    unsigned depth = 0;
    ASTSymbolId newSymbol(uint32_t owner)
    {
        ASTSymbolId id = uint32_t(ast.symbols.size());
        ast.symbols.push_back({ASTSymbol::Kind::VARIABLE, owner}); return id;
    }
    std::shared_ptr<ASTFunction> build(uint32_t proto, std::vector<ASTSymbolId> captures)
    {
        if (++depth > 200) throw std::runtime_error("AST: closure nesting exceeds 200");
        auto result = FunctionBuilder(*this, proto, std::move(captures)).run();
        --depth; return result;
    }
};

FunctionBuilder::FunctionBuilder(Builder& b, uint32_t proto, std::vector<ASTSymbolId> captures)
    : builder(b), ir(b.ssa.cfg.ir.functions.at(proto)), cfg(b.ssa.cfg.functions.at(proto)), ssa(b.ssa.functions.at(proto)),
      function(std::make_shared<ASTFunction>()), upvalues(std::move(captures)), groups(ssa.values.size()), visited(ir.blocks.size())
{
    function->id = b.nextFunction++;
    function->isVararg = ir.isVararg;
    // A numeric loop's initial index is evaluated outside the loop; its phi
    // and backedge value belong to the distinct, per-iteration loop binding.
    std::set<std::pair<uint32_t, uint16_t>> numericHeaders;
    for (const auto& block : ir.blocks)
    {
        const auto& term = block.instructions.back();
        if (term.op == IROp::FORNPREP)
            numericHeaders.emplace(std::get<IRBlockRef>(term.operands[2]).ref, std::get<IRRegister>(term.operands[0]).index + 2);
    }
    for (uint32_t block = 0; block < ssa.blocks.size(); ++block)
        for (const auto& phi : ssa.blocks[block].phis)
            for (auto [pred, input] : phi.inputs)
            {
                if (numericHeaders.count({block, phi.reg}) && ir.blocks[pred].instructions.back().op == IROp::FORNPREP) continue;
                groups.unite(phi.result, input);
            }
    captureGroups();
    for (const auto& parameter : ir.parameters)
    {
        auto id = binding(ssa.entryValues.at(parameter.reg.index));
        builder.ast.symbols[id].parameter = true;
        function->parameters.push_back(id);
    }
}

void FunctionBuilder::captureGroups()
{
    for (uint32_t b = 0; b < ir.blocks.size(); ++b)
        if (cfg.blocks[b].reachable)
            for (uint32_t i = 0; i < ir.blocks[b].instructions.size(); ++i)
            {
                const auto& insn = ir.blocks[b].instructions[i];
                if (insn.op != IROp::CLOSURE && insn.op != IROp::DUPCLOSURE) continue;
                for (size_t c = 2; c < insn.operands.size(); ++c)
                {
                    auto capture = std::get<IRCapture>(insn.operands[c]);
                    if (capture.kind != IRCapture::Kind::REFERENCE) continue;
                    auto initial = use(b, i, capture.index);
                    std::vector<std::pair<uint32_t, uint32_t>> work{{b, i + 1}};
                    std::set<std::pair<uint32_t, uint32_t>> seen;
                    while (!work.empty())
                    {
                        auto [node, offset] = work.back(); work.pop_back();
                        if (!seen.emplace(node, offset).second) continue;
                        bool closed = false;
                        for (uint32_t j = offset; j < ir.blocks[node].instructions.size(); ++j)
                        {
                            const auto& next = ir.blocks[node].instructions[j];
                            if (next.op == IROp::CLOSEUPVALS && std::get<IRRegister>(next.operands[0]).index <= capture.index)
                            { closed = true; break; }
                            for (auto d : ssa.blocks[node].instructions[j].definitions)
                                if (d.reg == capture.index) groups.unite(initial, d.value);
                        }
                        if (!closed) for (auto s : cfg.blocks[node].successors) work.emplace_back(s, 0);
                    }
                }
            }
}

ASTSymbolId FunctionBuilder::binding(SSAValueId value)
{
    auto root = groups.root(value);
    auto it = symbols.find(root);
    if (it != symbols.end()) return it->second;
    auto id = builder.newSymbol(function->id);
    symbols.emplace(root, id); return id;
}

ASTExpr FunctionBuilder::operand(const IROperand& op, uint32_t b, uint32_t i)
{
    if (auto r = std::get_if<IRRegister>(&op))
    {
        auto value = use(b, i, r->index);
        if (ssa.values[value].kind == SSAValue::Kind::UNDEFINED) return literal("nil");
        if (packs.count(value)) fail(b, "open result pack used as a scalar");
        return symbol(binding(value));
    }
    if (auto u = std::get_if<IRUpvalue>(&op)) return symbol(upvalues.at(u->index));
    if (std::holds_alternative<IRConstantNil>(op)) return literal("nil");
    if (auto v = std::get_if<IRConstantBool>(&op)) return literal(v->value ? "true" : "false");
    if (auto v = std::get_if<IRConstantNumber>(&op)) return literal(number(v->value));
    if (auto v = std::get_if<IRConstantInteger>(&op))
        return literal(v->value == std::numeric_limits<int64_t>::min() ? "0x8000000000000000i" : std::to_string(v->value) + "i");
    if (auto v = std::get_if<IRConstantString>(&op)) return literal(quote(v->value));
    if (auto v = std::get_if<IRConstantVector>(&op))
    {
        auto call = expr(EK::CALL, {}, {expr(EK::INDEX, "create", {expr(EK::GLOBAL, "vector")})});
        for (auto component : v->value) call->children.push_back(literal(number(component)));
        return call;
    }
    if (auto t = std::get_if<IRTableRef>(&op))
    {
        auto table = expr(EK::TABLE);
        for (const auto& [key, value] : ir.tables.at(t->ref).entries)
            table->fields.push_back({operand(key, b, i), operand(value, b, i)});
        return table;
    }
    if (auto f = std::get_if<IRFunctionRef>(&op))
    {
        auto closure = expr(EK::FUNCTION);
        closure->function = builder.build(f->ref, {});
        return closure;
    }
    fail(b, "operand is metadata, not an expression");
}

std::vector<ASTExpr> FunctionBuilder::range(IRRegisterRange r, uint32_t b, uint32_t i)
{
    std::vector<ASTExpr> result;
    int end = r.count < 0 ? ir.maxstacksize : r.start + r.count;
    for (int reg = r.start; reg < end; ++reg)
    {
        auto value = use(b, i, uint16_t(reg));
        if (r.count < 0)
        {
            auto pack = packs.find(value);
            if (pack != packs.end()) { result.push_back(pack->second); return result; }
        }
        result.push_back(operand(IRRegister{uint8_t(reg)}, b, i));
    }
    if (r.count < 0) fail(b, "dynamic stack top has no reaching result pack");
    return result;
}

ASTExpr FunctionBuilder::condition(uint32_t b)
{
    uint32_t i = uint32_t(ir.blocks[b].instructions.size() - 1);
    const auto& o = ir.blocks[b].instructions[i].operands;
    auto c = std::get<IRCondition>(o[0]);
    auto lhs = operand(o[1], b, i);
    switch (c)
    {
    case IRCondition::TRUTHY: return lhs;
    case IRCondition::FALSY: return negate(lhs);
    case IRCondition::EQ: case IRCondition::NOT_EQ: case IRCondition::LE: case IRCondition::NOT_LE: case IRCondition::LT: case IRCondition::NOT_LT:
    {
        const char* op = c == IRCondition::EQ ? "==" : c == IRCondition::NOT_EQ ? "~=" :
            c == IRCondition::LE || c == IRCondition::NOT_LE ? "<=" : "<";
        auto e = expr(EK::BINARY, op, {lhs, operand(o[2], b, i)});
        return c == IRCondition::NOT_LE || c == IRCondition::NOT_LT ? negate(e) : e;
    }
    case IRCondition::PROTO_MISMATCH: return literal("true");
    }
    fail(b, "unknown branch condition");
}

void FunctionBuilder::instruction(ASTBlock& out, uint32_t b, uint32_t i)
{
    const auto& insn = ir.blocks[b].instructions[i];
    const auto& o = insn.operands;
    auto get = [&](size_t n) { return operand(o.at(n), b, i); };
    auto dest = [&] { return symbol(binding(definition(b, i, std::get<IRRegister>(o[0]).index))); };
    auto emit = [&](ASTExpr e) { out.push_back(assign({dest()}, {std::move(e)})); };
    auto index = [&](ASTExpr base, const IROperand& key) {
        if (auto s = std::get_if<IRConstantString>(&key); s && identifier(s->value)) return expr(EK::INDEX, s->value, {base});
        return expr(EK::INDEX, {}, {base, operand(key, b, i)});
    };
    auto results = [&](IRRegisterRange r, ASTExpr e) {
        if (r.count < 0)
        {
            e->multret = true;
            // Only the first slot identifies the expandable tail, even though
            // SSA conservatively defines the remaining register slots as well.
            packs.emplace(definition(b, i, r.start), e);
        }
        else if (!r.count) { ASTStatement s{SK::EXPRESSION}; s.rhs = {e}; out.push_back(std::move(s)); }
        else
        {
            std::vector<ASTExpr> lhs;
            for (int n = 0; n < r.count; ++n) lhs.push_back(symbol(binding(definition(b, i, r.start + n))));
            e->multret = r.count > 1;
            out.push_back(assign(std::move(lhs), {e}));
        }
    };
    switch (insn.op)
    {
    case IROp::MOVE: emit(get(1)); break;
    case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV: case IROp::IDIV: case IROp::MOD: case IROp::POW: case IROp::AND: case IROp::OR:
    {
        static const char* ops[] = {"+", "-", "*", "/", "//", "%", "^", "and", "or"};
        emit(expr(EK::BINARY, ops[int(insn.op) - int(IROp::ADD)], {get(1), get(2)})); break;
    }
    case IROp::CONCAT:
    {
        auto values = range(std::get<IRRegisterRange>(o[1]), b, i);
        auto e = values.back();
        for (size_t n = values.size() - 1; n-- > 0;) e = expr(EK::BINARY, "..", {values[n], e});
        emit(e); break;
    }
    case IROp::NOT: case IROp::MINUS: case IROp::LENGTH:
        emit(expr(EK::UNARY, insn.op == IROp::NOT ? "not" : insn.op == IROp::MINUS ? "-" : "#", {get(1)})); break;
    case IROp::GETGLOBAL: emit(expr(EK::GLOBAL, std::get<IRConstantString>(o[1]).value)); break;
    case IROp::SETGLOBAL: out.push_back(assign({expr(EK::GLOBAL, std::get<IRConstantString>(o[0]).value)}, {get(1)})); break;
    case IROp::GETUPVAL: emit(get(1)); break;
    case IROp::SETUPVAL: out.push_back(assign({get(0)}, {get(1)})); break;
    case IROp::CLOSEUPVALS: break;
    case IROp::GETIMPORT:
    {
        auto e = expr(EK::GLOBAL, std::get<IRConstantString>(o[1]).value);
        for (size_t n = 2; n < o.size(); ++n) e = index(e, o[n]);
        emit(e); break;
    }
    case IROp::GETTABLE: emit(index(get(1), o[2])); break;
    case IROp::SETTABLE:
    {
        auto value = get(2);
        if (value->kind == EK::SYMBOL && builder.ast.symbols[value->symbol].captured)
        {
            auto snapshot = builder.newSymbol(function->id);
            out.push_back(assign({symbol(snapshot)}, {value})); value = symbol(snapshot);
        }
        out.push_back(assign({index(get(0), o[1])}, {value})); break;
    }
    case IROp::NEWTABLE: emit(expr(EK::TABLE)); break;
    case IROp::DUPTABLE: emit(get(1)); break;
    case IROp::SETLIST:
    {
        ASTStatement s{SK::SETLIST}; s.lhs = {get(0)};
        s.firstIndex = std::get<IRImmediate>(o[1]).value;
        s.rhs = range(std::get<IRRegisterRange>(o[2]), b, i);
        out.push_back(std::move(s)); break;
    }
    case IROp::CLOSURE: case IROp::DUPCLOSURE:
    {
        auto id = dest()->symbol;
        builder.ast.symbols[id].kind = ASTSymbol::Kind::FUNCTION;
        if (!builder.ast.symbols[id].debugname)
            builder.ast.symbols[id].debugname = builder.ssa.cfg.ir.functions.at(std::get<IRFunctionRef>(o[1]).ref).debugname;
        std::vector<ASTSymbolId> captures;
        for (size_t n = 2; n < o.size(); ++n)
        {
            auto capture = std::get<IRCapture>(o[n]);
            ASTSymbolId captured;
            if (capture.kind == IRCapture::Kind::UPVALUE) captured = upvalues.at(capture.index);
            else
            {
                auto value = use(b, i, capture.index);
                captured = binding(value);
                if (capture.kind == IRCapture::Kind::VALUE)
                {
                    // A value capture must remain a snapshot even when its source
                    // is also part of a mutable phi/capture group.
                    size_t definitions = 0;
                    for (SSAValueId v = 0; v < ssa.values.size(); ++v)
                        if (groups.root(v) == groups.root(value) && ssa.values[v].kind != SSAValue::Kind::PHI) ++definitions;
                    if (definitions > 1)
                    {
                        auto snapshot = builder.newSymbol(function->id);
                        out.push_back(assign({symbol(snapshot)}, {symbol(captured)})); captured = snapshot;
                    }
                }
            }
            auto& info = builder.ast.symbols[captured];
            info.captured = true;
            if (info.kind != ASTSymbol::Kind::FUNCTION) info.kind = ASTSymbol::Kind::UPVALUE;
            captures.push_back(captured);
        }
        auto e = expr(EK::FUNCTION);
        e->function = builder.build(std::get<IRFunctionRef>(o[1]).ref, std::move(captures));
        emit(e); break;
    }
    case IROp::NAMECALL: break; // Reconstructed together with its CALL.
    case IROp::CALL:
    {
        auto fnValue = use(b, i, std::get<IRRegister>(o[1]).index);
        const auto& origin = ssa.values[fnValue];
        auto args = std::get<IRRegisterRange>(o[2]);
        ASTExpr e;
        if (origin.kind == SSAValue::Kind::INSTRUCTION && ir.blocks[origin.block].instructions[origin.instruction].op == IROp::NAMECALL)
        {
            const auto& method = ir.blocks[origin.block].instructions[origin.instruction];
            auto receiver = operand(method.operands[1], origin.block, origin.instruction);
            const auto& name = std::get<IRConstantString>(method.operands[2]).value;
            if (!identifier(name)) fail(b, "NAMECALL method is not an identifier");
            e = expr(EK::METHOD_CALL, name, {receiver});
            ++args.start; if (args.count >= 0) --args.count;
        }
        else e = expr(EK::CALL, {}, {get(1)});
        e->argumentsBeforeCallee = insn.argumentsBeforeCallee;
        if (e->argumentsBeforeCallee && origin.kind == SSAValue::Kind::INSTRUCTION && origin.block == b && origin.instruction + 1 == i &&
            !out.empty() && out.back().kind == SK::ASSIGN && out.back().lhs.size() == 1 && out.back().rhs.size() == 1 &&
            out.back().lhs[0]->kind == EK::SYMBOL && e->children[0]->kind == EK::SYMBOL &&
            out.back().lhs[0]->symbol == e->children[0]->symbol && !builder.ast.symbols[e->children[0]->symbol].captured)
        {
            // The fallback loads the function after evaluating its arguments.
            // Attach that load now: open-result arguments are stored as nested
            // expressions and must still execute before the callee lookup.
            e->children[0] = out.back().rhs[0];
            out.pop_back();
        }
        auto arguments = range(args, b, i);
        e->children.insert(e->children.end(), arguments.begin(), arguments.end());
        results(std::get<IRRegisterRange>(o[0]), e); break;
    }
    case IROp::VARARGS:
        if (auto r = std::get<IRRegisterRange>(o[0]); r.count != 0) results(r, expr(EK::VARARGS));
        break;
    case IROp::NEWCLASS:
    {
        const auto& shape = ir.classes.at(std::get<IRClassRef>(o[1]).ref);
        ASTStatement s{SK::CLASS}; s.lhs = {dest()}; s.rhs = {get(2)};
        s.text = shape.name; s.fields = shape.fields; s.open = std::get<IRConstantBool>(o[3]).value;
        out.push_back(std::move(s)); break;
    }
    case IROp::NEWCLASSMEMBER:
    {
        auto cls = get(0), value = get(2);
        size_t classIndex = out.size(), closureIndex = out.size();
        if (cls->kind != EK::SYMBOL || value->kind != EK::SYMBOL) fail(b, "invalid class member binding");
        for (size_t n = 0; n < out.size(); ++n)
        {
            auto& s = out[n];
            if (s.lhs.size() != 1 || s.lhs[0]->kind != EK::SYMBOL) continue;
            if (s.kind == SK::CLASS && s.lhs[0]->symbol == cls->symbol) classIndex = n;
            if (s.kind == SK::ASSIGN && s.lhs[0]->symbol == value->symbol && s.rhs.size() == 1 && s.rhs[0]->kind == EK::FUNCTION) closureIndex = n;
        }
        if (classIndex >= closureIndex || closureIndex == out.size()) fail(b, "class method is not a local closure");
        ASTStatement method{SK::METHOD}; method.text = std::get<IRConstantString>(o[1]).value; method.rhs = out[closureIndex].rhs;
        out[classIndex].body.push_back(std::move(method));
        out.erase(out.begin() + closureIndex); break;
    }
    default: fail(b, "terminator encountered inside a basic block");
    }
}

void FunctionBuilder::instructions(ASTBlock& out, uint32_t b)
{
    for (uint32_t i = 0; i + 1 < ir.blocks[b].instructions.size(); ++i) instruction(out, b, i);
}

void FunctionBuilder::region(ASTBlock& out, uint32_t start, uint32_t stop, const Loop* active)
{
    const uint32_t exit = uint32_t(ir.blocks.size());
    auto transfer = [&](uint32_t target) -> std::optional<SK> {
        if (!active) return {};
        std::set<uint32_t> seen;
        while (target < exit && seen.insert(target).second)
        {
            if (target == active->exit) return SK::BREAK;
            if (target == active->header && !active->forLoop) return SK::CONTINUE;
            if (target == active->continuation &&
                (!active->forLoop || ir.blocks[target].instructions.size() == 1)) return SK::CONTINUE;
            const auto& block = ir.blocks[target];
            if (block.instructions.size() != 1 || block.instructions.back().op != IROp::JUMP) return {};
            target = cfg.blocks[target].successors[0];
        }
        return {};
    };
    uint32_t b = start;
    while (b != stop && b != exit)
    {
        if (active && b == active->exit)
        { out.push_back(ASTStatement{SK::BREAK}); return; }
        bool loopEpilogue = active && active->forLoop && b == active->latch;
        if (active && visited[b] && (b == active->continuation || b == active->header) && !loopEpilogue)
        { out.push_back(ASTStatement{SK::CONTINUE}); return; }
        if (visited[b])
        {
            // The compiler shares tiny boolean/nil materialization tails across
            // expression paths. Repeating those copies is harmless; repeating
            // an effectful block or rebuilding a closure is not.
            const auto& code = ir.blocks[b].instructions;
            bool materialization = code.back().op == IROp::JUMP || code.back().op == IROp::RETURN;
            for (size_t i = 0; i + 1 < code.size(); ++i)
                materialization &= code[i].op == IROp::MOVE && code[i].operands.size() == 2 &&
                    (std::holds_alternative<IRConstantNil>(code[i].operands[1]) ||
                     std::holds_alternative<IRConstantBool>(code[i].operands[1]));
            if (!materialization && !loopEpilogue) fail(b, "control-flow region overlaps an already emitted region");
        }
        const auto& term = ir.blocks[b].instructions.back();
        uint32_t ti = uint32_t(ir.blocks[b].instructions.size() - 1);

        // FOR*PREP retains the exact evaluation order of the initial expressions.
        if (term.op == IROp::FORNPREP || term.op == IROp::FORGPREP)
        {
            visited[b] = true;
            instructions(out, b);
            auto base = std::get<IRRegister>(term.operands[0]).index;
            uint32_t header, latch = exit, after;
            ASTStatement loop{term.op == IROp::FORNPREP ? SK::NUMERIC_FOR : SK::GENERIC_FOR};
            if (term.op == IROp::FORNPREP)
            {
                header = std::get<IRBlockRef>(term.operands[2]).ref;
                after = std::get<IRBlockRef>(term.operands[1]).ref;
                for (uint32_t node = header; node < ir.blocks.size(); ++node)
                {
                    const auto& candidate = ir.blocks[node].instructions.back();
                    if (candidate.op == IROp::FORNLOOP && std::get<IRRegister>(candidate.operands[0]).index == base
                        && std::get<IRBlockRef>(candidate.operands[1]).ref == header) { latch = node; break; }
                }
                if (latch == exit) fail(b, "numeric loop has no matching latch");
                auto indexValue = definition(latch, uint32_t(ir.blocks[latch].instructions.size() - 1), base + 2);
                auto id = binding(indexValue);
                builder.ast.symbols[id].loopVariable = true;
                loop.names = {id};
                loop.rhs = {operand(IRRegister{uint8_t(base + 2)}, b, ti), operand(IRRegister{base}, b, ti), operand(IRRegister{uint8_t(base + 1)}, b, ti)};
            }
            else
            {
                latch = std::get<IRBlockRef>(term.operands[1]).ref;
                const auto& end = ir.blocks[latch].instructions.back();
                if (end.op != IROp::FORGLOOP) fail(b, "generic loop has no matching latch");
                header = std::get<IRBlockRef>(end.operands[2]).ref;
                after = std::get<IRBlockRef>(end.operands[3]).ref;
                for (int v = 0; v < std::get<IRImmediate>(end.operands[1]).value; ++v)
                {
                    auto id = binding(definition(latch, uint32_t(ir.blocks[latch].instructions.size() - 1), base + 3 + v));
                    builder.ast.symbols[id].loopVariable = true; loop.names.push_back(id);
                }
                loop.rhs = {operand(IRRegister{base}, b, ti), operand(IRRegister{uint8_t(base + 1)}, b, ti), operand(IRRegister{uint8_t(base + 2)}, b, ti)};
            }
            Loop context{header, latch, after, true, false, latch};
            region(loop.body, header, exit, &context);
            visited[latch] = true;
            out.push_back(std::move(loop)); b = after; continue;
        }

        const CFGLoop* natural = nullptr;
        for (const auto& loop : cfg.loops)
            if (loop.header == b && (!active || (active->header != b && !(active->forLoop && active->latch == b)))) { natural = &loop; break; }
        if (natural)
        {
            uint32_t after = natural->exits.empty() ? exit : natural->exits.back();
            auto inside = [&](uint32_t node) { return std::binary_search(natural->blocks.begin(), natural->blocks.end(), node); };
            if (term.op == IROp::BRANCH && cfg.blocks[b].successors.size() == 2
                && inside(cfg.blocks[b].successors[0]) != inside(cfg.blocks[b].successors[1]))
            {
                auto taken = cfg.blocks[b].successors[0], fallthrough = cfg.blocks[b].successors[1];
                uint32_t body = inside(taken) ? taken : fallthrough;
                after = inside(taken) ? fallthrough : taken;
                ASTStatement loop{SK::WHILE}; loop.condition = literal("true");
                visited[b] = true; instructions(loop.body, b);
                ASTStatement guard{SK::IF}; guard.condition = inside(taken) ? negate(condition(b)) : condition(b);
                guard.body.push_back(ASTStatement{SK::BREAK}); loop.body.push_back(std::move(guard));
                Loop context{b, b, after};
                region(loop.body, body, b, &context);
                out.push_back(std::move(loop)); b = after; continue;
            }
            uint32_t latch = exit;
            if (natural->latches.size() == 1)
            {
                auto l = natural->latches[0];
                const auto& end = ir.blocks[l].instructions.back();
                if (end.op == IROp::BRANCH && natural->exits.size() == 1) latch = l;
            }
            ASTStatement loop{latch != exit ? SK::REPEAT : SK::WHILE};
            loop.condition = literal("true");
            Loop context{b, latch != exit ? latch : b, after, false, latch != exit, latch};
            region(loop.body, b, exit, &context);
            if (latch != exit)
            {
                auto taken = std::get<IRBlockRef>(ir.blocks[latch].instructions.back().operands[ir.blocks[latch].instructions.back().operands.size() - 2]).ref;
                loop.condition = taken == b ? negate(condition(latch)) : condition(latch);
            }
            out.push_back(std::move(loop)); b = after; continue;
        }

        visited[b] = true;
        instructions(out, b);
        if (active && active->repeat && b == active->latch) return;
        switch (term.op)
        {
        case IROp::RETURN:
        {
            ASTStatement s{SK::RETURN}; s.rhs = range(std::get<IRRegisterRange>(term.operands[0]), b, ti);
            out.push_back(std::move(s)); return;
        }
        case IROp::JUMP:
        {
            uint32_t next = cfg.blocks[b].successors.at(0);
            if (auto action = transfer(next); action && next != stop)
            { out.push_back(ASTStatement{*action}); return; }
            if (active && next == active->exit) { out.push_back(ASTStatement{SK::BREAK}); return; }
            if (active && next == active->continuation)
            {
                if (next == stop) return;
                if (!active->forLoop && !active->repeat) { out.push_back(ASTStatement{SK::CONTINUE}); return; }
                // An explicit continue target is a separate block containing only
                // the latch (or repeat condition). Ordinary fallthrough emits it.
                if (next != b + 1 && (!active->forLoop || ir.blocks[next].instructions.size() == 1))
                { out.push_back(ASTStatement{SK::CONTINUE}); return; }
            }
            b = next; break;
        }
        case IROp::BRANCH:
        {
            if (cfg.blocks[b].successors.size() == 1)
            {
                if (std::get<IRCondition>(term.operands[0]) != IRCondition::PROTO_MISMATCH)
                {
                    // Equal successors do not make a comparison removable: its
                    // metamethod can still produce observable effects.
                    ASTStatement test{SK::IF}; test.condition = condition(b); out.push_back(std::move(test));
                }
                b = cfg.blocks[b].successors[0]; break;
            }
            // Plan short-circuit regions from the inside out before emitting
            // either arm. Planning must not create symbols/closures or mark blocks
            // visited: an outer test may be unable to absorb a candidate region.
            struct TestRegion
            {
                struct Step { bool onTaken, sharedTaken; std::shared_ptr<TestRegion> child; };
                uint32_t entry, taken, fallthrough;
                std::set<uint32_t> nodes;
                std::vector<Step> steps;
                uint32_t prefix = UINT32_MAX;
            };
            std::function<TestRegion(uint32_t, std::set<uint32_t>, uint32_t)> planTest;
            planTest = [&](uint32_t entry, std::set<uint32_t> available, uint32_t boundary) {
                const auto& successors = cfg.blocks[entry].successors;
                TestRegion plan{entry, successors[0], successors[1], {entry}, {}};
                available.insert(entry);
                for (;;)
                {
                    if (plan.taken == boundary || plan.fallthrough == boundary) return plan;
                    bool merged = false;
                    for (bool onTaken : {true, false})
                    {
                        uint32_t child = onTaken ? plan.taken : plan.fallthrough;
                        uint32_t shared = onTaken ? plan.fallthrough : plan.taken;
                        if (child <= entry || child == stop || child >= exit || visited[child] || available.count(child) ||
                            (active && (child == active->header || child == active->continuation || child == active->exit))) continue;
                        const auto& node = cfg.blocks[child];
                        if (ir.blocks[child].instructions.back().op != IROp::BRANCH || node.successors.size() != 2 ||
                            std::any_of(node.predecessors.begin(), node.predecessors.end(),
                                [&](uint32_t pred) { return cfg.blocks[pred].reachable && !available.count(pred); }) ||
                            std::any_of(cfg.loops.begin(), cfg.loops.end(),
                                [&](const CFGLoop& loop) { return loop.header == child; })) continue;
                        auto nested = planTest(child, available, shared);
                        // Inlined condition evaluation can contain its own ifs.
                        // Follow their postdominators to the actual outer test.
                        uint32_t tail = child;
                        while (nested.taken != shared && nested.fallthrough != shared)
                        {
                            auto next = cfg.blocks[tail].immediatePostDominator;
                            if (next <= int32_t(tail) || uint32_t(next) >= exit || uint32_t(next) >= shared || uint32_t(next) == stop ||
                                visited[next] || ir.blocks[next].instructions.back().op != IROp::BRANCH ||
                                cfg.blocks[next].successors.size() != 2) break;
                            std::set<uint32_t> prefix;
                            std::vector<uint32_t> work{child};
                            bool closed = true;
                            while (!work.empty() && closed)
                            {
                                auto node = work.back(); work.pop_back();
                                if (node == uint32_t(next) || !prefix.insert(node).second) continue;
                                if (node < child || node >= uint32_t(next) || node == stop || visited[node] ||
                                    (active && (node == active->header || node == active->continuation || node == active->exit)) ||
                                    std::any_of(cfg.loops.begin(), cfg.loops.end(),
                                        [&](const CFGLoop& loop) { return loop.header == node; }))
                                { closed = false; break; }
                                for (auto successor : cfg.blocks[node].successors) work.push_back(successor);
                            }
                            prefix.insert(uint32_t(next));
                            for (auto node : prefix)
                                for (auto pred : cfg.blocks[node].predecessors)
                                    if (cfg.blocks[pred].reachable && !prefix.count(pred) && !available.count(pred)) closed = false;
                            if (!closed) break;
                            tail = uint32_t(next);
                            auto prefixAvailable = available;
                            prefixAvailable.insert(prefix.begin(), prefix.end());
                            nested = planTest(tail, std::move(prefixAvailable), shared);
                            nested.nodes.insert(prefix.begin(), prefix.end());
                            nested.prefix = child;
                        }
                        bool sharedTaken = nested.taken == shared;
                        if (!sharedTaken && nested.fallthrough != shared) continue;
                        plan.taken = sharedTaken ? nested.fallthrough : nested.taken;
                        plan.fallthrough = shared;
                        available.insert(nested.nodes.begin(), nested.nodes.end());
                        plan.nodes.insert(nested.nodes.begin(), nested.nodes.end());
                        plan.steps.push_back({onTaken, sharedTaken, std::make_shared<TestRegion>(std::move(nested))});
                        merged = true;
                        break;
                    }
                    if (!merged) return plan;
                }
            };
            std::function<ASTExpr(ASTBlock&, const TestRegion&)> emitTest;
            emitTest = [&](ASTBlock& output, const TestRegion& plan) {
                auto test = condition(plan.entry);
                for (const auto& step : plan.steps)
                {
                    auto result = symbol(builder.newSymbol(function->id));
                    ASTStatement evaluation{SK::IF};
                    evaluation.condition = step.onTaken ? test : negate(test);
                    if (step.child->prefix != UINT32_MAX)
                        region(evaluation.body, step.child->prefix, step.child->entry, active);
                    visited[step.child->entry] = true;
                    instructions(evaluation.body, step.child->entry);
                    auto childTest = emitTest(evaluation.body, *step.child);
                    evaluation.body.push_back(assign({result}, {step.sharedTaken ? negate(childTest) : childTest}));
                    evaluation.alternative.push_back(assign({result}, {literal("false")}));
                    output.push_back(std::move(evaluation));
                    test = result;
                }
                return test;
            };
            auto plan = planTest(b, {}, exit);
            uint32_t taken = plan.taken, fallthrough = plan.fallthrough;
            ASTExpr test = emitTest(out, plan);
            auto takenAction = transfer(taken), fallthroughAction = transfer(fallthrough);
            if (takenAction || fallthroughAction)
            {
                ASTStatement guard{SK::IF};
                guard.condition = takenAction ? test : negate(test);
                guard.body.push_back(ASTStatement{takenAction ? *takenAction : *fallthroughAction});
                out.push_back(std::move(guard));
                if (takenAction && fallthroughAction) { out.push_back(ASTStatement{*fallthroughAction}); return; }
                b = takenAction ? fallthrough : taken;
                break;
            }
            uint32_t join = cfg.blocks[b].immediatePostDominator < 0 ? exit : uint32_t(cfg.blocks[b].immediatePostDominator);
            bool terminalJoin = join == exit || (active &&
                (join == active->exit || join == active->header || join == active->continuation));
            if (active && join == active->exit) join = active->continuation;
            if (active && (join == active->header || join == active->continuation)) join = stop;
            auto reachableBeforeBoundary = [&](uint32_t from) {
                std::vector<bool> seen(exit);
                std::vector<uint32_t> work{from};
                while (!work.empty())
                {
                    auto node = work.back(); work.pop_back();
                    if (node == exit || seen[node]) continue;
                    seen[node] = true;
                    if (node == stop || visited[node] || transfer(node)) continue;
                    for (auto successor : cfg.blocks[node].successors) work.push_back(successor);
                }
                return seen;
            };
            // Returns and loop transfers need not pass through the continuation.
            // Respect the enclosing boundary, then recover a shared successor
            // when strict postdominance can only identify the function/loop exit.
            if (terminalJoin)
            {
                auto yes = reachableBeforeBoundary(taken), no = reachableBeforeBoundary(fallthrough);
                if (stop != exit && (yes[stop] || no[stop])) join = stop;
                // Bytecode blocks are ordered by PC. The first shared forward
                // block is the continuation of a structured source branch, even
                // when either arm also has paths ending in return/continue.
                for (uint32_t node = b + 1; node < exit; ++node)
                    if (yes[node] && no[node] && !visited[node] && !transfer(node))
                    { join = node; break; }
            }
            ASTStatement branch{SK::IF}; branch.condition = negate(test);
            region(branch.body, fallthrough, join, active);
            region(branch.alternative, taken, join, active);
            out.push_back(std::move(branch));
            b = join; break;
        }
        case IROp::FORNLOOP: case IROp::FORGLOOP:
            if (!active || !active->forLoop) fail(b, "loop latch outside its loop");
            // A branch can enter the iteration epilogue early (for example an
            // inlined return). Execute its instructions, then actually transfer
            // to the next iteration instead of falling through the enclosing if.
            out.push_back(ASTStatement{SK::CONTINUE});
            return;
        default: fail(b, "unsupported terminator");
        }
    }
}

std::shared_ptr<ASTFunction> FunctionBuilder::run()
{
    region(function->body, 0, uint32_t(ir.blocks.size()));
    return function;
}

// Keep expression rewrites separate from control-flow recovery. In particular,
// moving a load/call across another effect can change metamethods and captures.
void eachExpr(ASTExpr& e, const std::function<void(ASTExpr&)>& visit, bool closures = true);
void eachBlock(ASTBlock& block, const std::function<void(ASTExpr&)>& visit, bool closures = true)
{
    for (auto& s : block)
    {
        for (auto& e : s.lhs) eachExpr(e, visit, closures);
        for (auto& e : s.rhs) eachExpr(e, visit, closures);
        eachExpr(s.condition, visit, closures);
        eachBlock(s.body, visit, closures); eachBlock(s.alternative, visit, closures);
    }
}
void eachExpr(ASTExpr& e, const std::function<void(ASTExpr&)>& visit, bool closures)
{
    if (!e) return;
    visit(e);
    for (auto& child : e->children) eachExpr(child, visit, closures);
    for (auto& field : e->fields) { eachExpr(field.key, visit, closures); eachExpr(field.value, visit, closures); }
    if (closures && e->function) eachBlock(e->function->body, visit, closures);
}
unsigned count(const ASTExpr& e, ASTSymbolId id)
{
    if (!e) return 0;
    unsigned result = e->kind == EK::SYMBOL && e->symbol == id;
    for (auto& child : e->children) result += count(child, id);
    for (auto& field : e->fields) result += count(field.key, id) + count(field.value, id);
    return result;
}
unsigned directUses(const ASTStatement& s, ASTSymbolId id)
{
    unsigned result = count(s.condition, id);
    for (const auto& e : s.rhs) result += count(e, id);
    for (const auto& e : s.lhs) if (e->kind != EK::SYMBOL) result += count(e, id);
    return result;
}
struct Usage { std::vector<unsigned> reads, writes; };
Usage usage(ASTContext& ast)
{
    Usage result{std::vector<unsigned>(ast.symbols.size()), std::vector<unsigned>(ast.symbols.size())};
    std::function<void(const ASTExpr&)> read;
    std::function<void(const ASTBlock&)> block;
    read = [&](const ASTExpr& e) {
        if (!e) return;
        if (e->kind == EK::SYMBOL) ++result.reads[e->symbol];
        for (auto& c : e->children) read(c);
        for (auto& f : e->fields) { read(f.key); read(f.value); }
        if (e->function) block(e->function->body);
    };
    block = [&](const ASTBlock& body) {
        for (const auto& s : body)
        {
            for (const auto& e : s.lhs)
                if (e->kind == EK::SYMBOL) ++result.writes[e->symbol]; else read(e);
            for (auto id : s.names) ++result.writes[id];
            for (const auto& e : s.rhs) read(e);
            read(s.condition); block(s.body); block(s.alternative);
        }
    };
    block(ast.entry->body); return result;
}
bool stable(const ASTExpr& e, const ASTContext& ast, const Usage& uses)
{
    if (!e) return true;
    if (e->kind == EK::LITERAL) return true;
    if (e->kind == EK::SYMBOL)
    {
        const auto& info = ast.symbols[e->symbol];
        return !info.loopVariable && uses.writes[e->symbol] <= (info.parameter ? 0u : 1u);
    }
    if (e->kind == EK::UNARY && e->text == "not") return stable(e->children[0], ast, uses);
    return false;
}
// These expressions neither execute user code nor snapshot a shared mutable
// binding. A closure's value captures have already been made explicit above.
bool passive(const ASTExpr& e, const ASTContext& ast)
{
    if (!e || e->kind == EK::LITERAL || e->kind == EK::VARARGS || e->kind == EK::FUNCTION) return true;
    if (e->kind == EK::SYMBOL) return !ast.symbols[e->symbol].captured;
    if (e->kind == EK::UNARY && e->text == "not") return passive(e->children[0], ast);
    return e->kind == EK::TABLE && e->fields.empty();
}
bool writesSymbol(const ASTStatement& s, ASTSymbolId id)
{
    for (const auto& lhs : s.lhs) if (lhs->kind == EK::SYMBOL && lhs->symbol == id) return true;
    if (std::find(s.names.begin(), s.names.end(), id) != s.names.end()) return true;
    for (const auto& child : s.body) if (writesSymbol(child, id)) return true;
    for (const auto& child : s.alternative) if (writesSymbol(child, id)) return true;
    return false;
}
bool canMovePast(const ASTStatement& s, const ASTExpr& value, const ASTContext& ast)
{
    // Calls cannot reassign an uncaptured local. A copy can cross unrelated
    // effects, but must stop at any direct or conditional write to its source.
    if (value->kind == EK::SYMBOL && !ast.symbols[value->symbol].captured)
        return !writesSymbol(s, value->symbol);
    if (s.kind != SK::ASSIGN) return false;
    for (const auto& lhs : s.lhs)
        if (lhs->kind != EK::SYMBOL || ast.symbols[lhs->symbol].captured || count(value, lhs->symbol)) return false;
    for (const auto& rhs : s.rhs) if (!passive(rhs, ast)) return false;
    return true;
}
bool firstUse(const ASTExpr& e, ASTSymbolId id, bool& barrier, bool conditional, const ASTContext& ast, const Usage& uses)
{
    if (!e) return false;
    if (e->kind == EK::SYMBOL && e->symbol == id) return !barrier && !conditional;
    for (size_t i = 0; i < e->children.size(); ++i)
    {
        // Luau compiles fast-call arguments before the fallback callee lookup.
        // Inspect that order so the lookup does not block folding its arguments.
        size_t child = e->kind == EK::CALL && e->argumentsBeforeCallee ? (i + 1) % e->children.size() : i;
        if (firstUse(e->children[child], id, barrier, conditional || (e->kind == EK::BINARY && child == 1 && (e->text == "and" || e->text == "or")) ||
            (e->kind == EK::CONDITIONAL && child > 0), ast, uses)) return true;
    }
    for (auto& f : e->fields)
        if (firstUse(f.key, id, barrier, conditional, ast, uses) || firstUse(f.value, id, barrier, conditional, ast, uses)) return true;
    if (!stable(e, ast, uses) && !passive(e, ast)) barrier = true;
    return false;
}
bool canInline(const ASTStatement& s, ASTSymbolId id, const ASTExpr& value, const ASTContext& ast, const Usage& uses)
{
    if (stable(value, ast, uses)) return true;
    if (s.kind == SK::WHILE || s.kind == SK::REPEAT) return false;
    // A later assignment or loop iteration does not require a snapshot for
    // this expression: its local writes happen after the operands are read.
    if (value->kind == EK::SYMBOL && !ast.symbols[value->symbol].captured) return true;
    bool barrier = false;
    for (auto& e : s.lhs)
        // An assignment evaluates the table and key, but does not read the
        // indexed value. The actual store happens after evaluating the RHS.
        if (e->kind == EK::INDEX)
        {
            for (auto& child : e->children) if (firstUse(child, id, barrier, false, ast, uses)) return true;
        }
        else if (e->kind != EK::SYMBOL && firstUse(e, id, barrier, false, ast, uses)) return true;
    for (auto& e : s.rhs) if (firstUse(e, id, barrier, false, ast, uses)) return true;
    return firstUse(s.condition, id, barrier, false, ast, uses);
}
void substitute(ASTStatement& s, ASTSymbolId id, const ASTExpr& value)
{
    auto replace = [&](ASTExpr& e) { if (e->kind == EK::SYMBOL && e->symbol == id) e = value; };
    for (auto& e : s.lhs) if (e->kind != EK::SYMBOL) eachExpr(e, replace, false);
    for (auto& e : s.rhs) eachExpr(e, replace, false);
    eachExpr(s.condition, replace, false);
}
bool functionValueUse(const ASTExpr& e, ASTSymbolId id)
{
    if (!e) return false;
    if (e->kind == EK::CALL || e->kind == EK::METHOD_CALL)
        for (size_t i = 1; i < e->children.size(); ++i)
            if (e->children[i]->kind == EK::SYMBOL && e->children[i]->symbol == id) return true;
    for (const auto& child : e->children) if (functionValueUse(child, id)) return true;
    for (const auto& field : e->fields)
    {
        if (field.value->kind == EK::SYMBOL && field.value->symbol == id) return true;
        if (functionValueUse(field.key, id) || functionValueUse(field.value, id)) return true;
    }
    return false;
}
bool functionValueUse(const ASTStatement& s, ASTSymbolId id)
{
    // Moving a closure into a loop condition would create a new function on
    // every iteration. Keep its original identity and capture lifetime.
    if (s.kind == SK::WHILE || s.kind == SK::REPEAT) return false;
    if (s.kind == SK::ASSIGN && s.lhs.size() == 1 && s.lhs[0]->kind == EK::INDEX && s.rhs.size() == 1 &&
        s.rhs[0]->kind == EK::SYMBOL && s.rhs[0]->symbol == id) return true;
    for (const auto& e : s.lhs) if (functionValueUse(e, id)) return true;
    for (const auto& e : s.rhs) if (functionValueUse(e, id)) return true;
    return functionValueUse(s.condition, id);
}
bool inlineUse(ASTStatement& s, ASTSymbolId id, const ASTExpr& value, const ASTContext& ast, const Usage& uses)
{
    if (directUses(s, id) == 1 && (value->kind == EK::FUNCTION ? functionValueUse(s, id) : canInline(s, id, value, ast, uses)))
    {
        substitute(s, id, value); return true;
    }
    // Only immutable values can move into a branch or be evaluated repeatedly
    // in a loop. Calls and other observable evaluations stay on their path.
    if (stable(value, ast, uses))
    {
        for (auto& child : s.body) if (inlineUse(child, id, value, ast, uses)) return true;
        for (auto& child : s.alternative) if (inlineUse(child, id, value, ast, uses)) return true;
    }
    return false;
}

bool foldTables(ASTBlock& block, ASTContext& ast)
{
    bool changed = false;
    for (size_t i = 0; i < block.size(); ++i)
    {
        auto& init = block[i];
        if (init.kind != SK::ASSIGN || init.lhs.size() != 1 || init.lhs[0]->kind != EK::SYMBOL || init.rhs.size() != 1 || init.rhs[0]->kind != EK::TABLE) continue;
        auto id = init.lhs[0]->symbol;
        if (ast.symbols[id].captured) continue;
        auto table = init.rhs[0];
        std::set<ASTSymbolId> dependencies;
        auto record = [&](ASTExpr& e) { if (e->kind == EK::SYMBOL) dependencies.insert(e->symbol); };
        for (auto& f : table->fields) { eachExpr(f.key, record, false); eachExpr(f.value, record, false); }
        for (size_t j = i + 1; j < block.size(); ++j)
        {
            auto& s = block[j];
            if (s.kind != SK::ASSIGN && s.kind != SK::SETLIST) break;
            bool writesTable = s.lhs.size() == 1 &&
                ((s.kind == SK::SETLIST && s.lhs[0]->kind == EK::SYMBOL && s.lhs[0]->symbol == id) ||
                 (s.kind == SK::ASSIGN && s.lhs[0]->kind == EK::INDEX && s.lhs[0]->children[0]->kind == EK::SYMBOL && s.lhs[0]->children[0]->symbol == id));
            if (!writesTable)
            {
                bool conflict = directUses(s, id) != 0;
                for (const auto& lhs : s.lhs)
                    conflict |= lhs->kind != EK::SYMBOL || lhs->symbol == id || dependencies.count(lhs->symbol);
                // Captured tables are excluded above. Creating an unrelated
                // callback does not expose this table or execute the callback.
                for (auto dependency : dependencies)
                    if (ast.symbols[dependency].captured)
                        for (auto& rhs : s.rhs)
                            if (rhs->kind != EK::LITERAL && rhs->kind != EK::SYMBOL) conflict = true;
                bool fieldEffects = false;
                for (const auto& field : table->fields)
                    for (const auto& e : {field.key, field.value})
                        if (e && e->kind != EK::LITERAL && e->kind != EK::SYMBOL) fieldEffects = true;
                if (fieldEffects)
                    for (const auto& rhs : s.rhs)
                        if (rhs->kind != EK::LITERAL && !(rhs->kind == EK::SYMBOL && !ast.symbols[rhs->symbol].captured)) conflict = true;
                if (conflict) break;
                continue;
            }
            bool self = false;
            for (const auto& rhs : s.rhs) self |= count(rhs, id) != 0;
            if (s.lhs[0]->children.size() > 1) self |= count(s.lhs[0]->children[1], id) != 0;
            if (self) break;
            if (s.kind == SK::SETLIST)
            {
                int64_t next = 1;
                for (const auto& f : table->fields) if (!f.key) ++next;
                if (!s.rhs.empty() && s.rhs.back()->multret && s.firstIndex != next) break;
                for (size_t k = 0; k < s.rhs.size(); ++k)
                {
                    int64_t index = s.firstIndex + int64_t(k);
                    table->fields.push_back({index == next ? nullptr : literal(std::to_string(index)), s.rhs[k]});
                    if (index == next) ++next;
                }
            }
            else
            {
                auto lhs = s.lhs[0];
                auto key = lhs->text.empty() ? lhs->children[1] : literal(quote(lhs->text));
                bool overwrittenList = false;
                int64_t listIndex = 0;
                for (const auto& field : table->fields)
                    if (!field.key && key->kind == EK::LITERAL && key->text == std::to_string(++listIndex)) overwrittenList = true;
                if (overwrittenList) break;
                // DUPTABLE templates may contain placeholder values. Discard
                // overwritten constants without retaining duplicate record keys.
                table->fields.erase(std::remove_if(table->fields.begin(), table->fields.end(), [&](const ASTTableField& field) {
                    return key->kind == EK::LITERAL && field.key && field.key->kind == EK::LITERAL && field.key->text == key->text && field.value->kind == EK::LITERAL;
                }), table->fields.end());
                table->fields.push_back({key, s.rhs[0]});
            }
            for (auto& f : table->fields)
            {
                eachExpr(f.key, record, false); eachExpr(f.value, record, false);
            }
            // Place construction after the value computations, at the original
            // store. No table reference escapes across the moved allocation.
            ASTStatement moved = std::move(block[i]);
            block.erase(block.begin() + i);
            --j;
            block[j] = std::move(moved);
            i = j; changed = true;
        }
    }
    return changed;
}

bool equalExpr(const ASTExpr& a, const ASTExpr& b)
{
    if (!a || !b) return a == b;
    if (a->kind != b->kind || a->text != b->text || a->symbol != b->symbol || a->children.size() != b->children.size() ||
        a->multret != b->multret || a->argumentsBeforeCallee != b->argumentsBeforeCallee ||
        a->function || b->function || !a->fields.empty() || !b->fields.empty()) return false;
    for (size_t i = 0; i < a->children.size(); ++i) if (!equalExpr(a->children[i], b->children[i])) return false;
    return true;
}
bool booleanExpr(const ASTExpr& e)
{
    return (e->kind == EK::UNARY && e->text == "not") || (e->kind == EK::LITERAL && (e->text == "true" || e->text == "false")) ||
        (e->kind == EK::BINARY && (e->text == "==" || e->text == "~=" || e->text == "<" || e->text == "<=" || e->text == ">" || e->text == ">="));
}
// Only truthiness is observed in conditions. Here a false-valued conditional
// can use and/or even when its operands themselves can be nil or non-booleans.
bool simplifyCondition(ASTExpr& e)
{
    if (!e) return false;
    bool changed = false;
    if (e->kind == EK::CONDITIONAL)
    {
        auto yes = e->children[1], no = e->children[2];
        auto falsy = [](const ASTExpr& value) {
            return value->kind == EK::LITERAL && (value->text == "false" || value->text == "nil");
        };
        if (falsy(no)) { e = expr(EK::BINARY, "and", {e->children[0], yes}); changed = true; }
        else if (falsy(yes)) { e = expr(EK::BINARY, "and", {negate(e->children[0]), no}); changed = true; }
        else if (yes->kind == EK::LITERAL && yes->text == "true")
        { e = expr(EK::BINARY, "or", {e->children[0], no}); changed = true; }
        else if (no->kind == EK::LITERAL && no->text == "true")
        { e = expr(EK::BINARY, "or", {negate(e->children[0]), yes}); changed = true; }
    }
    if ((e->kind == EK::UNARY && e->text == "not") ||
        (e->kind == EK::BINARY && (e->text == "and" || e->text == "or")))
    {
        // Expressions can be shared with assignments where their exact value
        // still matters; don't rewrite children through those shared pointers.
        e = std::make_shared<ASTExpression>(*e);
        for (auto& child : e->children) changed |= simplifyCondition(child);
    }
    return changed;
}
bool hasContinue(const ASTBlock& block)
{
    for (const auto& s : block)
    {
        if (s.kind == SK::CONTINUE) return true;
        if (s.kind == SK::IF && (hasContinue(s.body) || hasContinue(s.alternative))) return true;
    }
    return false;
}
bool guard(const ASTStatement& s, SK action)
{
    return s.kind == SK::IF && s.alternative.empty() && s.body.size() == 1 && s.body[0].kind == action;
}

bool optimizeBlock(ASTBlock& block, ASTContext& ast, const Usage& uses)
{
    bool changed = false;
    for (auto& s : block)
    {
        if (s.kind == SK::IF && s.body.size() == 1 && s.body[0].kind == SK::ASSIGN && s.body[0].lhs.size() == 1 && s.body[0].lhs[0]->kind == EK::SYMBOL && s.body[0].rhs.size() == 1)
        {
            auto lhs = s.body[0].lhs[0];
            auto yes = s.body[0].rhs[0];
            if (s.alternative.empty() && (equalExpr(s.condition, lhs) ||
                (s.condition->kind == EK::UNARY && s.condition->text == "not" && equalExpr(s.condition->children[0], lhs))))
            {
                auto op = s.condition->kind == EK::UNARY ? "or" : "and";
                s = assign({lhs}, {expr(EK::BINARY, op, {lhs, yes})}); changed = true;
            }
            else if (s.alternative.size() == 1 && s.alternative[0].kind == SK::ASSIGN && s.alternative[0].lhs.size() == 1 &&
                equalExpr(lhs, s.alternative[0].lhs[0]) && s.alternative[0].rhs.size() == 1)
            {
                auto no = s.alternative[0].rhs[0];
                ASTExpr value;
                if (yes->kind == EK::LITERAL && no->kind == EK::LITERAL && yes->text == "false" && no->text == "true") value = negate(s.condition);
                else if (yes->kind == EK::LITERAL && no->kind == EK::LITERAL && yes->text == "true" && no->text == "false")
                    value = booleanExpr(s.condition) ? s.condition : expr(EK::UNARY, "not", {expr(EK::UNARY, "not", {s.condition})});
                else if (yes->kind == EK::BINARY && yes->text == "or" && equalExpr(yes->children[1], no))
                    value = expr(EK::BINARY, "or", {expr(EK::BINARY, "and", {s.condition, yes->children[0]}), no});
                else value = expr(EK::CONDITIONAL, {}, {s.condition, yes, no});
                s = assign({lhs}, {value}); changed = true;
            }
        }
        changed |= optimizeBlock(s.body, ast, uses);
        changed |= optimizeBlock(s.alternative, ast, uses);
        auto nested = [&](ASTExpr& e) { if (e->function) changed |= optimizeBlock(e->function->body, ast, uses); };
        for (auto& e : s.lhs) eachExpr(e, nested, false);
        for (auto& e : s.rhs) eachExpr(e, nested, false);
        eachExpr(s.condition, nested, false);
    }
    changed |= foldTables(block, ast);
    for (size_t i = 0; i < block.size(); ++i)
    {
        auto& s = block[i];
        if (s.kind != SK::ASSIGN || s.lhs.size() != 1 || s.lhs[0]->kind != EK::SYMBOL || s.rhs.size() != 1) continue;
        auto id = s.lhs[0]->symbol;
        if (s.rhs[0]->kind == EK::SYMBOL && s.rhs[0]->symbol == id)
        { block.erase(block.begin() + i--); changed = true; continue; }
        const auto& info = ast.symbols[id];
        if (info.captured || info.parameter || info.loopVariable || uses.writes[id] != 1) continue;
        if (info.kind == ASTSymbol::Kind::FUNCTION && s.rhs[0]->kind != EK::FUNCTION) continue;
        if (uses.reads[id] == 0 && (stable(s.rhs[0], ast, uses) || s.rhs[0]->kind == EK::VARARGS))
        { block.erase(block.begin() + i--); changed = true; continue; }
        if (uses.reads[id] == 0 && (s.rhs[0]->kind == EK::CALL || s.rhs[0]->kind == EK::METHOD_CALL))
        { s.kind = SK::EXPRESSION; s.lhs.clear(); changed = true; continue; }
        if (uses.reads[id] != 1) continue;
        for (size_t j = i + 1; j < block.size(); ++j)
        {
            if (inlineUse(block[j], id, s.rhs[0], ast, uses))
            {
                block.erase(block.begin() + i--); changed = true; break;
            }
            // Constructing a callback does not execute its body. Its value
            // captures are explicit snapshots, so it can move past effects
            // to its sole argument use in this same block.
            if (s.rhs[0]->kind != EK::FUNCTION && !stable(s.rhs[0], ast, uses) && !canMovePast(block[j], s.rhs[0], ast)) break;
        }
    }
    // Merge iterator triplets back into their originating multiple-result call.
    for (size_t i = 0; i + 1 < block.size(); ++i)
    {
        auto& a = block[i]; auto& b = block[i + 1];
        if (a.kind != SK::ASSIGN || a.lhs.size() != 3 || a.rhs.size() != 1 || b.kind != SK::GENERIC_FOR || b.rhs.size() != 3) continue;
        bool match = true;
        for (size_t j = 0; j < 3; ++j)
            match &= a.lhs[j]->kind == EK::SYMBOL && b.rhs[j]->kind == EK::SYMBOL && a.lhs[j]->symbol == b.rhs[j]->symbol && uses.reads[a.lhs[j]->symbol] == 1;
        if (match) { b.rhs = a.rhs; block.erase(block.begin() + i--); changed = true; }
    }
    for (auto& s : block)
    {
        changed |= simplifyCondition(s.condition);
        if (s.kind == SK::WHILE && s.condition->kind == EK::LITERAL && s.condition->text == "true" && !s.body.empty())
        {
            auto& guard = s.body.front();
            if (guard.kind == SK::IF && guard.alternative.empty() && guard.body.size() == 1 && guard.body[0].kind == SK::BREAK)
            { s.condition = negate(guard.condition); s.body.erase(s.body.begin()); changed = true; }
        }
        if (s.kind == SK::IF && s.body.empty() && !s.alternative.empty())
        { s.condition = negate(s.condition); s.body = std::move(s.alternative); s.alternative.clear(); changed = true; }
        if (s.kind == SK::IF && s.alternative.empty() && s.body.size() == 1 && s.body[0].kind == SK::IF && s.body[0].alternative.empty())
        {
            auto inner = std::move(s.body[0]);
            s.condition = expr(EK::BINARY, "and", {s.condition, inner.condition}); s.body = std::move(inner.body); changed = true;
        }
        if (s.kind == SK::GENERIC_FOR)
            while (s.rhs.size() > 1 && s.rhs.back()->kind == EK::LITERAL && s.rhs.back()->text == "nil")
            { s.rhs.pop_back(); changed = true; }
        if (s.kind == SK::NUMERIC_FOR && s.rhs.size() == 3 && s.rhs.back()->kind == EK::LITERAL && s.rhs.back()->text == "1")
        { s.rhs.pop_back(); changed = true; }
        if ((s.kind == SK::WHILE || s.kind == SK::REPEAT || s.kind == SK::NUMERIC_FOR || s.kind == SK::GENERIC_FOR)
            && !s.body.empty() && s.body.back().kind == SK::CONTINUE)
        { s.body.pop_back(); changed = true; }
        if (s.kind == SK::WHILE && s.condition->kind == EK::LITERAL && s.condition->text == "true" && !s.body.empty() && guard(s.body.back(), SK::BREAK))
        {
            while (s.body.size() >= 2 && guard(s.body[s.body.size() - 2], SK::CONTINUE))
            {
                auto previous = std::move(s.body[s.body.size() - 2]);
                s.body.back().condition = expr(EK::BINARY, "and", {negate(previous.condition), s.body.back().condition});
                s.body.erase(s.body.end() - 2); changed = true;
            }
            if (!hasContinue(s.body))
            {
                s.kind = SK::REPEAT; s.condition = s.body.back().condition; s.body.pop_back(); changed = true;
            }
        }
    }
    for (size_t i = 0; i + 1 < block.size(); ++i)
    {
        auto& a = block[i]; auto& b = block[i + 1];
        if (a.kind == SK::ASSIGN && b.kind == SK::ASSIGN && a.lhs.size() == 1 && b.lhs.size() == 1 &&
            a.lhs[0]->kind == EK::SYMBOL && equalExpr(a.lhs[0], b.lhs[0]) && a.rhs.size() == 1 && b.rhs.size() == 1 &&
            !ast.symbols[a.lhs[0]->symbol].captured && count(b.rhs[0], a.lhs[0]->symbol) == 1 && canInline(b, a.lhs[0]->symbol, a.rhs[0], ast, uses))
        {
            substitute(b, a.lhs[0]->symbol, a.rhs[0]); block.erase(block.begin() + i--); changed = true;
        }
    }
    return changed;
}

// Find the smallest lexical scope containing every use/definition, including
// uses by nested closures. This avoids one giant declaration of VM registers.
void declareFunction(ASTFunction& fn, ASTContext& ast)
{
    struct Scope { ASTBlock* block; int parent; size_t parentIndex; };
    struct Place { int scope; size_t index; };
    std::vector<Scope> scopes;
    std::map<ASTSymbolId, std::vector<Place>> places;
    auto record = [&](ASTSymbolId id, int scope, size_t index) {
        if (ast.symbols[id].owner == fn.id && !ast.symbols[id].parameter && !ast.symbols[id].loopVariable)
            places[id].push_back({scope, index});
    };
    auto expression = [&](ASTExpr& e, int scope, size_t index) {
        eachExpr(e, [&](ASTExpr& value) { if (value->kind == EK::SYMBOL) record(value->symbol, scope, index); });
    };
    std::function<int(ASTBlock&, int, size_t)> scan = [&](ASTBlock& block, int parent, size_t parentIndex) {
        int scope = int(scopes.size()); scopes.push_back({&block, parent, parentIndex});
        for (size_t i = 0; i < block.size(); ++i)
        {
            auto& s = block[i];
            for (auto& e : s.lhs) expression(e, scope, i);
            for (auto& e : s.rhs) expression(e, scope, i);
            if (s.kind != SK::REPEAT) expression(s.condition, scope, i);
            int child = scan(s.body, scope, i);
            if (s.kind == SK::REPEAT) expression(s.condition, child, s.body.size());
            scan(s.alternative, scope, i);
        }
        return scope;
    };
    scan(fn.body, -1, 0);
    std::map<std::pair<int, size_t>, std::vector<ASTSymbolId>> declarations;
    for (const auto& [id, occurrences] : places)
    {
        int scope = occurrences.front().scope;
        for (auto occurrence : occurrences)
        {
            std::set<int> ancestors;
            for (int s = occurrence.scope; s >= 0; s = scopes[s].parent) ancestors.insert(s);
            while (!ancestors.count(scope)) scope = scopes[scope].parent;
        }
        size_t first = scopes[scope].block->size();
        for (auto occurrence : occurrences)
        {
            while (occurrence.scope != scope)
            { occurrence.index = scopes[occurrence.scope].parentIndex; occurrence.scope = scopes[occurrence.scope].parent; }
            first = std::min(first, occurrence.index);
        }
        declarations[{scope, first}].push_back(id);
    }
    // Modify deeper scopes first; parent vector insertions invalidate child addresses.
    for (int scope = int(scopes.size()); scope-- > 0;)
    {
        auto& block = *scopes[scope].block;
        for (size_t index = block.size() + 1; index-- > 0;)
        {
            auto found = declarations.find({scope, index});
            if (found == declarations.end()) continue;
            const auto& ids = found->second;
            bool local = index < block.size() && block[index].kind == SK::ASSIGN && block[index].lhs.size() == ids.size();
            if (local)
                for (auto& lhs : block[index].lhs)
                    local &= lhs->kind == EK::SYMBOL && std::find(ids.begin(), ids.end(), lhs->symbol) != ids.end();
            // A non-function initializer reading the new local needs a separate
            // declaration; Lua's local initializer otherwise resolves outer names.
            if (local)
                for (auto& rhs : block[index].rhs)
                    if (rhs->kind != EK::FUNCTION) for (auto id : ids) if (count(rhs, id)) local = false;
            if (local) block[index].kind = SK::LOCAL;
            else
            {
                ASTStatement declaration{SK::LOCAL};
                for (auto id : ids) declaration.lhs.push_back(symbol(id));
                block.insert(block.begin() + index, std::move(declaration));
            }
        }
    }
    eachBlock(fn.body, [&](ASTExpr& e) { if (e->function) declareFunction(*e->function, ast); }, false);
    if (!fn.body.empty() && fn.body.back().kind == SK::RETURN && fn.body.back().rhs.empty()) fn.body.pop_back();
}

struct Printer
{
    const ASTContext& ast;
    explicit Printer(const ASTContext& ast) : ast(ast)
    {
        for (const auto& info : ast.symbols)
            if (info.kind == ASTSymbol::Kind::FUNCTION && info.debugname && identifier(*info.debugname))
                preferredNames.insert(*info.debugname);
        reserveNames(ast.entry->body);
    }
    std::ostringstream out;
    std::map<ASTSymbolId, std::string> names;
    std::set<std::string> preferredNames;
    std::set<std::string> usedNames{"getfenv"};
    uint32_t variables = 0, upvalues = 0, functions = 0;
    void reserveNames(const ASTExpr& e)
    {
        if (!e) return;
        if (e->kind == EK::GLOBAL) usedNames.insert(e->text);
        for (const auto& child : e->children) reserveNames(child);
        for (const auto& field : e->fields) { reserveNames(field.key); reserveNames(field.value); }
        if (e->function) reserveNames(e->function->body);
    }
    void reserveNames(const ASTBlock& body)
    {
        for (const auto& s : body)
        {
            if (s.kind == SK::CLASS) usedNames.insert(s.text);
            for (const auto& e : s.lhs) reserveNames(e);
            for (const auto& e : s.rhs) reserveNames(e);
            reserveNames(s.condition); reserveNames(s.body); reserveNames(s.alternative);
        }
    }
    void indent(unsigned depth) { out << std::string(depth * 4, ' '); }
    const std::string& name(ASTSymbolId id, bool declaration = false)
    {
        auto it = names.find(id);
        if (it != names.end()) return it->second;
        if (!declaration) throw std::runtime_error("AST printer: symbol used before its declaration");
        const auto& info = ast.symbols.at(id);
        std::string text;
        if (info.kind == ASTSymbol::Kind::FUNCTION && info.debugname && identifier(*info.debugname))
        {
            text = *info.debugname;
            for (unsigned suffix = 1; usedNames.count(text) || (text != *info.debugname && preferredNames.count(text)); ++suffix)
                text = *info.debugname + "_" + std::to_string(suffix);
        }
        else
        {
            do
                text = info.kind == ASTSymbol::Kind::FUNCTION ? "f_" + std::to_string(functions++) :
                    info.kind == ASTSymbol::Kind::UPVALUE ? "uv_" + std::to_string(upvalues++) : "v_" + std::to_string(variables++);
            while (usedNames.count(text) || preferredNames.count(text));
        }
        usedNames.insert(text);
        return names.emplace(id, std::move(text)).first->second;
    }
    int precedence(const ASTExpr& e)
    {
        if (e->kind == EK::CONDITIONAL) return 0;
        if (e->kind == EK::UNARY || (e->kind == EK::LITERAL && !e->text.empty() && e->text.front() == '-')) return 7;
        if (e->kind != EK::BINARY) return 10;
        auto& op = e->text;
        return op == "or" ? 1 : op == "and" ? 2 : op == "==" || op == "~=" || op == "<" || op == "<=" || op == ">" || op == ">=" ? 3 :
            op == ".." ? 4 : op == "+" || op == "-" ? 5 : op == "^" ? 8 : 6;
    }
    void prefix(const ASTExpr& e, unsigned depth)
    {
        bool parens = e->kind != EK::SYMBOL && e->kind != EK::GLOBAL && e->kind != EK::INDEX && e->kind != EK::CALL && e->kind != EK::METHOD_CALL;
        if (parens) out << '(';
        expression(e, depth);
        if (parens) out << ')';
    }
    bool startsWithParen(const ASTExpr& e)
    {
        if (e->kind == EK::CALL || e->kind == EK::METHOD_CALL || e->kind == EK::INDEX)
        {
            auto& base = e->children[0];
            return (base->kind != EK::SYMBOL && base->kind != EK::GLOBAL && base->kind != EK::INDEX && base->kind != EK::CALL && base->kind != EK::METHOD_CALL) || startsWithParen(base);
        }
        return false;
    }
    void list(const std::vector<ASTExpr>& values, unsigned depth, bool expands = true, size_t start = 0)
    {
        for (size_t i = start; i < values.size(); ++i)
        {
            if (i != start) out << ", ";
            auto& e = values[i];
            bool single = expands && i + 1 == values.size() && !e->multret && (e->kind == EK::CALL || e->kind == EK::METHOD_CALL || e->kind == EK::VARARGS);
            if (single) out << '(';
            expression(e, depth);
            if (single) out << ')';
        }
    }
    void parameters(const ASTFunction& fn)
    {
        out << '(';
        for (size_t i = 0; i < fn.parameters.size(); ++i) out << (i ? ", " : "") << name(fn.parameters[i], true);
        if (fn.isVararg) out << (fn.parameters.empty() ? "..." : ", ...");
        out << ')';
    }
    void expression(const ASTExpr& e, unsigned depth, int parent = 0)
    {
        int p = precedence(e);
        bool parens = p < parent;
        if (parens) out << '(';
        switch (e->kind)
        {
        case EK::LITERAL: out << e->text; break;
        case EK::SYMBOL: out << name(e->symbol); break;
        case EK::GLOBAL:
            if (identifier(e->text) && !generatedName(e->text)) out << e->text;
            else out << "getfenv()[" << quote(e->text) << ']';
            break;
        case EK::UNARY:
            out << e->text << (e->text == "not" ? " " : "");
            // Avoid emitting -- (a comment) for nested negation.
            if (e->text == "-" && ((e->children[0]->kind == EK::UNARY && e->children[0]->text == "-") ||
                (e->children[0]->kind == EK::LITERAL && e->children[0]->text.starts_with('-'))))
            { out << '('; expression(e->children[0], depth); out << ')'; }
            else expression(e->children[0], depth, p);
            break;
        case EK::BINARY:
        {
            bool right = e->text == "^" || e->text == "..";
            expression(e->children[0], depth, p + (right ? 1 : 0));
            out << ' ' << e->text << ' ';
            expression(e->children[1], depth, p + (right ? 0 : 1)); break;
        }
        case EK::INDEX:
            prefix(e->children[0], depth);
            if (!e->text.empty()) out << '.' << e->text;
            else { out << '['; expression(e->children[1], depth); out << ']'; }
            break;
        case EK::CALL: case EK::METHOD_CALL:
            prefix(e->children[0], depth);
            if (e->kind == EK::METHOD_CALL) out << ':' << e->text;
            out << '('; list(e->children, depth, true, 1); out << ')'; break;
        case EK::VARARGS: out << "..."; break;
        case EK::CONDITIONAL:
            out << "if "; expression(e->children[0], depth, 1); out << " then "; expression(e->children[1], depth);
            out << " else "; expression(e->children[2], depth); break;
        case EK::FUNCTION:
            out << "function"; parameters(*e->function); out << '\n';
            block(e->function->body, depth + 1); indent(depth); out << "end"; break;
        case EK::TABLE:
            out << '{';
            for (size_t i = 0; i < e->fields.size(); ++i)
            {
                if (i) out << ", ";
                const auto& field = e->fields[i];
                if (field.key)
                {
                    auto& key = field.key;
                    if (key->kind == EK::LITERAL && key->text.size() >= 2 && key->text.front() == '"' && identifier(std::string_view(key->text).substr(1, key->text.size() - 2)))
                        out << key->text.substr(1, key->text.size() - 2);
                    else { out << '['; expression(key, depth); out << ']'; }
                    out << " = ";
                }
                bool single = !field.key && i + 1 == e->fields.size() && !field.value->multret &&
                    (field.value->kind == EK::CALL || field.value->kind == EK::METHOD_CALL || field.value->kind == EK::VARARGS);
                if (single) out << '(';
                expression(field.value, depth);
                if (single) out << ')';
            }
            out << '}'; break;
        }
        if (parens) out << ')';
    }
    void branch(const ASTStatement& s, unsigned depth, bool elseif = false)
    {
        out << (elseif ? "elseif " : "if "); expression(s.condition, depth); out << " then\n";
        block(s.body, depth + 1);
        if (s.alternative.size() == 1 && s.alternative[0].kind == SK::IF)
        { indent(depth); branch(s.alternative[0], depth, true); return; }
        if (!s.alternative.empty()) { indent(depth); out << "else\n"; block(s.alternative, depth + 1); }
        indent(depth); out << "end\n";
    }
    void block(const ASTBlock& body, unsigned depth)
    {
        for (const auto& s : body)
        {
            indent(depth);
            switch (s.kind)
            {
            case SK::LOCAL: case SK::ASSIGN:
            {
                if (s.kind == SK::LOCAL)
                    for (auto& lhs : s.lhs) name(lhs->symbol, true);
                if (s.kind == SK::LOCAL && s.lhs.size() == 1 && s.rhs.size() == 1 && s.rhs[0]->kind == EK::FUNCTION)
                {
                    out << "local function " << name(s.lhs[0]->symbol);
                    auto& fn = *s.rhs[0]->function; parameters(fn); out << '\n';
                    block(fn.body, depth + 1); indent(depth); out << "end\n"; break;
                }
                if (s.kind == SK::LOCAL) out << "local ";
                else if (!s.lhs.empty() && startsWithParen(s.lhs[0])) out << ';';
                list(s.lhs, depth, false);
                if (!s.rhs.empty()) { out << " = "; list(s.rhs, depth, s.lhs.size() > s.rhs.size()); }
                out << '\n'; break;
            }
            case SK::EXPRESSION:
                if (!s.rhs.empty() && startsWithParen(s.rhs[0])) out << ';';
                list(s.rhs, depth, false); out << '\n'; break;
            case SK::RETURN:
                out << "return"; if (!s.rhs.empty()) { out << ' '; list(s.rhs, depth); } out << '\n'; break;
            case SK::IF: branch(s, depth); break;
            case SK::WHILE:
                out << "while "; expression(s.condition, depth); out << " do\n";
                block(s.body, depth + 1); indent(depth); out << "end\n"; break;
            case SK::REPEAT:
                out << "repeat\n"; block(s.body, depth + 1); indent(depth); out << "until "; expression(s.condition, depth); out << '\n'; break;
            case SK::NUMERIC_FOR: case SK::GENERIC_FOR:
                out << "for ";
                for (size_t i = 0; i < s.names.size(); ++i) out << (i ? ", " : "") << name(s.names[i], true);
                out << (s.kind == SK::NUMERIC_FOR ? " = " : " in "); list(s.rhs, depth, s.kind == SK::GENERIC_FOR); out << " do\n";
                block(s.body, depth + 1); indent(depth); out << "end\n"; break;
            case SK::BREAK: out << "break\n"; break;
            case SK::CONTINUE: out << "continue\n"; break;
            case SK::CLASS:
                // Class names are runtime shape metadata, like property names;
                // retain them and use the generated binding for all references.
                out << (s.open ? "open class " : "class ") << s.text;
                if (!(s.rhs[0]->kind == EK::LITERAL && s.rhs[0]->text == "nil"))
                { out << " extends "; expression(s.rhs[0], depth); }
                out << '\n';
                for (const auto& field : s.fields) { indent(depth + 1); out << "public " << field << '\n'; }
                block(s.body, depth + 1); indent(depth); out << "end\n";
                indent(depth); expression(s.lhs[0], depth); out << " = " << s.text << '\n'; break;
            case SK::METHOD:
                out << "function " << s.text;
                parameters(*s.rhs[0]->function); out << '\n';
                block(s.rhs[0]->function->body, depth + 1); indent(depth); out << "end\n"; break;
            case SK::SETLIST:
                for (size_t i = 0; i < s.rhs.size(); ++i)
                {
                    if (s.rhs[i]->multret) throw std::runtime_error("AST: open SETLIST could not be reconstructed as a table constructor");
                    if (i) indent(depth);
                    expression(s.lhs[0], depth); out << '[' << s.firstIndex + int64_t(i) << "] = "; expression(s.rhs[i], depth); out << '\n';
                }
                break;
            }
        }
    }
};
} // namespace

ASTContext buildAST(const SSAContext& ssa)
{
    if (ssa.functions.empty()) throw std::runtime_error("AST: missing entry function");
    if (ssa.cfg.ir.functions[0].upvalueCount) throw std::runtime_error("AST: entry chunk has unbound upvalues");
    Builder builder{ssa};
    builder.ast.entry = builder.build(0, {});
    while (optimizeBlock(builder.ast.entry->body, builder.ast, usage(builder.ast))) {}
    declareFunction(*builder.ast.entry, builder.ast);
    return std::move(builder.ast);
}

std::string printAST(const ASTContext& ast)
{
    if (!ast.entry) throw std::runtime_error("AST: missing entry function");
    Printer printer{ast};
    for (auto id : ast.entry->parameters) printer.name(id, true);
    printer.block(ast.entry->body, 0);
    return printer.out.str();
}
