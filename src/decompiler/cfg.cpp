#include "cfg.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace
{
void appendUnique(std::vector<uint32_t>& values, uint32_t value)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

// Cooper, Harvey and Kennedy, "A Simple, Fast Dominance Algorithm". `order`
// holds the nodes reachable from order[0] in reverse postorder.
std::vector<int> dominators(const std::vector<uint32_t>& order, const std::vector<std::vector<uint32_t>>& predecessors)
{
    const size_t size = predecessors.size();
    std::vector<int> rpo(size, -1), idom(size, -1);
    for (size_t i = 0; i < order.size(); ++i) rpo[order[i]] = int(i);
    idom[order[0]] = int(order[0]);
    auto intersect = [&](uint32_t a, uint32_t b) {
        while (a != b)
        {
            while (rpo[a] > rpo[b]) a = uint32_t(idom[a]);
            while (rpo[b] > rpo[a]) b = uint32_t(idom[b]);
        }
        return a;
    };
    for (bool changed = true; changed;)
    {
        changed = false;
        for (size_t i = 1; i < order.size(); ++i)
        {
            uint32_t node = order[i];
            int next = -1;
            for (auto pred : predecessors[node])
                if (rpo[pred] >= 0 && idom[pred] >= 0) next = next < 0 ? int(pred) : int(intersect(pred, uint32_t(next)));
            if (next != idom[node]) { idom[node] = next; changed = true; }
        }
    }
    idom[order[0]] = -1;
    return idom;
}

std::vector<uint32_t> reversePostorder(uint32_t root, const std::vector<std::vector<uint32_t>>& successors)
{
    std::vector<uint32_t> order;
    std::vector<bool> seen(successors.size());
    std::vector<std::pair<uint32_t, size_t>> stack{{root, 0}};
    seen[root] = true;
    while (!stack.empty())
    {
        auto& [node, next] = stack.back();
        if (next < successors[node].size())
        {
            uint32_t successor = successors[node][next++];
            if (!seen[successor]) { seen[successor] = true; stack.emplace_back(successor, 0); }
            continue;
        }
        order.push_back(node);
        stack.pop_back();
    }
    std::reverse(order.begin(), order.end());
    return order;
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

    // Dominators over the blocks reachable from the entry.
    std::vector<std::vector<uint32_t>> forward(n), backward(n);
    for (uint32_t b = 0; b < n; ++b) forward[b] = cfg.blocks[b].successors;
    const auto order = reversePostorder(0, forward);
    for (auto b : order)
    {
        cfg.blocks[b].reachable = true;
        for (auto s : cfg.blocks[b].successors) backward[s].push_back(b);
    }
    const auto idom = dominators(order, backward);
    for (auto b : order) cfg.blocks[b].immediateDominator = idom[b];
    for (uint32_t b = 0; b < n; ++b)
        for (auto p : cfg.blocks[b].predecessors)
        {
            if (!cfg.blocks[b].reachable || !cfg.blocks[p].reachable) continue;
            for (int runner = int(p); runner >= 0 && runner != cfg.blocks[b].immediateDominator;
                 runner = cfg.blocks[runner].immediateDominator)
                appendUnique(cfg.blocks[runner].dominanceFrontier, b);
        }
    // Preorder intervals of the dominator tree answer "a dominates b" in O(1).
    std::vector<std::vector<uint32_t>> children(n);
    for (auto b : order) if (idom[b] >= 0) children[idom[b]].push_back(b);
    std::vector<uint32_t> enter(n), leave(n);
    {
        uint32_t clock = 0;
        std::vector<std::pair<uint32_t, size_t>> stack{{0, 0}};
        enter[0] = clock++;
        while (!stack.empty())
        {
            auto& [node, next] = stack.back();
            if (next < children[node].size())
            {
                uint32_t child = children[node][next++];
                enter[child] = clock++;
                stack.emplace_back(child, 0);
                continue;
            }
            leave[node] = clock++;
            stack.pop_back();
        }
    }
    auto dominates = [&](uint32_t a, uint32_t b) { return enter[a] <= enter[b] && leave[b] <= leave[a]; };

    // Postdominators use a synthetic exit (index n). Blocks in non-terminating
    // SCCs have no real postdominator; an extra edge from each such block to
    // the exit keeps the result conservative.
    std::vector<bool> reachesExit(n);
    std::vector<uint32_t> work;
    for (auto b : order) if (cfg.blocks[b].successors.empty()) work.push_back(b);
    while (!work.empty())
    {
        uint32_t b = work.back(); work.pop_back();
        if (reachesExit[b]) continue;
        reachesExit[b] = true;
        for (auto p : cfg.blocks[b].predecessors) if (cfg.blocks[p].reachable) work.push_back(p);
    }
    std::vector<std::vector<uint32_t>> reverse(n + 1), reversePredecessors(n + 1);
    for (auto b : order)
    {
        if (cfg.blocks[b].successors.empty() || !reachesExit[b])
        {
            reverse[n].push_back(b);
            reversePredecessors[b].push_back(n);
        }
        for (auto s : cfg.blocks[b].successors)
        {
            reverse[s].push_back(b);
            reversePredecessors[b].push_back(s);
        }
    }
    const auto ipdom = dominators(reversePostorder(n, reverse), reversePredecessors);
    for (auto b : order) cfg.blocks[b].immediatePostDominator = ipdom[b];

    std::map<uint32_t, CFGLoop> loops;
    std::vector<uint32_t> inLoop(n, UINT32_MAX); // Last back edge that collected the block.
    uint32_t backEdge = 0;
    for (uint32_t b = 0; b < n; ++b)
        if (cfg.blocks[b].reachable)
            for (auto h : cfg.blocks[b].successors)
                if (dominates(h, b))
                {
                    auto& loop = loops.try_emplace(h, CFGLoop{h, {h}, {}, {}}).first->second;
                    appendUnique(loop.latches, b);
                    // Blocks that reach the latch without passing through the header.
                    ++backEdge;
                    inLoop[h] = backEdge;
                    work = {b};
                    while (!work.empty())
                    {
                        auto node = work.back(); work.pop_back();
                        if (inLoop[node] == backEdge) continue;
                        inLoop[node] = backEdge;
                        loop.blocks.push_back(node);
                        for (auto p : cfg.blocks[node].predecessors) if (cfg.blocks[p].reachable) work.push_back(p);
                    }
                }
    for (auto& [header, loop] : loops)
    {
        std::sort(loop.blocks.begin(), loop.blocks.end());
        loop.blocks.erase(std::unique(loop.blocks.begin(), loop.blocks.end()), loop.blocks.end());
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
    result.functions.reserve(result.ir.functions.size());
    for (const auto& function : result.ir.functions) result.functions.push_back(analyze(function));
    return result;
}
