#pragma once

#include "ir.h"

struct CFGBlock
{
    bool reachable = false;
    std::vector<uint32_t> predecessors;
    std::vector<uint32_t> successors;
    int immediateDominator = -1;
    int immediatePostDominator = -1; // blocks.size() is the synthetic exit.
    std::vector<uint32_t> dominanceFrontier;
};

struct CFGLoop
{
    uint32_t header;
    std::vector<uint32_t> blocks;
    std::vector<uint32_t> latches;
    std::vector<uint32_t> exits;
};

struct CFGFunction
{
    std::vector<CFGBlock> blocks;
    std::vector<CFGLoop> loops;
};

struct CFGContext
{
    IRContext ir;
    std::vector<CFGFunction> functions;
};

CFGContext buildCFG(IRContext ir);
