#include "glscript/parser.hpp"

#include "glscript/lexer.hpp"

#include <string>
#include <utility>

namespace glscript {
namespace {

template <typename T>
std::unique_ptr<T> make_node(SourceSpan span) {
    auto node = std::make_unique<T>();
    node->span = span;
    return node;
}

std::string join_path(const std::vector<std::string>& path) {
    std::string result;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) {
            result += "::";
        }
        result += path[i];
    }
    return result;
}

} // namespace

Parser::Parser(const SourceFile& file, DiagnosticSink& diagnostics)
    : Parser(file, diagnostics, 0, static_cast<std::uint32_t>(file.text().size())) {}

Parser::Parser(const SourceFile& file, DiagnosticSink& diagnostics, std::uint32_t begin,
               std::uint32_t end)
    : file_(file), diagnostics_(diagnostics) {
    Lexer lexer(file, diagnostics, begin, end);
    tokens_ = lexer.tokenize();
}

const Token& Parser::current() const {
    return tokens_[std::min(index_, tokens_.size() - 1)];
}

const Token& Parser::previous() const {
    return tokens_[index_ > 0 ? index_ - 1 : 0];
}

const Token& Parser::peek(std::size_t offset) const {
    const auto target = index_ + offset;
    return tokens_[std::min(target, tokens_.size() - 1)];
}

bool Parser::at(TokenKind kind) const {
    return current().kind == kind;
}

bool Parser::at_end() const {
    return current().kind == TokenKind::Eof;
}

bool Parser::match(TokenKind kind) {
    if (!at(kind)) {
        return false;
    }
    ++index_;
    return true;
}

const Token& Parser::advance() {
    if (!at_end()) {
        ++index_;
    }
    return previous();
}

const Token& Parser::consume(TokenKind kind, const std::string& message) {
    if (at(kind)) {
        return advance();
    }
    error(current(), message);
    return current();
}

SourceSpan Parser::span_from(const Token& begin) const {
    const auto end = previous().span.end >= begin.span.begin ? previous().span.end
                                                             : begin.span.end;
    return file_.span(begin.span.begin, end);
}

SourceSpan Parser::span_between(const Token& begin, const Token& end) const {
    return file_.span(begin.span.begin, end.span.end);
}

void Parser::error(const Token& token, std::string message) {
    if (!suppress_errors_) {
        diagnostics_.error(token.span, std::move(message));
    }
}

void Parser::error(SourceSpan span, std::string message) {
    if (!suppress_errors_) {
        diagnostics_.error(span, std::move(message));
    }
}

void Parser::warn(const Token& token, std::string message) {
    if (!suppress_errors_) {
        diagnostics_.warning(token.span, std::move(message));
    }
}

void Parser::synchronize_top_level() {
    while (!at_end()) {
        switch (current().kind) {
        case TokenKind::KwImport:
        case TokenKind::KwClass:
        case TokenKind::KwEnum:
        case TokenKind::KwEffect:
        case TokenKind::KwHandle:
        case TokenKind::KwFunction:
        case TokenKind::KwExport:
        case TokenKind::KwPub:
        case TokenKind::Hash:
            return;
        default:
            advance();
            break;
        }
    }
}

void Parser::synchronize_statement() {
    while (!at_end()) {
        if (match(TokenKind::Semicolon)) {
            return;
        }
        switch (current().kind) {
        case TokenKind::RBrace:
        case TokenKind::KwIf:
        case TokenKind::KwElse:
        case TokenKind::KwWhile:
        case TokenKind::KwReturn:
        case TokenKind::KwBreak:
        case TokenKind::KwContinue:
            return;
        default:
            advance();
            break;
        }
    }
}

ast::Attribute Parser::parse_attribute() {
    const auto begin = consume(TokenKind::Hash, "expected '#'");
    consume(TokenKind::LBracket, "expected '[' after '#'");
    const auto name = consume(TokenKind::Identifier, "expected attribute name");
    consume(TokenKind::RBracket, "expected ']' to close attribute");
    return ast::Attribute{name.text, span_from(begin)};
}

std::vector<ast::Attribute> Parser::parse_attributes() {
    std::vector<ast::Attribute> attributes;
    while (at(TokenKind::Hash)) {
        attributes.push_back(parse_attribute());
    }
    return attributes;
}

ast::Visibility Parser::parse_visibility() {
    ast::Visibility visibility = ast::Visibility::Local;

    while (at(TokenKind::KwExport) || at(TokenKind::KwPub)) {
        const bool is_export = at(TokenKind::KwExport);
        const auto token = advance();
        const auto next = is_export ? ast::Visibility::Export : ast::Visibility::Pub;

        if (visibility == ast::Visibility::Local) {
            visibility = next;
        } else if (visibility == next) {
            warn(token, "duplicate visibility modifier");
        } else {
            visibility = ast::Visibility::PubExport;
        }
    }

    return visibility;
}

