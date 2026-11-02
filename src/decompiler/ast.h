#pragma once

#include "ssa.h"

#include <memory>

using ASTSymbolId = uint32_t;
struct ASTExpression;
struct ASTFunction;
using ASTExpr = std::shared_ptr<ASTExpression>;

struct ASTSymbol
{
    enum class Kind { VARIABLE, UPVALUE, FUNCTION };
    Kind kind = Kind::VARIABLE;
    uint32_t owner = 0;
    bool parameter = false;
    bool loopVariable = false;
    bool captured = false;
    std::optional<std::string> debugname;
};

struct ASTTableField { ASTExpr key; ASTExpr value; }; // A null key is a list field.

struct ASTExpression
{
    enum class Kind { LITERAL, SYMBOL, GLOBAL, UNARY, BINARY, INDEX, CALL, METHOD_CALL, FUNCTION, TABLE, VARARGS, CONDITIONAL };
    Kind kind;
    std::string text;
    ASTSymbolId symbol = 0;
    std::vector<ASTExpr> children;
    std::vector<ASTTableField> fields;
    std::shared_ptr<ASTFunction> function;
    bool multret = false;
    bool argumentsBeforeCallee = false; // CALL: retain the fast-call evaluation order.
};

struct ASTStatement
{
    enum class Kind { ASSIGN, LOCAL, EXPRESSION, IF, WHILE, REPEAT, NUMERIC_FOR, GENERIC_FOR, RETURN, BREAK, CONTINUE, SETLIST, CLASS, METHOD };
    explicit ASTStatement(Kind kind) : kind(kind) {}
    Kind kind;
    std::vector<ASTExpr> lhs;
    std::vector<ASTExpr> rhs;
    ASTExpr condition;
    std::vector<ASTSymbolId> names;
    std::vector<ASTStatement> body;
    std::vector<ASTStatement> alternative;
    int64_t firstIndex = 1;
    std::string text;
    std::vector<std::string> fields;
    bool open = false;
};
using ASTBlock = std::vector<ASTStatement>;

struct ASTFunction
{
    uint32_t id;
    std::vector<ASTSymbolId> parameters;
    bool isVararg;
    ASTBlock body;
};

struct ASTContext
{
    std::vector<ASTSymbol> symbols;
    std::shared_ptr<ASTFunction> entry;
};

ASTContext buildAST(const SSAContext& ssa);
std::string printAST(const ASTContext& ast);
