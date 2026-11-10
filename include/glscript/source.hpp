#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glscript {

class SourceFile;

struct SourceLocation {
    std::uint32_t offset = 0;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

struct SourceSpan {
    const SourceFile* file = nullptr;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    SourceLocation start() const;
    SourceLocation finish() const;

    bool valid() const { return file != nullptr && end >= begin; }
    std::string_view text() const;
};

class SourceFile {
public:
    SourceFile(std::string path, std::string text);

    const std::string& path() const { return path_; }
    const std::string& text() const { return text_; }

    SourceLocation location(std::uint32_t offset) const;
    SourceSpan span(std::uint32_t begin, std::uint32_t end) const;

    std::string_view line_text(std::uint32_t line_number) const;
    std::uint32_t line_count() const {
        return static_cast<std::uint32_t>(line_starts_.size());
    }

private:
    std::string path_;
    std::string text_;
    std::vector<std::uint32_t> line_starts_;
};

} // namespace glscript
