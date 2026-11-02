#include "ssa.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>

namespace
{
struct Effects { std::vector<uint16_t> reads, writes; };

Effects effects(const IRInstruction& insn, uint16_t stack)
{
    Effects e;
    auto read = [&](uint16_t r) { e.reads.push_back(r); };
    auto write = [&](uint16_t r) { e.writes.push_back(r); };
    auto range = [&](IRRegisterRange r, bool dest) {
        int end = r.count < 0 ? stack : r.start + r.count;
        for (int i = r.start; i < end; ++i) (dest ? e.writes : e.reads).push_back(uint16_t(i));
    };
    auto operand = [&](const IROperand& op) {
        if (auto r = std::get_if<IRRegister>(&op)) read(r->index);
        if (auto r = std::get_if<IRRegisterRange>(&op)) range(*r, false);
        if (auto c = std::get_if<IRCapture>(&op); c && c->kind != IRCapture::Kind::UPVALUE) read(c->index);
    };
    const auto& o = insn.operands;
    switch (insn.op)
    {
    case IROp::CLOSEUPVALS: case IROp::JUMP: break;
    case IROp::CALL:
        operand(o[1]); operand(o[2]); range(std::get<IRRegisterRange>(o[0]), true); break;
    case IROp::VARARGS: range(std::get<IRRegisterRange>(o[0]), true); break;
    case IROp::NAMECALL:
        operand(o[1]);
        write(std::get<IRRegister>(o[0]).index); write(std::get<IRRegister>(o[0]).index + 1); break;
    case IROp::FORNPREP: case IROp::FORNLOOP: case IROp::FORGPREP: case IROp::FORGLOOP:
    {
        auto base = std::get<IRRegister>(o[0]).index;
        for (int i = 0; i < 3; ++i) read(base + i);
        if (insn.op == IROp::FORNLOOP) write(base + 2);
        if (insn.op == IROp::FORGLOOP)
        {
            write(base + 2);
            for (int i = 0; i < std::get<IRImmediate>(o[1]).value; ++i) write(base + 3 + i);
        }
        break;
    }
    case IROp::SETGLOBAL: case IROp::SETUPVAL: case IROp::SETTABLE: case IROp::SETLIST:
    case IROp::NEWCLASSMEMBER: case IROp::BRANCH: case IROp::RETURN:
        for (const auto& op : o) operand(op);
        break;
    default:
        write(std::get<IRRegister>(o[0]).index);
        for (size_t i = 1; i < o.size(); ++i) operand(o[i]);
        break;
    }
    for (auto* values : {&e.reads, &e.writes})
    {
        std::sort(values->begin(), values->end());
        values->erase(std::unique(values->begin(), values->end()), values->end());
        if (!values->empty() && values->back() >= stack) throw std::runtime_error("SSA: register out of range");
    }
    return e;
}

SSAFunction analyze(const IRFunction& ir, const CFGFunction& cfg)
{
    SSAFunction result;
    const auto n = uint32_t(ir.blocks.size());
    const uint16_t stack = ir.maxstacksize;
    result.blocks.resize(n);
    std::vector<std::vector<Effects>> access(n);
    std::vector<std::vector<bool>> uses(n, std::vector<bool>(stack)), defs = uses;
    auto value = [&](SSAValue::Kind kind, uint16_t reg, uint32_t b, uint32_t i) {
        auto id = SSAValueId(result.values.size());
        result.values.push_back({kind, reg, b, i}); return id;
    };
    for (uint16_t r = 0; r < stack; ++r)
        result.entryValues.push_back(value(r < ir.parameters.size() ? SSAValue::Kind::PARAMETER : SSAValue::Kind::UNDEFINED, r, 0, 0));
    for (uint32_t b = 0; b < n; ++b)
    {
        auto& block = result.blocks[b];
        block.liveIn.resize(stack); block.liveOut.resize(stack);
        for (uint32_t i = 0; i < ir.blocks[b].instructions.size(); ++i)
        {
            access[b].push_back(effects(ir.blocks[b].instructions[i], stack));
            block.instructions.emplace_back();
            for (auto r : access[b][i].reads) if (!defs[b][r]) uses[b][r] = true;
            for (auto r : access[b][i].writes)
            {
                defs[b][r] = true;
                block.instructions.back().definitions.push_back({r, value(SSAValue::Kind::INSTRUCTION, r, b, i)});
            }
        }
    }
    bool changed;
    do
    {
        changed = false;
        for (uint32_t b = n; b-- > 0;)
        {
            if (!cfg.blocks[b].reachable) continue;
            auto& block = result.blocks[b];
            for (uint16_t r = 0; r < stack; ++r)
            {
                bool out = false;
                for (auto s : cfg.blocks[b].successors) out = out || result.blocks[s].liveIn[r];
                bool in = uses[b][r] || (out && !defs[b][r]);
                changed |= in != block.liveIn[r] || out != block.liveOut[r];
                block.liveIn[r] = in; block.liveOut[r] = out;
            }
        }
    } while (changed);
    for (uint16_t r = 0; r < stack; ++r)
    {
        std::vector<uint32_t> work;
        std::vector<bool> placed(n), queued(n);
        for (uint32_t b = 0; b < n; ++b)
            if (cfg.blocks[b].reachable && defs[b][r]) { work.push_back(b); queued[b] = true; }
        while (!work.empty())
        {
            auto b = work.back(); work.pop_back();
            for (auto frontier : cfg.blocks[b].dominanceFrontier)
                if (!placed[frontier] && result.blocks[frontier].liveIn[r])
                {
                    result.blocks[frontier].phis.push_back({r, value(SSAValue::Kind::PHI, r, frontier, 0), {}});
                    placed[frontier] = true;
                    if (!queued[frontier]) { work.push_back(frontier); queued[frontier] = true; }
                }
        }
    }
    std::vector<std::vector<uint32_t>> children(n);
    for (uint32_t b = 1; b < n; ++b)
        if (cfg.blocks[b].reachable && cfg.blocks[b].immediateDominator >= 0)
            children[cfg.blocks[b].immediateDominator].push_back(b);
    std::vector<std::vector<SSAValueId>> stacks(stack);
    for (uint16_t r = 0; r < stack; ++r) stacks[r].push_back(result.entryValues[r]);
    std::function<void(uint32_t)> rename = [&](uint32_t b) {
        std::vector<uint16_t> pushed;
        auto push = [&](uint16_t r, SSAValueId v) { stacks[r].push_back(v); pushed.push_back(r); };
        for (auto& phi : result.blocks[b].phis) push(phi.reg, phi.result);
        for (uint32_t i = 0; i < access[b].size(); ++i)
        {
            auto& insn = result.blocks[b].instructions[i];
            for (auto r : access[b][i].reads) insn.uses.push_back({r, stacks[r].back()});
            for (auto d : insn.definitions) push(d.reg, d.value);
            // NEWCLOSURE writes its destination before processing captures, which
            // matters for local recursive functions capturing their own register.
            const auto& source = ir.blocks[b].instructions[i];
            if (source.op == IROp::CLOSURE || source.op == IROp::DUPCLOSURE)
                for (size_t c = 2; c < source.operands.size(); ++c)
                    if (auto capture = std::get<IRCapture>(source.operands[c]); capture.kind != IRCapture::Kind::UPVALUE)
                        for (auto& use : insn.uses)
                            if (use.reg == capture.index) use.value = stacks[use.reg].back();
        }
        for (auto s : cfg.blocks[b].successors)
            for (auto& phi : result.blocks[s].phis) phi.inputs.emplace_back(b, stacks[phi.reg].back());
        for (auto child : children[b]) rename(child);
        for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) stacks[*it].pop_back();
    };
    rename(0);
    for (auto& block : result.blocks)
        for (auto& phi : block.phis) std::sort(phi.inputs.begin(), phi.inputs.end());
    return result;
}
}

SSAContext buildSSA(CFGContext cfg)
{
    SSAContext result{std::move(cfg), {}};
    for (size_t f = 0; f < result.cfg.ir.functions.size(); ++f)
        result.functions.push_back(analyze(result.cfg.ir.functions[f], result.cfg.functions[f]));
    return result;
}
