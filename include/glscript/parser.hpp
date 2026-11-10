#pragma once

#include "glscript/ast.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/source.hpp"
#include "glscript/token.hpp"

#include <memory>
#include <vector>

namespace glscript {

class Parser {
public:
    Parser(const SourceFile& file, DiagnosticSink& diagnostics);
    Parser(const SourceFile& file, DiagnosticSink& diagnostics, std::uint32_t begin,
           std::uint32_t end);

    ast::ModuleUnit parse_module();
    ast::ExprPtr parse_expression_only();

private:
    const SourceFile& file_;
    DiagnosticSink& diagnostics_;
    std::vector<Token> tokens_;
    std::size_t index_ = 0;
    bool suppress_errors_ = false;
    int loop_depth_ = 0;

    const Token& current() const;
    const Token& previous() const;
    const Token& peek(std::size_t offset = 1) const;
    bool at(TokenKind kind) const;
    bool at_end() const;
    bool match(TokenKind kind);
    const Token& advance();
    const Token& consume(TokenKind kind, const std::string& message);

    SourceSpan span_from(const Token& begin) const;
    SourceSpan span_between(const Token& begin, const Token& end) const;

    void error(const Token& token, std::string message);
    void error(SourceSpan span, std::string message);
    void warn(const Token& token, std::string message);
    void synchronize_top_level();
    void synchronize_statement();

    ast::Attribute parse_attribute();
    std::vector<ast::Attribute> parse_attributes();
    ast::Visibility parse_visibility();
    void parse_modifiers(ast::Visibility& visibility, std::vector<ast::Attribute>& attributes);

    std::vector<std::string> parse_path();
    ast::TypeExprPtr parse_type();
    std::vector<ast::TypeExprPtr> parse_type_arguments();
    std::vector<ast::Parameter> parse_parameter_list();
    ast::Parameter parse_parameter();

    ast::DeclPtr parse_import();
    ast::DeclPtr parse_class(ast::Visibility visibility, std::vector<ast::Attribute> attributes);
    ast::DeclPtr parse_enum(ast::Visibility visibility, std::vector<ast::Attribute> attributes);
    ast::DeclPtr parse_effect(ast::Visibility visibility, std::vector<ast::Attribute> attributes);
    ast::DeclPtr parse_handler_decl(ast::Visibility visibility,
                                    std::vector<ast::Attribute> attributes);
    ast::DeclPtr parse_function_decl(ast::Visibility visibility,
                                     std::vector<ast::Attribute> attributes);

    std::unique_ptr<ast::FieldDecl> parse_field(ast::Visibility visibility,
                                                std::vector<ast::Attribute> attributes);
    std::unique_ptr<ast::FunctionDecl> parse_method(ast::Visibility visibility,
                                                    std::vector<ast::Attribute> attributes);
    std::unique_ptr<ast::FunctionDecl> parse_effect_operation(
        ast::Visibility visibility, std::vector<ast::Attribute> attributes);

    std::vector<ast::HandlerCase> parse_handler_cases();
    ast::HandlerCase parse_handler_case();

    ast::StmtPtr parse_block();
    ast::StmtPtr parse_statement();
    ast::StmtPtr parse_var_decl();
    ast::StmtPtr parse_if();
    ast::StmtPtr parse_while();
    std::unique_ptr<ast::VarDeclStmt> try_parse_var_decl();

    ast::ExprPtr parse_expression();
    ast::ExprPtr parse_logical_or();
    ast::ExprPtr parse_logical_and();
    ast::ExprPtr parse_equality();
    ast::ExprPtr parse_comparison();
    ast::ExprPtr parse_additive();
    ast::ExprPtr parse_multiplicative();
    ast::ExprPtr parse_unary();
    ast::ExprPtr parse_postfix();
    ast::ExprPtr parse_primary();
    ast::ExprPtr parse_resume();
    ast::ExprPtr parse_handle_expr();
    ast::ExprPtr parse_lambda();
    ast::ExprPtr parse_parenthesized();

    bool is_lambda_ahead() const;
    bool looks_like_type_argument_call() const;
};

} // namespace glscript
