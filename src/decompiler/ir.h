#pragma once

#include "vector"
#include "variant"

enum IROp
{
    ADD,
    SUB,
    MUL,
    DIV,
    IDIV,
    POW,

};

struct IRRegister
{
    uint8_t index;
};

struct IRConstantNil
{
};

struct IRConstantBool
{
    bool value;
};

struct IRConstantNumber
{
    double value;
};

struct IRConstantInteger
{
    long long int value;
};

struct IRConstantString
{
    std::string value;
};

/*
using IRValue = std::variant<
    IRRegister,
    IRBlock,

    IRConstantNil,
    IRConstantBool,
    IRConstantNumber,
    IRConstantInteger,
    IRConstantString>;
    */

using IROperand = std::variant<
    IRRegister,
    IRBlockRef,
    IRFunctionRef,

    IRConstantNil,
    IRConstantBool,
    IRConstantNumber,
    IRConstantInteger,
    IRConstantString>;

struct IRInstruction
{
    IROp op;
    std::vector<IROperand> operands;
};

struct IRBlock
{
    uint32_t id;
    uint32_t startpc;
    uint16_t endpc;
    std::vector<IRInstruction> instructions;
};

struct IRBlockRef
{
    uint32_t ref;
};

struct IRParameter
{
};

struct IRFunction
{
    uint32_t id;
    std::optional<std::string> debugname;
    std::vector<IRParameter> parameters;
    std::vector<IRBlock> blocks;
};

struct IRFunctionRef
{
    uint32_t ref;
};

// 이걸 구조체로 해야할지, 아니면 전부를 객체로 해야할지 모르겟음. 솔직히 객체는 좀 아닌것 같은데.
struct IRContext
{
    std::vector<IRFunction> functions;
};

IRContext *lift(char *bytecode, int size);
std::string dump(IRContext *context);