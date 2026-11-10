#pragma once

#include "glscript/diagnostic.hpp"
#include "glscript/source.hpp"
#include "glscript/token.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace glscript {

class Lexer {
public:
    Lexer(const SourceFile& file, DiagnosticSink& diagnostics);
    Lexer(const SourceFile& file, DiagnosticSink& diagnostics, std::uint32_t begin, std::uint32_t end);

    std::vector<Token> tokenize();

private:
    const SourceFile& file_;
    DiagnosticSink& diagnostics_;
    const std::string& text_;
    std::uint32_t position_ = 0;
    std::uint32_t end_ = 0;

    bool at_end() const { return position_ >= end_; }
    char peek(std::uint32_t lookahead = 0) const;
    char advance();
    bool match(char expected);

    Token make_token(TokenKind kind, std::uint32_t begin, std::uint32_t token_end,
                     std::string text = {}) const;
    void skip_trivia();

    Token lex_identifier(std::uint32_t begin);
    Token lex_number(std::uint32_t begin);
    Token lex_string(std::uint32_t begin);
    Token lex_template_string(std::uint32_t begin);
};

} // namespace glscript
