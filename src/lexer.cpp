#include "glscript/lexer.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace glscript {
namespace {

const std::unordered_map<std::string, TokenKind>& keywords() {
    static const std::unordered_map<std::string, TokenKind> map = {
        {"import", TokenKind::KwImport},
        {"export", TokenKind::KwExport},
        {"pub", TokenKind::KwPub},
        {"class", TokenKind::KwClass},
        {"extends", TokenKind::KwExtends},
        {"function", TokenKind::KwFunction},
        {"fn", TokenKind::KwFn},
        {"effect", TokenKind::KwEffect},
        {"handle", TokenKind::KwHandle},
        {"with", TokenKind::KwWith},
        {"enum", TokenKind::KwEnum},
        {"resume", TokenKind::KwResume},
        {"dynamic", TokenKind::KwDynamic},
        {"match", TokenKind::KwMatch},
        {"let", TokenKind::KwLet},
        {"true", TokenKind::KwTrue},
        {"false", TokenKind::KwFalse},
        {"if", TokenKind::KwIf},
        {"else", TokenKind::KwElse},
        {"while", TokenKind::KwWhile},
        {"return", TokenKind::KwReturn},
        {"break", TokenKind::KwBreak},
        {"continue", TokenKind::KwContinue},
    };
    return map;
}

bool is_identifier_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_identifier_continue(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_hex_digit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

} // namespace

const char* token_kind_name(TokenKind kind) {
    switch (kind) {
    case TokenKind::Eof:
        return "end of file";
    case TokenKind::Invalid:
        return "invalid token";
    case TokenKind::Identifier:
        return "identifier";
    case TokenKind::Integer:
        return "integer literal";
    case TokenKind::Float:
        return "float literal";
    case TokenKind::String:
        return "string literal";
    case TokenKind::TemplateString:
        return "template string";
    case TokenKind::LParen:
        return "'('";
    case TokenKind::RParen:
        return "')'";
    case TokenKind::LBrace:
        return "'{'";
    case TokenKind::RBrace:
        return "'}'";
    case TokenKind::LBracket:
        return "'['";
    case TokenKind::RBracket:
        return "']'";
    case TokenKind::Less:
        return "'<'";
    case TokenKind::Greater:
        return "'>'";
    case TokenKind::Comma:
        return "','";
    case TokenKind::Semicolon:
        return "';'";
    case TokenKind::Colon:
        return "':'";
    case TokenKind::ColonColon:
        return "'::'";
    case TokenKind::Dot:
        return "'.'";
    case TokenKind::Arrow:
        return "'->'";
    case TokenKind::FatArrow:
        return "'=>'";
    case TokenKind::Plus:
        return "'+'";
    case TokenKind::Minus:
        return "'-'";
    case TokenKind::Star:
        return "'*'";
    case TokenKind::Slash:
        return "'/'";
    case TokenKind::Percent:
        return "'%'";
    case TokenKind::Equal:
        return "'='";
    case TokenKind::EqualEqual:
        return "'=='";
    case TokenKind::Bang:
        return "'!'";
    case TokenKind::BangEqual:
        return "'!='";
    case TokenKind::LessEqual:
        return "'<='";
    case TokenKind::GreaterEqual:
        return "'>='";
    case TokenKind::AmpAmp:
        return "'&&'";
    case TokenKind::PipePipe:
        return "'||'";
    case TokenKind::Question:
        return "'?'";
    case TokenKind::Hash:
        return "'#'";
    case TokenKind::Underscore:
        return "'_'";
    default:
        break;
    }

    if (const char* keyword = keyword_text(kind); keyword != nullptr) {
        return keyword;
    }
    return "token";
}

const char* keyword_text(TokenKind kind) {
    switch (kind) {
    case TokenKind::KwImport:
        return "'import'";
    case TokenKind::KwExport:
        return "'export'";
    case TokenKind::KwPub:
        return "'pub'";
    case TokenKind::KwClass:
        return "'class'";
    case TokenKind::KwExtends:
        return "'extends'";
    case TokenKind::KwFunction:
        return "'function'";
    case TokenKind::KwFn:
        return "'fn'";
    case TokenKind::KwEffect:
        return "'effect'";
    case TokenKind::KwHandle:
        return "'handle'";
    case TokenKind::KwWith:
        return "'with'";
    case TokenKind::KwEnum:
        return "'enum'";
    case TokenKind::KwResume:
        return "'resume'";
    case TokenKind::KwMatch:
        return "'match'";
    case TokenKind::KwLet:
        return "'let'";
    case TokenKind::KwDynamic:
        return "'dynamic'";
    case TokenKind::KwTrue:
        return "'true'";
    case TokenKind::KwFalse:
        return "'false'";
    case TokenKind::KwIf:
        return "'if'";
    case TokenKind::KwElse:
        return "'else'";
    case TokenKind::KwWhile:
        return "'while'";
    case TokenKind::KwReturn:
        return "'return'";
    case TokenKind::KwBreak:
        return "'break'";
    case TokenKind::KwContinue:
        return "'continue'";
    default:
        return nullptr;
    }
}

Lexer::Lexer(const SourceFile& file, DiagnosticSink& diagnostics)
    : Lexer(file, diagnostics, 0, static_cast<std::uint32_t>(file.text().size())) {}

Lexer::Lexer(const SourceFile& file, DiagnosticSink& diagnostics, std::uint32_t begin,
             std::uint32_t end)
    : file_(file),
      diagnostics_(diagnostics),
      text_(file.text()),
      position_(begin),
      end_(std::min<std::uint32_t>(end, static_cast<std::uint32_t>(file.text().size()))) {}

char Lexer::peek(std::uint32_t lookahead) const {
    const auto offset = position_ + lookahead;
    if (offset >= end_) {
        return '\0';
    }
    return text_[offset];
}

char Lexer::advance() {
    if (at_end()) {
        return '\0';
    }
    return text_[position_++];
}

bool Lexer::match(char expected) {
    if (peek() != expected) {
        return false;
    }
    ++position_;
    return true;
}

Token Lexer::make_token(TokenKind kind, std::uint32_t begin, std::uint32_t token_end,
                        std::string text) const {
    if (text.empty() && token_end > begin && kind != TokenKind::Eof) {
        text = std::string(text_.substr(begin, token_end - begin));
    }
    return Token{kind, file_.span(begin, token_end), std::move(text), {}};
}

void Lexer::skip_trivia() {
    while (!at_end()) {
        const char c = peek();

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            advance();
            continue;
        }

        if (c == '/' && peek(1) == '/') {
            advance();
            advance();
            while (!at_end() && peek() != '\n') {
                advance();
            }
            continue;
        }

        if (c == '/' && peek(1) == '*') {
            const auto begin = position_;
            advance();
            advance();
            int depth = 1;
            while (!at_end() && depth > 0) {
                if (peek() == '/' && peek(1) == '*') {
                    advance();
                    advance();
                    ++depth;
                } else if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    --depth;
                } else {
                    advance();
                }
            }
            if (depth != 0) {
                diagnostics_.error(file_.span(begin, position_), "unterminated block comment");
            }
            continue;
        }

