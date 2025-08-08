#pragma once

#include "ast/astnode.hh"
#include <memory>
#include <string>

namespace lona {

// 编译器策略基类
class CompilerStrategy {
public:
    virtual ~CompilerStrategy() = default;
    virtual void execute() = 0;
};

// 词法分析策略
class LexerStrategy : public CompilerStrategy {
public:
    explicit LexerStrategy(const std::string& source) : source_code(source) {}
    void execute() override;

private:
    std::string source_code;
};

// 语法分析策略
class ParserStrategy : public CompilerStrategy {
public:
    void execute() override;
};

// 语义分析策略
class SemanticAnalysisStrategy : public CompilerStrategy {
public:
    explicit SemanticAnalysisStrategy(AstNode* ast) : ast_root(ast) {}
    void execute() override;

private:
    AstNode* ast_root;
};

// 代码生成策略
class CodeGenStrategy : public CompilerStrategy {
public:
    CodeGenStrategy(AstNode* ast, std::ostream& os)
        : ast_root(ast), output_stream(os) {}
    void execute() override;

private:
    AstNode* ast_root;
    std::ostream& output_stream;
};

}  // namespace lona