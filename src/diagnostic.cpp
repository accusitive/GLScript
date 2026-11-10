#include "glscript/diagnostic.hpp"

#include <sstream>

namespace glscript {
namespace {

const char* severity_name(Severity severity) {
    switch (severity) {
    case Severity::Note:
        return "note";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    }
    return "error";
}

void append_formatted(std::ostringstream& out, const Diagnostic& diagnostic) {
    if (!diagnostic.file_path.empty()) {
        out << diagnostic.file_path << ':' << diagnostic.line << ':' << diagnostic.column
            << ": ";
    }
    out << severity_name(diagnostic.severity) << ": " << diagnostic.message << '\n';

    if (!diagnostic.source_line.empty()) {
        out << "  " << diagnostic.source_line << '\n';

        const auto column = diagnostic.column > 0 ? diagnostic.column - 1 : 0;
        out << "  ";
        for (std::uint32_t i = 0; i < column; ++i) {
            out << (i < diagnostic.source_line.size() && diagnostic.source_line[i] == '\t' ? '\t'
                                                                                          : ' ');
        }

        auto width = diagnostic.length > 0 ? diagnostic.length : 1;
        width = std::min<std::uint32_t>(width, 80);
        for (std::uint32_t i = 0; i < width; ++i) {
            out << '^';
        }
        out << '\n';
    }

    for (const auto& note : diagnostic.notes) {
        out << "  note: " << note << '\n';
    }
}

} // namespace

std::string Diagnostic::format() const {
    std::ostringstream out;
    append_formatted(out, *this);
    return out.str();
}

void DiagnosticSink::report(Severity severity, SourceSpan span, std::string message) {
    if (severity == Severity::Error) {
        ++error_count_;
    } else if (severity == Severity::Warning) {
        ++warning_count_;
    }

    Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.span = span;
    diagnostic.message = std::move(message);

    if (span.file != nullptr) {
        const auto location = span.start();
        diagnostic.file_path = span.file->path();
        diagnostic.line = location.line;
        diagnostic.column = location.column;
        diagnostic.source_line = std::string(span.file->line_text(location.line));
        diagnostic.length = span.end > span.begin ? span.end - span.begin : 1;
    }

    diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticSink::error(SourceSpan span, std::string message) {
    report(Severity::Error, span, std::move(message));
}

void DiagnosticSink::warning(SourceSpan span, std::string message) {
    report(Severity::Warning, span, std::move(message));
}

void DiagnosticSink::note(SourceSpan span, std::string message) {
    report(Severity::Note, span, std::move(message));
}

std::string DiagnosticSink::format() const {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics_) {
        append_formatted(out, diagnostic);
    }
    return out.str();
}

void DiagnosticSink::clear() {
    diagnostics_.clear();
    error_count_ = 0;
    warning_count_ = 0;
}

} // namespace glscript