void Parser::parse_modifiers(ast::Visibility& visibility,
                             std::vector<ast::Attribute>& attributes) {
    while (true) {
        if (at(TokenKind::Hash)) {
            attributes.push_back(parse_attribute());
            continue;
        }
        if (at(TokenKind::KwExport) || at(TokenKind::KwPub)) {
            const auto next = parse_visibility();
            if (visibility == ast::Visibility::Local) {
                visibility = next;
            } else if (visibility == next) {
                warn(current(), "duplicate visibility modifier");
            } else {
                visibility = ast::Visibility::PubExport;
            }
            continue;
        }
        break;
    }
}

std::vector<std::string> Parser::parse_path() {
    std::vector<std::string> path;
    const auto first = consume(TokenKind::Identifier, "expected identifier");
    if (first.kind != TokenKind::Identifier) {
        return path;
    }
    path.push_back(first.text);

    while (at(TokenKind::ColonColon) && peek().kind == TokenKind::Identifier) {
        advance();
        path.push_back(advance().text);
    }

    return path;
}

ast::TypeExprPtr Parser::parse_type() {
    if (match(TokenKind::KwDynamic)) {
        return make_node<ast::DynamicTypeExpr>(previous().span);
    }

    if (!at(TokenKind::Identifier)) {
        const auto token = current();
        error(token, "expected type");
        return make_node<ast::ErrorTypeExpr>(token.span);
    }

    const auto begin = current();
    auto path = parse_path();

    auto type = make_node<ast::NamedTypeExpr>(span_from(begin));
    type->path = std::move(path);

    if (at(TokenKind::Less)) {
        type->arguments = parse_type_arguments();
        type->span = span_from(begin);
    }

    return type;
}

