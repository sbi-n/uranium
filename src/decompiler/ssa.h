#pragma once

#include "cfg.h"

using SSAValueId = uint32_t;

struct SSAValue
{
    enum class Kind { UNDEFINED, PARAMETER, INSTRUCTION, PHI };
    Kind kind;
    uint16_t reg;
    uint32_t block;
    uint32_t instruction;
};

struct SSARegisterValue
{
    uint16_t reg;
    SSAValueId value;
};

struct SSAInstruction
{
    std::vector<SSARegisterValue> uses;
    std::vector<SSARegisterValue> definitions;
};

struct SSAPhi
{
    uint16_t reg;
    SSAValueId result;
    std::vector<std::pair<uint32_t, SSAValueId>> inputs; // predecessor, value
};

struct SSABlock
{
    std::vector<SSAPhi> phis;
    std::vector<SSAInstruction> instructions;
    std::vector<bool> liveIn;
    std::vector<bool> liveOut;
};

struct SSAFunction
{
    std::vector<SSAValue> values;
    std::vector<SSAValueId> entryValues;
    std::vector<SSABlock> blocks;
};

struct SSAContext
{
    CFGContext cfg;
    std::vector<SSAFunction> functions;
};

// Pruned SSA: liveness, iterated dominance frontiers, then dominator-tree renaming.
SSAContext buildSSA(CFGContext cfg);
