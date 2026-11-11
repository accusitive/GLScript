#pragma once

#include "glscript/source.hpp"

#include <string>
#include <vector>

namespace glscript {

enum class TokenKind {
    Eof,
    Invalid,

    Identifier,
    Integer,
    Float,
    String,
    TemplateString,

    KwImport,
    KwExport,
    KwPub,
    KwClass,
    KwExtends,
    KwFunction,
    KwFn,
    KwEffect,
    KwHandle,
    KwWith,
    KwEnum,
    KwResume,
    KwDynamic,
    KwMatch,
    KwLet,
    KwTrue,
    KwFalse,
    KwIf,
    KwElse,
    KwWhile,
    KwReturn,
    KwBreak,
    KwContinue,

    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Less,
    Greater,
    Comma,
    Semicolon,
    Colon,
    ColonColon,
    Dot,
    Arrow,
    FatArrow,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Equal,
    EqualEqual,
    Bang,
    BangEqual,
    LessEqual,
    GreaterEqual,
    AmpAmp,
    PipePipe,
    Question,
    Hash,
    Underscore,
};

struct TemplatePartToken {
    bool expression = false;
    std::string text;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    SourceSpan span;
    std::string text;
    std::vector<TemplatePartToken> template_parts;
};

const char* token_kind_name(TokenKind kind);
const char* keyword_text(TokenKind kind);

} // namespace glscript
