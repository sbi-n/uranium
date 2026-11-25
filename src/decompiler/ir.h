#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

enum class IROp
{
    MOVE,
    ADD, SUB, MUL, DIV, IDIV, MOD, POW, AND, OR,
    CONCAT, NOT, MINUS, LENGTH,
    GETGLOBAL, SETGLOBAL, GETUPVAL, SETUPVAL, CLOSEUPVALS, GETIMPORT,
    GETTABLE, SETTABLE, NEWTABLE, DUPTABLE, SETLIST,
    CLOSURE, DUPCLOSURE, NAMECALL, CALL, VARARGS,
    JUMP, BRANCH, RETURN,
    FORNPREP, FORNLOOP, FORGPREP, FORGLOOP,
};

enum class IRCondition { TRUTHY, FALSY, EQ, LE, LT, NOT_EQ, NOT_LE, NOT_LT, PROTO_MISMATCH };

struct IRRegister { uint8_t index; };
struct IRUpvalue { uint8_t index; };
struct IRBlockRef { uint32_t ref; };
struct IRFunctionRef { uint32_t ref; };
struct IRTableRef { uint32_t ref; };
struct IRImmediate { int64_t value; }; // Instruction metadata, not a Luau value.

// count == -1 denotes all values through the dynamic stack top; 0 is an empty pack.
struct IRRegisterRange { uint16_t start; int count; };

struct IRCapture
{
    enum class Kind { VALUE, REFERENCE, UPVALUE };
    Kind kind;
    uint8_t index;
};

struct IRConstantNil {};
struct IRConstantBool { bool value; };
struct IRConstantNumber { double value; };
struct IRConstantString { std::string value; };
struct IRConstantVector { std::vector<double> value; };

using IROperand = std::variant<
    IRRegister, IRRegisterRange, IRUpvalue, IRBlockRef, IRFunctionRef,
    IRTableRef, IRImmediate, IRCondition, IRCapture,
    IRConstantNil, IRConstantBool, IRConstantNumber,
    IRConstantString, IRConstantVector>;

struct IRInstruction
{
    IROp op;
    uint32_t pc;
    std::vector<IROperand> operands;
    bool argumentsBeforeCallee = false; // CALL from a FASTCALL fallback sequence.
};

struct IRBlock
{
    uint32_t id;
    uint32_t startpc;
    uint32_t endpc; // Exclusive bytecode word offset; includes AUX and CAPTURE words.
    std::vector<IRInstruction> instructions;
};

struct IRParameter
{
    IRRegister reg;
    std::optional<std::string> debugname;
};

struct IRLocal
{
    IRRegister reg;
    uint32_t startpc, endpc;
    std::string debugname;
};

struct IRTable
{
    std::vector<std::pair<IROperand, IROperand>> entries;
};

struct IRFunction
{
    uint32_t id;
    std::optional<std::string> debugname;
    uint8_t maxstacksize;
    uint8_t upvalueCount;
    bool isVararg;
    bool native = false; // Declared with the @native attribute.
    std::vector<IRParameter> parameters;
    std::vector<IRLocal> locals;
    std::vector<IRTable> tables;
    std::vector<IRBlock> blocks;
};

struct IRContext
{
    std::vector<IRFunction> functions; // Function 0 is the entry point; refs are vector indices.
    bool nativeModule = false;         // Compiled with the --!native hot comment.
};

// Accepts compiler-produced Luau bytecode. Throws std::runtime_error on load/lift errors.
// The returned IR owns its data and remains valid after the temporary VM is closed.
IRContext lift(std::string_view bytecode);
std::string dump(const IRContext& context);