        break;
    }
}

Token Lexer::lex_identifier(std::uint32_t begin) {
    while (is_identifier_continue(peek())) {
        advance();
    }

    auto text = text_.substr(begin, position_ - begin);

    if (text == "_") {
        return make_token(TokenKind::Underscore, begin, position_, std::string(text));
    }

    if (const auto it = keywords().find(std::string(text)); it != keywords().end()) {
        return make_token(it->second, begin, position_, std::string(text));
    }

    return make_token(TokenKind::Identifier, begin, position_, std::string(text));
}

Token Lexer::lex_number(std::uint32_t begin) {
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance();
        advance();
        const auto digits_begin = position_;
        while (is_hex_digit(peek())) {
            advance();
        }
        if (position_ == digits_begin) {
            diagnostics_.error(file_.span(begin, position_), "expected hexadecimal digits");
        }
        return make_token(TokenKind::Integer, begin, position_,
                          std::string(text_.substr(begin, position_ - begin)));
    }

    while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        advance();
    }

    bool is_float = false;
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1))) != 0) {
        is_float = true;
        advance();
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            advance();
        }
    }

    if (peek() == 'e' || peek() == 'E') {
        const auto exponent_begin = position_;
        advance();
        if (peek() == '+' || peek() == '-') {
            advance();
        }
        if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
            diagnostics_.error(file_.span(exponent_begin, position_),
                               "expected exponent digits");
        } else {
            is_float = true;
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                advance();
            }
        }
    }

    if (peek() == 'f' || peek() == 'F') {
        is_float = true;
        advance();
    }

    return make_token(is_float ? TokenKind::Float : TokenKind::Integer, begin, position_,
                      std::string(text_.substr(begin, position_ - begin)));
}

