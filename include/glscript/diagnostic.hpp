#pragma once

#include "glscript/source.hpp"

#include <string>
#include <vector>

namespace glscript {

enum class Severity {
    Note,
    Warning,
    Error,
};

struct Diagnostic {
    Severity severity = Severity::Error;
    SourceSpan span;
    std::string message;
    std::vector<std::string> notes;

    std::string file_path;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string source_line;
    std::uint32_t length = 0;

    std::string format() const;
};

class DiagnosticSink {
public:
    void report(Severity severity, SourceSpan span, std::string message);
    void error(SourceSpan span, std::string message);
    void warning(SourceSpan span, std::string message);
    void note(SourceSpan span, std::string message);

    bool has_errors() const { return error_count_ > 0; }
    std::size_t error_count() const { return error_count_; }
    std::size_t warning_count() const { return warning_count_; }

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    std::string format() const;

    void clear();

private:
    std::vector<Diagnostic> diagnostics_;
    std::size_t error_count_ = 0;
    std::size_t warning_count_ = 0;
};

} // namespace glscript