std::vector<ast::TypeExprPtr> Parser::parse_type_arguments() {
    std::vector<ast::TypeExprPtr> arguments;
    consume(TokenKind::Less, "expected '<'");

    if (!at(TokenKind::Greater)) {
        while (true) {
            arguments.push_back(parse_type());
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
    }

    consume(TokenKind::Greater, "expected '>' to close type arguments");
    return arguments;
}

ast::Parameter Parser::parse_parameter() {
    const auto begin = current();
    const auto name = consume(TokenKind::Identifier, "expected parameter name");
    consume(TokenKind::Colon, "expected ':' after parameter name");

    ast::Parameter parameter;
    parameter.name = name.text;
    parameter.type = parse_type();
    parameter.span = span_from(begin);
    return parameter;
}

std::vector<ast::Parameter> Parser::parse_parameter_list() {
    std::vector<ast::Parameter> parameters;
    consume(TokenKind::LParen, "expected '(' before parameter list");

    if (!at(TokenKind::RParen)) {
        while (true) {
            parameters.push_back(parse_parameter());
            if (!match(TokenKind::Comma)) {
                break;
            }
            if (at(TokenKind::RParen)) {
                break;
            }
        }
    }

    consume(TokenKind::RParen, "expected ')' to close parameter list");
    return parameters;
}

ast::DeclPtr Parser::parse_import() {
    const auto begin = consume(TokenKind::KwImport, "expected 'import'");

    auto declaration = make_node<ast::ImportDecl>(begin.span);

    std::vector<std::string> segments;
    const auto first = consume(TokenKind::Identifier, "expected module name after 'import'");
    if (first.kind != TokenKind::Identifier) {
        synchronize_top_level();
        return declaration;
    }
    segments.push_back(first.text);

    bool items_form = false;
    while (match(TokenKind::ColonColon)) {
        if (at(TokenKind::LBrace)) {
            items_form = true;
            break;
        }
        const auto segment = consume(TokenKind::Identifier, "expected module or symbol name");
        if (segment.kind != TokenKind::Identifier) {
            break;
        }
        segments.push_back(segment.text);
    }

    if (items_form) {
        declaration->module_path = segments;
        consume(TokenKind::LBrace, "expected '{'");
        while (!at(TokenKind::RBrace) && !at_end()) {
            auto item = consume(TokenKind::Identifier, "expected imported symbol name");
            if (item.kind == TokenKind::Identifier) {
                declaration->items.push_back(ast::ImportItem{item.text, item.span});
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        consume(TokenKind::RBrace, "expected '}' after imported symbols");
    } else if (segments.size() == 1) {
        declaration->module_path = segments;
        declaration->wildcard = true;
    } else {
        declaration->module_path.assign(segments.begin(), segments.end() - 1);
        declaration->items.push_back(ast::ImportItem{segments.back(), first.span});
    }

    consume(TokenKind::Semicolon, "expected ';' after import");
    declaration->span = span_from(begin);
    return declaration;
}

ast::DeclPtr Parser::parse_class(ast::Visibility visibility,
                                 std::vector<ast::Attribute> attributes) {
    const auto begin = consume(TokenKind::KwClass, "expected 'class'");

    auto declaration = make_node<ast::ClassDecl>(begin.span);
    declaration->visibility = visibility;
    declaration->attributes = std::move(attributes);

    const auto name = consume(TokenKind::Identifier, "expected class name");
    declaration->name = name.text;

    if (match(TokenKind::Less)) {
        while (!at(TokenKind::Greater) && !at_end()) {
            const auto parameter = consume(TokenKind::Identifier, "expected type parameter name");
            if (parameter.kind == TokenKind::Identifier) {
                declaration->type_parameters.push_back(parameter.text);
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        consume(TokenKind::Greater, "expected '>' after type parameters");
    }

    if (match(TokenKind::KwExtends)) {
        auto base = parse_path();
        if (!base.empty()) {
            declaration->base = join_path(base);
        }
    }

    consume(TokenKind::LBrace, "expected '{' to start class body");

    while (!at(TokenKind::RBrace) && !at_end()) {
        auto member_visibility = ast::Visibility::Local;
        std::vector<ast::Attribute> member_attributes;
        parse_modifiers(member_visibility, member_attributes);

        if (at(TokenKind::KwFunction)) {
            declaration->methods.push_back(
                parse_method(member_visibility, std::move(member_attributes)));
            continue;
        }

        if (at(TokenKind::KwFn)) {
            error(current(), "'fn' is not supported; use 'function'");
            advance();
            declaration->methods.push_back(
                parse_method(member_visibility, std::move(member_attributes)));
            continue;
        }

        declaration->fields.push_back(
            parse_field(member_visibility, std::move(member_attributes)));
    }

    consume(TokenKind::RBrace, "expected '}' to close class body");
    match(TokenKind::Semicolon);
    declaration->span = span_from(begin);
    return declaration;
}

std::unique_ptr<ast::FieldDecl> Parser::parse_field(ast::Visibility visibility,
                                                    std::vector<ast::Attribute> attributes) {
    const auto begin = current();
    auto field = make_node<ast::FieldDecl>(begin.span);
    field->visibility = visibility;
    field->attributes = std::move(attributes);
    field->type = parse_type();

    const auto name = consume(TokenKind::Identifier, "expected field name");
    field->name = name.text;

    if (match(TokenKind::Equal)) {
        field->initializer = parse_expression();
    }

    consume(TokenKind::Semicolon, "expected ';' after field declaration");
    field->span = span_from(begin);
    return field;
}

std::unique_ptr<ast::FunctionDecl> Parser::parse_method(
    ast::Visibility visibility, std::vector<ast::Attribute> attributes) {
    const auto begin = consume(TokenKind::KwFunction, "expected 'function'");

    auto method = make_node<ast::FunctionDecl>(begin.span);
    method->visibility = visibility;
    method->attributes = std::move(attributes);

    const auto name = consume(TokenKind::Identifier, "expected method name");
    method->name = name.text;

    if (match(TokenKind::Less)) {
        while (!at(TokenKind::Greater) && !at_end()) {
            const auto parameter = consume(TokenKind::Identifier, "expected type parameter name");
            if (parameter.kind == TokenKind::Identifier) {
                method->type_parameters.push_back(parameter.text);
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        consume(TokenKind::Greater, "expected '>' after type parameters");
    }

    method->parameters = parse_parameter_list();

    if (match(TokenKind::Arrow)) {
        method->return_type = parse_type();
    }

    if (at(TokenKind::Semicolon)) {
        advance();
    } else {
        method->body = parse_block();
    }

    method->span = span_from(begin);
    return method;
}

std::unique_ptr<ast::FunctionDecl> Parser::parse_effect_operation(
    ast::Visibility visibility, std::vector<ast::Attribute> attributes) {
    const auto begin = consume(TokenKind::KwFunction, "expected 'function'");

    auto operation = make_node<ast::FunctionDecl>(begin.span);
    operation->visibility = visibility;
    operation->attributes = std::move(attributes);
    operation->is_effect_operation = true;
    operation->body = nullptr;

    const auto name = consume(TokenKind::Identifier, "expected operation name");
    operation->name = name.text;

    if (match(TokenKind::Less)) {
        while (!at(TokenKind::Greater) && !at_end()) {
            const auto parameter = consume(TokenKind::Identifier, "expected type parameter name");
            if (parameter.kind == TokenKind::Identifier) {
                operation->type_parameters.push_back(parameter.text);
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        consume(TokenKind::Greater, "expected '>' after type parameters");
    }

    operation->parameters = parse_parameter_list();

    if (match(TokenKind::Arrow)) {
        operation->return_type = parse_type();
    }

    consume(TokenKind::Semicolon, "expected ';' after effect operation");
    operation->span = span_from(begin);
    return operation;
}

ast::DeclPtr Parser::parse_effect(ast::Visibility visibility,
                                  std::vector<ast::Attribute> attributes) {
    const auto begin = consume(TokenKind::KwEffect, "expected 'effect'");

    auto declaration = make_node<ast::EffectDecl>(begin.span);
    declaration->visibility = visibility;
    declaration->attributes = std::move(attributes);

    const auto name = consume(TokenKind::Identifier, "expected effect name");
    declaration->name = name.text;

    consume(TokenKind::LBrace, "expected '{' to start effect body");

    while (!at(TokenKind::RBrace) && !at_end()) {
        auto operation_visibility = ast::Visibility::Local;
        std::vector<ast::Attribute> operation_attributes;
        parse_modifiers(operation_visibility, operation_attributes);

        if (at(TokenKind::KwFunction)) {
            declaration->operations.push_back(
                parse_effect_operation(operation_visibility, std::move(operation_attributes)));
            continue;
        }

        if (at(TokenKind::KwFn)) {
            error(current(), "'fn' is not supported; use 'function'");
            advance();
            declaration->operations.push_back(
                parse_effect_operation(operation_visibility, std::move(operation_attributes)));
            continue;
        }

        error(current(), "expected effect operation");
        synchronize_top_level();
    }

    consume(TokenKind::RBrace, "expected '}' to close effect body");
    match(TokenKind::Semicolon);
    declaration->span = span_from(begin);
    return declaration;
}

ast::DeclPtr Parser::parse_enum(ast::Visibility visibility,
                                std::vector<ast::Attribute> attributes) {
    const auto begin = consume(TokenKind::KwEnum, "expected 'enum'");

    auto declaration = make_node<ast::EnumDecl>(begin.span);
    declaration->visibility = visibility;
    declaration->attributes = std::move(attributes);

    const auto name = consume(TokenKind::Identifier, "expected enum name");
    declaration->name = name.text;

    if (match(TokenKind::Less)) {
        while (!at(TokenKind::Greater) && !at_end()) {
            const auto parameter = consume(TokenKind::Identifier, "expected type parameter name");
            if (parameter.kind == TokenKind::Identifier) {
                declaration->type_parameters.push_back(parameter.text);
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        consume(TokenKind::Greater, "expected '>' after type parameters");
    }

    consume(TokenKind::LBrace, "expected '{' to start enum body");

    while (!at(TokenKind::RBrace) && !at_end()) {
        const auto variant_begin = current();
        const auto variant_name = consume(TokenKind::Identifier, "expected enum variant name");
        if (variant_name.kind != TokenKind::Identifier) {
            advance();
            continue;
        }

        ast::EnumVariantDecl variant;
        variant.name = variant_name.text;

        if (match(TokenKind::LParen)) {
            if (!at(TokenKind::RParen)) {
                while (true) {
                    variant.payload.push_back(parse_type());
                    if (!match(TokenKind::Comma)) {
                        break;
                    }
                }
            }
            consume(TokenKind::RParen, "expected ')' after enum variant payload");
        }

        variant.span = span_from(variant_begin);
        declaration->variants.push_back(std::move(variant));

        if (!match(TokenKind::Comma)) {
            if (!at(TokenKind::RBrace)) {
                if (match(TokenKind::Semicolon)) {
                    continue;
                }
                error(current(), "expected ',' between enum variants");
                break;
            }
        }
    }

    consume(TokenKind::RBrace, "expected '}' to close enum body");
    match(TokenKind::Semicolon);
    declaration->span = span_from(begin);
    return declaration;
}

ast::DeclPtr Parser::parse_function_decl(ast::Visibility visibility,
                                         std::vector<ast::Attribute> attributes) {
    const auto begin = consume(TokenKind::KwFunction, "expected 'function'");

    auto declaration = make_node<ast::FunctionDecl>(begin.span);
    declaration->visibility = visibility;
    declaration->attributes = std::move(attributes);

    const auto name = consume(TokenKind::Identifier, "expected function name");
    declaration->name = name.text;

    if (match(TokenKind::Less)) {
        while (!at(TokenKind::Greater) && !at_end()) {
            const auto parameter = consume(TokenKind::Identifier, "expected type parameter name");
            if (parameter.kind == TokenKind::Identifier) {
                declaration->type_parameters.push_back(parameter.text);
            }
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        consume(TokenKind::Greater, "expected '>' after type parameters");
    }

    declaration->parameters = parse_parameter_list();

    if (match(TokenKind::Arrow)) {
        declaration->return_type = parse_type();
    }

    if (at(TokenKind::Semicolon)) {
        error(current(), "top-level function declarations require a body");
        advance();
    } else {
        declaration->body = parse_block();
    }

    declaration->span = span_from(begin);
    return declaration;
}

ast::HandlerCase Parser::parse_handler_case() {
    const auto begin = current();

    if (at(TokenKind::KwFn)) {
        error(current(), "'fn' is not supported; use 'function'");
        advance();
    } else {
        match(TokenKind::KwFunction);
    }

    ast::HandlerCase handler_case;
    handler_case.span = begin.span;

    auto path = parse_path();
    if (path.empty()) {
        synchronize_top_level();
        return handler_case;
    }

    if (path.size() == 1) {
        handler_case.operation = path[0];
    } else {
        if (path.size() > 2) {
            error(begin, "handler case must be written as 'Effect::operation'");
        }
        handler_case.effect = path[0];
        handler_case.operation = path[1];
    }

    handler_case.parameters = parse_parameter_list();

    if (match(TokenKind::Arrow)) {
        handler_case.result_type = parse_type();
    }

    handler_case.body = parse_block();
    handler_case.span = span_from(begin);
    return handler_case;
}

std::vector<ast::HandlerCase> Parser::parse_handler_cases() {
    std::vector<ast::HandlerCase> cases;
    consume(TokenKind::LBrace, "expected '{' to start handler");

    while (!at(TokenKind::RBrace) && !at_end()) {
        if (match(TokenKind::Semicolon)) {
            continue;
        }
        cases.push_back(parse_handler_case());
    }

    consume(TokenKind::RBrace, "expected '}' to close handler");
    return cases;
}

ast::DeclPtr Parser::parse_handler_decl(ast::Visibility visibility,
                                        std::vector<ast::Attribute> attributes) {
    const auto begin = consume(TokenKind::KwHandle, "expected 'handle'");

    auto declaration = make_node<ast::HandlerDecl>(begin.span);
    declaration->visibility = visibility;
    declaration->attributes = std::move(attributes);

    if (at(TokenKind::LParen)) {
        declaration->handled_expression = parse_expression();
        consume(TokenKind::KwWith, "expected 'with' after handled expression");
        declaration->cases = parse_handler_cases();
    } else {
        auto path = parse_path();
        declaration->effect = join_path(path);
        if (match(TokenKind::KwWith)) {
            declaration->handled_expression = parse_expression();
        }
        declaration->cases = parse_handler_cases();
    }

    match(TokenKind::Semicolon);
    declaration->span = span_from(begin);
    return declaration;
}

ast::StmtPtr Parser::parse_block() {
    const auto begin = consume(TokenKind::LBrace, "expected '{'");

    auto block = make_node<ast::BlockStmt>(begin.span);

    while (!at(TokenKind::RBrace) && !at_end()) {
        block->statements.push_back(parse_statement());
    }

    consume(TokenKind::RBrace, "expected '}' to close block");
    block->span = span_from(begin);
    return block;
}

ast::StmtPtr Parser::parse_var_decl() {
    const auto begin = current();
    auto declaration = try_parse_var_decl();
    if (declaration == nullptr) {
        error(current(), "expected variable declaration");
        synchronize_statement();
        return make_node<ast::EmptyStmt>(begin.span);
    }
    declaration->span = span_from(begin);
    return declaration;
}

std::unique_ptr<ast::VarDeclStmt> Parser::try_parse_var_decl() {
    const auto saved_index = index_;

    suppress_errors_ = true;
    auto probe_type = parse_type();
    const bool is_declaration =
        probe_type != nullptr && dynamic_cast<ast::ErrorTypeExpr*>(probe_type.get()) == nullptr &&
        at(TokenKind::Identifier) &&
        (peek().kind == TokenKind::Equal || peek().kind == TokenKind::Semicolon);
    suppress_errors_ = false;

    if (!is_declaration) {
        index_ = saved_index;
        return nullptr;
    }

    index_ = saved_index;
    auto declaration = std::make_unique<ast::VarDeclStmt>();
    declaration->type = parse_type();

    const auto name = consume(TokenKind::Identifier, "expected variable name");
    declaration->name = name.text;

    if (match(TokenKind::Equal)) {
        declaration->initializer = parse_expression();
    }

    consume(TokenKind::Semicolon, "expected ';' after variable declaration");
    return declaration;
}

ast::StmtPtr Parser::parse_if() {
    const auto begin = consume(TokenKind::KwIf, "expected 'if'");
    consume(TokenKind::LParen, "expected '(' after 'if'");
    auto condition = parse_expression();
    consume(TokenKind::RParen, "expected ')' after if condition");

    auto statement = make_node<ast::IfStmt>(begin.span);
    statement->condition = std::move(condition);
    statement->then_branch = parse_statement();

    if (match(TokenKind::KwElse)) {
        statement->else_branch = parse_statement();
    }

    statement->span = span_from(begin);
    return statement;
}

ast::StmtPtr Parser::parse_while() {
    const auto begin = consume(TokenKind::KwWhile, "expected 'while'");
    consume(TokenKind::LParen, "expected '(' after 'while'");
    auto condition = parse_expression();
    consume(TokenKind::RParen, "expected ')' after while condition");

    auto statement = make_node<ast::WhileStmt>(begin.span);
    statement->condition = std::move(condition);
    ++loop_depth_;
    statement->body = parse_statement();
    --loop_depth_;
    statement->span = span_from(begin);
    return statement;
}

ast::StmtPtr Parser::parse_statement() {
    if (at(TokenKind::LBrace)) {
        return parse_block();
    }

    if (match(TokenKind::Semicolon)) {
        return make_node<ast::EmptyStmt>(previous().span);
    }

    if (at(TokenKind::KwIf)) {
        return parse_if();
    }

    if (at(TokenKind::KwWhile)) {
        return parse_while();
    }

    if (match(TokenKind::KwReturn)) {
        const auto begin = previous();
        auto statement = make_node<ast::ReturnStmt>(begin.span);
        if (!at(TokenKind::Semicolon)) {
            statement->value = parse_expression();
        }
        consume(TokenKind::Semicolon, "expected ';' after return statement");
        statement->span = span_from(begin);
        return statement;
    }

    if (match(TokenKind::KwBreak)) {
        return make_node<ast::BreakStmt>(previous().span);
    }

    if (match(TokenKind::KwContinue)) {
        return make_node<ast::ContinueStmt>(previous().span);
    }

    if (auto declaration = try_parse_var_decl(); declaration != nullptr) {
        declaration->span =
            file_.span(declaration->type->span.begin, previous().span.end);
        return declaration;
    }

    const auto begin = current();
    auto expression = parse_expression();

    if (match(TokenKind::Equal)) {
        auto assignment = make_node<ast::AssignStmt>(begin.span);
        assignment->target = std::move(expression);
        assignment->value = parse_expression();
        consume(TokenKind::Semicolon, "expected ';' after assignment");
        assignment->span = span_from(begin);
        return assignment;
    }

    auto statement = make_node<ast::ExprStmt>(begin.span);
    statement->expression = std::move(expression);

    const bool tail_resume =
        dynamic_cast<ast::ResumeExpr*>(statement->expression.get()) != nullptr &&
        at(TokenKind::RBrace);

    if (!tail_resume) {
        consume(TokenKind::Semicolon, "expected ';' after expression");
    }

    statement->span = span_from(begin);
    return statement;
}

ast::ExprPtr Parser::parse_expression() {
    return parse_logical_or();
}

ast::ExprPtr Parser::parse_logical_or() {
    auto expression = parse_logical_and();
    while (at(TokenKind::PipePipe)) {
        const auto begin = current();
        advance();
        auto right = parse_logical_and();
        auto binary = make_node<ast::BinaryExpr>(span_from(begin));
        binary->op = "||";
        binary->left = std::move(expression);
        binary->right = std::move(right);
        expression = std::move(binary);
    }
    return expression;
}

ast::ExprPtr Parser::parse_logical_and() {
    auto expression = parse_equality();
    while (at(TokenKind::AmpAmp)) {
        const auto begin = current();
        advance();
        auto right = parse_equality();
        auto binary = make_node<ast::BinaryExpr>(span_from(begin));
        binary->op = "&&";
        binary->left = std::move(expression);
        binary->right = std::move(right);
        expression = std::move(binary);
    }
    return expression;
}

ast::ExprPtr Parser::parse_equality() {
    auto expression = parse_comparison();
    while (at(TokenKind::EqualEqual) || at(TokenKind::BangEqual)) {
        const auto begin = current();
        const auto op = advance().text;
        auto right = parse_comparison();
        auto binary = make_node<ast::BinaryExpr>(span_from(begin));
        binary->op = op;
        binary->left = std::move(expression);
        binary->right = std::move(right);
        expression = std::move(binary);
    }
    return expression;
}

ast::ExprPtr Parser::parse_comparison() {
    auto expression = parse_additive();
    while (at(TokenKind::Less) || at(TokenKind::LessEqual) || at(TokenKind::Greater) ||
           at(TokenKind::GreaterEqual)) {
        const auto begin = current();
        const auto op = advance().text;
        auto right = parse_additive();
        auto binary = make_node<ast::BinaryExpr>(span_from(begin));
        binary->op = op;
        binary->left = std::move(expression);
        binary->right = std::move(right);
        expression = std::move(binary);
    }
    return expression;
}

ast::ExprPtr Parser::parse_additive() {
    auto expression = parse_multiplicative();
    while (at(TokenKind::Plus) || at(TokenKind::Minus)) {
        const auto begin = current();
        const auto op = advance().text;
        auto right = parse_multiplicative();
        auto binary = make_node<ast::BinaryExpr>(span_from(begin));
        binary->op = op;
        binary->left = std::move(expression);
        binary->right = std::move(right);
        expression = std::move(binary);
    }
    return expression;
}

ast::ExprPtr Parser::parse_multiplicative() {
    auto expression = parse_unary();
    while (at(TokenKind::Star) || at(TokenKind::Slash) || at(TokenKind::Percent)) {
        const auto begin = current();
        const auto op = advance().text;
        auto right = parse_unary();
        auto binary = make_node<ast::BinaryExpr>(span_from(begin));
        binary->op = op;
        binary->left = std::move(expression);
        binary->right = std::move(right);
        expression = std::move(binary);
    }
    return expression;
}

ast::ExprPtr Parser::parse_unary() {
    if (at(TokenKind::Minus) || at(TokenKind::Bang) || at(TokenKind::Plus)) {
        const auto begin = current();
        const auto op = advance().text;
        auto operand = parse_unary();
        auto expression = make_node<ast::UnaryExpr>(span_from(begin));
        expression->op = op;
        expression->operand = std::move(operand);
        return expression;
    }

    if (at(TokenKind::Identifier) && current().text == "downcast" &&
        peek().kind == TokenKind::ColonColon && peek(2).kind == TokenKind::Less) {
        const auto begin = current();
        advance();
        advance();
        auto type_arguments = parse_type_arguments();
        if (type_arguments.size() != 1) {
            error(begin, "'downcast' expects exactly one target type");
        }

        auto expression = make_node<ast::DowncastExpr>(begin.span);
        if (!type_arguments.empty()) {
            expression->target = std::move(type_arguments.front());
        } else {
            expression->target = make_node<ast::ErrorTypeExpr>(begin.span);
        }
        expression->operand = parse_unary();
        expression->span = span_from(begin);
        return expression;
    }

    return parse_postfix();
}

bool Parser::looks_like_type_argument_call() const {
    if (!at(TokenKind::Less)) {
        return false;
    }

    int depth = 0;
    for (std::size_t i = index_; i < tokens_.size(); ++i) {
        if (tokens_[i].kind == TokenKind::Less) {
            ++depth;
        } else if (tokens_[i].kind == TokenKind::Greater) {
            --depth;
            if (depth == 0) {
                return i + 1 < tokens_.size() && tokens_[i + 1].kind == TokenKind::LParen;
            }
        } else if (tokens_[i].kind == TokenKind::Eof) {
            return false;
        }
    }
    return false;
}

ast::ExprPtr Parser::parse_postfix() {
    auto expression = parse_primary();
    std::vector<ast::TypeExprPtr> pending_type_arguments;

    while (true) {
        if (at(TokenKind::Less) && looks_like_type_argument_call()) {
            pending_type_arguments = parse_type_arguments();
            continue;
        }

        if (match(TokenKind::LParen)) {
            const auto begin = previous();
            auto call = make_node<ast::CallExpr>(begin.span);
            call->callee = std::move(expression);
            call->type_arguments = std::move(pending_type_arguments);
            pending_type_arguments.clear();

            if (!at(TokenKind::RParen)) {
                while (true) {
                    call->arguments.push_back(parse_expression());
                    if (!match(TokenKind::Comma)) {
                        break;
                    }
                }
            }

            consume(TokenKind::RParen, "expected ')' after arguments");
            call->span = span_from(begin);
            expression = std::move(call);
            continue;
        }

        if (match(TokenKind::Dot)) {
            const auto begin = previous();
            const auto name = consume(TokenKind::Identifier, "expected member name after '.'");
            auto member = make_node<ast::MemberExpr>(begin.span);
            member->object = std::move(expression);
            member->name = name.text;
            member->span = span_from(begin);
            expression = std::move(member);
            continue;
        }

        if (match(TokenKind::LBracket)) {
            const auto begin = previous();
            auto index = make_node<ast::IndexExpr>(begin.span);
            index->object = std::move(expression);
            index->index = parse_expression();
            consume(TokenKind::RBracket, "expected ']' after index");
            index->span = span_from(begin);
            expression = std::move(index);
            continue;
        }

        break;
    }

    if (!pending_type_arguments.empty()) {
        error(current(), "type arguments must be followed by a call");
    }

    return expression;
}

bool Parser::is_lambda_ahead() const {
    if (!at(TokenKind::LParen)) {
        return false;
    }

    int depth = 0;
    for (std::size_t i = index_; i < tokens_.size(); ++i) {
        if (tokens_[i].kind == TokenKind::LParen) {
            ++depth;
        } else if (tokens_[i].kind == TokenKind::RParen) {
            --depth;
            if (depth == 0) {
                return i + 1 < tokens_.size() &&
                       tokens_[i + 1].kind == TokenKind::FatArrow;
            }
        } else if (tokens_[i].kind == TokenKind::Eof) {
            return false;
        }
    }
    return false;
}

ast::ExprPtr Parser::parse_lambda() {
    const auto begin = current();
    auto lambda = make_node<ast::LambdaExpr>(begin.span);
    lambda->parameters = parse_parameter_list();
    consume(TokenKind::FatArrow, "expected '=>' after lambda parameters");

    if (at(TokenKind::LBrace)) {
        lambda->body = parse_block();
    } else {
        auto block = make_node<ast::BlockStmt>(current().span);
        auto statement = make_node<ast::ExprStmt>(current().span);
        statement->expression = parse_expression();
        const auto statement_span = statement->span;
        block->statements.push_back(std::move(statement));
        block->span = statement_span;
        lambda->body = std::move(block);
    }

    lambda->span = span_from(begin);
    return lambda;
}

ast::ExprPtr Parser::parse_parenthesized() {
    const auto begin = consume(TokenKind::LParen, "expected '('");

    if (match(TokenKind::RParen)) {
        return make_node<ast::UnitLiteralExpr>(span_from(begin));
    }

    auto expression = parse_expression();
    consume(TokenKind::RParen, "expected ')' after expression");
    expression->span = span_from(begin);
    return expression;
}

ast::ExprPtr Parser::parse_resume() {
    const auto begin = consume(TokenKind::KwResume, "expected 'resume'");
    auto expression = make_node<ast::ResumeExpr>(begin.span);

    if (!at(TokenKind::Semicolon) && !at(TokenKind::RParen) && !at(TokenKind::RBrace) &&
        !at(TokenKind::Comma) && !at_end()) {
        auto value = parse_expression();
        if (auto* wildcard = dynamic_cast<ast::WildcardExpr*>(value.get()); wildcard != nullptr) {
            expression->wildcard = true;
            expression->value.reset();
        } else {
            expression->value = std::move(value);
        }
    } else {
        expression->wildcard = true;
    }

    expression->span = span_from(begin);
    return expression;
}

ast::ExprPtr Parser::parse_handle_expr() {
    const auto begin = consume(TokenKind::KwHandle, "expected 'handle'");
    auto expression = make_node<ast::HandleExpr>(begin.span);
    expression->body = parse_expression();
    consume(TokenKind::KwWith, "expected 'with' after handled expression");
    expression->cases = parse_handler_cases();
    expression->span = span_from(begin);
    return expression;
}

ast::ExprPtr Parser::parse_primary() {
    if (at(TokenKind::Integer)) {
        const auto token = advance();
        auto expression = make_node<ast::IntLiteralExpr>(token.span);
        expression->text = token.text;
        return expression;
    }

    if (at(TokenKind::Float)) {
        const auto token = advance();
        auto expression = make_node<ast::FloatLiteralExpr>(token.span);
        expression->text = token.text;
        return expression;
    }

    if (at(TokenKind::String)) {
        const auto token = advance();
        auto expression = make_node<ast::StringLiteralExpr>(token.span);
        expression->value = token.text;
        return expression;
    }

    if (at(TokenKind::TemplateString)) {
        const auto token = advance();
        auto expression = make_node<ast::TemplateStringExpr>(token.span);
        for (const auto& part : token.template_parts) {
            ast::TemplatePart output;
            output.expression = part.expression;
            if (part.expression) {
                Parser sub_parser(file_, diagnostics_, part.begin, part.end);
                output.expression_value = sub_parser.parse_expression_only();
            } else {
                output.text = part.text;
            }
            expression->parts.push_back(std::move(output));
        }
        return expression;
    }

    if (match(TokenKind::KwTrue)) {
        auto expression = make_node<ast::BoolLiteralExpr>(previous().span);
        expression->value = true;
        return expression;
    }

    if (match(TokenKind::KwFalse)) {
        auto expression = make_node<ast::BoolLiteralExpr>(previous().span);
        expression->value = false;
        return expression;
    }

    if (match(TokenKind::Underscore)) {
        return make_node<ast::WildcardExpr>(previous().span);
    }

    if (at(TokenKind::Identifier)) {
        const auto begin = current();
        auto expression = make_node<ast::IdentifierExpr>(begin.span);
        expression->path = parse_path();
        expression->span = span_from(begin);
        return expression;
    }

    if (at(TokenKind::LParen)) {
        if (is_lambda_ahead()) {
            return parse_lambda();
        }
        return parse_parenthesized();
    }

    if (at(TokenKind::KwResume)) {
        return parse_resume();
    }

    if (at(TokenKind::KwHandle)) {
        return parse_handle_expr();
    }

    const auto token = current();
    error(token, "expected expression");
    advance();
    return make_node<ast::ErrorExpr>(token.span);
}

ast::ModuleUnit Parser::parse_module() {
    ast::ModuleUnit module;
    module.path = file_.path();

    while (!at_end()) {
        auto attributes = parse_attributes();
        auto visibility = parse_visibility();

        if (at(TokenKind::KwImport)) {
            if (visibility != ast::Visibility::Local) {
                error(current(), "import declarations cannot have visibility modifiers");
            }
            if (!attributes.empty()) {
                error(current(), "import declarations cannot have attributes");
            }
            module.declarations.push_back(parse_import());
            continue;
        }

        if (at(TokenKind::KwClass)) {
            module.declarations.push_back(parse_class(visibility, std::move(attributes)));
            continue;
        }

        if (at(TokenKind::KwEnum)) {
            module.declarations.push_back(parse_enum(visibility, std::move(attributes)));
            continue;
        }

        if (at(TokenKind::KwEffect)) {
            module.declarations.push_back(parse_effect(visibility, std::move(attributes)));
            continue;
        }

        if (at(TokenKind::KwHandle)) {
            module.declarations.push_back(parse_handler_decl(visibility, std::move(attributes)));
            continue;
        }

        if (at(TokenKind::KwFunction)) {
            module.declarations.push_back(parse_function_decl(visibility, std::move(attributes)));
            continue;
        }

        if (at(TokenKind::KwFn)) {
            error(current(), "'fn' is not supported; use 'function'");
            advance();
            module.declarations.push_back(parse_function_decl(visibility, std::move(attributes)));
            continue;
        }

        error(current(), "expected a top-level declaration");
        advance();
        synchronize_top_level();
    }

    return module;
}

ast::ExprPtr Parser::parse_expression_only() {
    auto expression = parse_expression();
    if (!at_end()) {
        error(current(), "unexpected token after expression");
    }
    return expression;
}

} // namespace glscript