Token Lexer::lex_string(std::uint32_t begin) {
    advance(); // opening quote

    std::string value;
    bool terminated = false;

    while (!at_end()) {
        const char c = advance();

        if (c == '"') {
            terminated = true;
            break;
        }

        if (c != '\\') {
            value.push_back(c);
            continue;
        }

        if (at_end()) {
            break;
        }

        switch (advance()) {
        case 'n':
            value.push_back('\n');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case 't':
            value.push_back('\t');
            break;
        case '0':
            value.push_back('\0');
            break;
        case '\\':
            value.push_back('\\');
            break;
        case '"':
            value.push_back('"');
            break;
        case '`':
            value.push_back('`');
            break;
        case '$':
            value.push_back('$');
            break;
        default:
            diagnostics_.warning(file_.span(position_ - 2, position_),
                                 "unknown escape sequence; using the character literally");
            value.push_back(text_[position_ - 1]);
            break;
        }
    }

    if (!terminated) {
        diagnostics_.error(file_.span(begin, position_), "unterminated string literal");
    }

    return make_token(TokenKind::String, begin, position_, std::move(value));
}

Token Lexer::lex_template_string(std::uint32_t begin) {
    advance(); // opening backtick

    Token token = make_token(TokenKind::TemplateString, begin, begin);
    std::string text;
    bool terminated = false;

    while (!at_end()) {
        const char c = peek();

        if (c == '`') {
            advance();
            terminated = true;
            break;
        }

        if (c == '\\') {
            advance();
            if (at_end()) {
                break;
            }
            const char escaped = advance();
            switch (escaped) {
            case 'n':
                text.push_back('\n');
                break;
            case 'r':
                text.push_back('\r');
                break;
            case 't':
                text.push_back('\t');
                break;
            case '`':
                text.push_back('`');
                break;
            case '\\':
                text.push_back('\\');
                break;
            case '$':
                text.push_back('$');
                break;
            default:
                text.push_back(escaped);
                break;
            }
            continue;
        }

        if (c == '$' && peek(1) == '{') {
            if (!text.empty()) {
                token.template_parts.push_back(TemplatePartToken{false, text, 0, 0});
                text.clear();
            }

            advance();
            advance();

            const auto expression_begin = position_;
            int depth = 1;
            bool in_string = false;
            bool in_template = false;

            while (!at_end() && depth > 0) {
                const char current = advance();

                if (in_string) {
                    if (current == '\\' && !at_end()) {
                        advance();
                    } else if (current == '"') {
                        in_string = false;
                    }
                    continue;
                }

                if (in_template) {
                    if (current == '\\' && !at_end()) {
                        advance();
                    } else if (current == '`') {
                        in_template = false;
                    }
                    continue;
                }

                if (current == '"') {
                    in_string = true;
                } else if (current == '`') {
                    in_template = true;
                } else if (current == '{') {
                    ++depth;
                } else if (current == '}') {
                    --depth;
                }
            }

            if (depth != 0) {
                diagnostics_.error(file_.span(expression_begin, position_),
                                   "unterminated template interpolation");
                break;
            }

            const auto expression_end = position_ - 1;
            token.template_parts.push_back(
                TemplatePartToken{true, {}, expression_begin, expression_end});
            continue;
        }

        text.push_back(advance());
    }

    if (!terminated) {
        diagnostics_.error(file_.span(begin, position_), "unterminated template string");
    }

    if (!text.empty() || token.template_parts.empty()) {
        token.template_parts.push_back(TemplatePartToken{false, std::move(text), 0, 0});
    }

    token.span = file_.span(begin, position_);
    return token;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (true) {
        skip_trivia();

        if (at_end()) {
            tokens.push_back(make_token(TokenKind::Eof, position_, position_));
            break;
        }

        const auto begin = position_;
        const char c = peek();

        if (is_identifier_start(c)) {
            tokens.push_back(lex_identifier(begin));
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            tokens.push_back(lex_number(begin));
            continue;
        }

        if (c == '"') {
            tokens.push_back(lex_string(begin));
            continue;
        }

        if (c == '`') {
            tokens.push_back(lex_template_string(begin));
            continue;
        }

        advance();

        switch (c) {
        case '(':
            tokens.push_back(make_token(TokenKind::LParen, begin, position_));
            break;
        case ')':
            tokens.push_back(make_token(TokenKind::RParen, begin, position_));
            break;
        case '{':
            tokens.push_back(make_token(TokenKind::LBrace, begin, position_));
            break;
        case '}':
            tokens.push_back(make_token(TokenKind::RBrace, begin, position_));
            break;
        case '[':
            tokens.push_back(make_token(TokenKind::LBracket, begin, position_));
            break;
        case ']':
            tokens.push_back(make_token(TokenKind::RBracket, begin, position_));
            break;
        case '<':
            tokens.push_back(make_token(match('=') ? TokenKind::LessEqual : TokenKind::Less,
                                         begin, position_));
            break;
        case '>':
            tokens.push_back(make_token(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater,
                                         begin, position_));
            break;
        case ',':
            tokens.push_back(make_token(TokenKind::Comma, begin, position_));
            break;
        case ';':
            tokens.push_back(make_token(TokenKind::Semicolon, begin, position_));
            break;
        case ':':
            tokens.push_back(make_token(match(':') ? TokenKind::ColonColon : TokenKind::Colon,
                                         begin, position_));
            break;
        case '.':
            tokens.push_back(make_token(TokenKind::Dot, begin, position_));
            break;
        case '+':
            tokens.push_back(make_token(TokenKind::Plus, begin, position_));
            break;
        case '-':
            tokens.push_back(make_token(match('>') ? TokenKind::Arrow : TokenKind::Minus, begin,
                                         position_));
            break;
        case '*':
            tokens.push_back(make_token(TokenKind::Star, begin, position_));
            break;
        case '/':
            tokens.push_back(make_token(TokenKind::Slash, begin, position_));
            break;
        case '%':
            tokens.push_back(make_token(TokenKind::Percent, begin, position_));
            break;
        case '=':
            if (match('=')) {
                tokens.push_back(make_token(TokenKind::EqualEqual, begin, position_));
            } else if (match('>')) {
                tokens.push_back(make_token(TokenKind::FatArrow, begin, position_));
            } else {
                tokens.push_back(make_token(TokenKind::Equal, begin, position_));
            }
            break;
        case '!':
            tokens.push_back(make_token(match('=') ? TokenKind::BangEqual : TokenKind::Bang,
                                         begin, position_));
            break;
        case '&':
            if (match('&')) {
                tokens.push_back(make_token(TokenKind::AmpAmp, begin, position_));
            } else {
                diagnostics_.error(file_.span(begin, position_),
                                   "expected '&' to form '&&'");
                tokens.push_back(make_token(TokenKind::Invalid, begin, position_));
            }
            break;
        case '|':
            if (match('|')) {
                tokens.push_back(make_token(TokenKind::PipePipe, begin, position_));
            } else {
                diagnostics_.error(file_.span(begin, position_),
                                   "expected '|' to form '||'");
                tokens.push_back(make_token(TokenKind::Invalid, begin, position_));
            }
            break;
        case '?':
            tokens.push_back(make_token(TokenKind::Question, begin, position_));
            break;
        case '#':
            tokens.push_back(make_token(TokenKind::Hash, begin, position_));
            break;
        default:
            diagnostics_.error(file_.span(begin, position_),
                               std::string("unexpected character '") + c + "'");
            tokens.push_back(make_token(TokenKind::Invalid, begin, position_));
            break;
        }
    }

    return tokens;
}

} // namespace glscript
