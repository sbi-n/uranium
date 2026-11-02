#include "cfg.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace
{
using Bits = std::vector<bool>;

void appendUnique(std::vector<uint32_t>& values, uint32_t value)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

CFGFunction analyze(const IRFunction& function)
{
    const uint32_t n = uint32_t(function.blocks.size());
    if (!n) throw std::runtime_error("CFG: function has no entry block");
    CFGFunction cfg;
    cfg.blocks.resize(n);
    for (uint32_t b = 0; b < n; ++b)
    {
        const auto& block = function.blocks[b];
        if (block.id != b || block.instructions.empty()) throw std::runtime_error("CFG: invalid block");
        const auto& term = block.instructions.back();
        for (const auto& operand : term.operands)
            if (auto ref = std::get_if<IRBlockRef>(&operand))
            {
                if (ref->ref >= n) throw std::runtime_error("CFG: invalid successor");
                // CMPPROTO guards an inlined specialization. Its ordinary call fallback
                // is valid for every function identity and doesn't require VM internals.
                if (term.op == IROp::BRANCH && std::get<IRCondition>(term.operands[0]) == IRCondition::PROTO_MISMATCH
                    && ref->ref != std::get<IRBlockRef>(term.operands[term.operands.size() - 2]).ref)
                    continue;
                appendUnique(cfg.blocks[b].successors, ref->ref);
                appendUnique(cfg.blocks[ref->ref].predecessors, b);
            }
    }
    std::vector<uint32_t> work{0};
    while (!work.empty())
    {
        uint32_t b = work.back();
        work.pop_back();
        if (cfg.blocks[b].reachable) continue;
        cfg.blocks[b].reachable = true;
        for (auto s : cfg.blocks[b].successors) work.push_back(s);
    }
    Bits reachable(n, false);
    for (uint32_t b = 0; b < n; ++b) reachable[b] = cfg.blocks[b].reachable;
    std::vector<Bits> dom(n, reachable);
    dom[0] = Bits(n, false);
    dom[0][0] = true;
    bool changed;
    do
    {
        changed = false;
        for (uint32_t b = 1; b < n; ++b)
        {
            if (!reachable[b]) continue;
            Bits next = reachable;
            for (auto p : cfg.blocks[b].predecessors)
                if (reachable[p]) for (uint32_t i = 0; i < n; ++i) next[i] = next[i] && dom[p][i];
            next[b] = true;
            if (next != dom[b]) { dom[b] = std::move(next); changed = true; }
        }
    } while (changed);
    std::vector<size_t> depth(n);
    for (uint32_t b = 0; b < n; ++b) depth[b] = std::count(dom[b].begin(), dom[b].end(), true);
    for (uint32_t b = 1; b < n; ++b)
        if (reachable[b])
            for (uint32_t d = 0; d < n; ++d)
                if (b != d && dom[b][d])
                {
                    int previous = cfg.blocks[b].immediateDominator;
                    if (previous < 0 || depth[d] > depth[previous]) cfg.blocks[b].immediateDominator = int(d);
                }
    for (uint32_t b = 0; b < n; ++b)
        if (reachable[b])
            for (auto p : cfg.blocks[b].predecessors)
            {
                if (!reachable[p]) continue;
                for (int runner = int(p); runner >= 0 && runner != cfg.blocks[b].immediateDominator;
                     runner = cfg.blocks[runner].immediateDominator)
                    appendUnique(cfg.blocks[runner].dominanceFrontier, b);
            }

    // Blocks in non-terminating SCCs have no real postdominator. Giving each
    // such block an additional synthetic exit keeps the result conservative.
    Bits reachesExit(n, false);
    for (uint32_t b = 0; b < n; ++b)
        if (reachable[b] && cfg.blocks[b].successors.empty()) work.push_back(b);
    while (!work.empty())
    {
        uint32_t b = work.back(); work.pop_back();
        if (reachesExit[b]) continue;
        reachesExit[b] = true;
        for (auto p : cfg.blocks[b].predecessors) if (reachable[p]) work.push_back(p);
    }
    Bits universe(n + 1, false);
    for (uint32_t b = 0; b < n; ++b) universe[b] = reachable[b];
    universe[n] = true;
    std::vector<Bits> post(n + 1, universe);
    post[n] = Bits(n + 1, false); post[n][n] = true;
    do
    {
        changed = false;
        for (uint32_t b = n; b-- > 0;)
        {
            if (!reachable[b]) continue;
            Bits next = universe;
            if (cfg.blocks[b].successors.empty() || !reachesExit[b]) next = post[n];
            for (auto s : cfg.blocks[b].successors)
                for (uint32_t i = 0; i <= n; ++i) next[i] = next[i] && post[s][i];
            next[b] = true;
            if (next != post[b]) { post[b] = std::move(next); changed = true; }
        }
    } while (changed);
    depth.resize(n + 1);
    for (uint32_t b = 0; b <= n; ++b) depth[b] = std::count(post[b].begin(), post[b].end(), true);
    for (uint32_t b = 0; b < n; ++b)
        if (reachable[b])
            for (uint32_t d = 0; d <= n; ++d)
                if (b != d && post[b][d])
                {
                    int previous = cfg.blocks[b].immediatePostDominator;
                    if (previous < 0 || depth[d] > depth[previous]) cfg.blocks[b].immediatePostDominator = int(d);
                }

    std::map<uint32_t, CFGLoop> loops;
    for (uint32_t b = 0; b < n; ++b)
        if (reachable[b])
            for (auto h : cfg.blocks[b].successors)
                if (dom[b][h])
                {
                    auto& loop = loops.try_emplace(h, CFGLoop{h, {h}, {}, {}}).first->second;
                    appendUnique(loop.latches, b);
                    std::set<uint32_t> members{h};
                    work = {b};
                    while (!work.empty())
                    {
                        auto node = work.back(); work.pop_back();
                        if (!members.insert(node).second) continue;
                        for (auto p : cfg.blocks[node].predecessors) if (reachable[p]) work.push_back(p);
                    }
                    for (auto node : members) appendUnique(loop.blocks, node);
                }
    for (auto& [header, loop] : loops)
    {
        std::sort(loop.blocks.begin(), loop.blocks.end());
        for (auto b : loop.blocks)
            for (auto s : cfg.blocks[b].successors)
                if (!std::binary_search(loop.blocks.begin(), loop.blocks.end(), s)) appendUnique(loop.exits, s);
        std::sort(loop.exits.begin(), loop.exits.end());
        cfg.loops.push_back(std::move(loop));
    }
    return cfg;
}
}

CFGContext buildCFG(IRContext ir)
{
    CFGContext result{std::move(ir), {}};
    for (const auto& function : result.ir.functions) result.functions.push_back(analyze(function));
    return result;
}
