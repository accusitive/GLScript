#include "glscript/source.hpp"

#include <algorithm>

namespace glscript {

SourceLocation SourceSpan::start() const {
    if (file == nullptr) {
        return {};
    }
    return file->location(begin);
}

SourceLocation SourceSpan::finish() const {
    if (file == nullptr) {
        return {};
    }
    return file->location(end);
}

std::string_view SourceSpan::text() const {
    if (file == nullptr || end < begin) {
        return {};
    }
    return std::string_view(file->text()).substr(begin, end - begin);
}

SourceFile::SourceFile(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            line_starts_.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
}

SourceLocation SourceFile::location(std::uint32_t offset) const {
    if (line_starts_.empty()) {
        return {offset, 1, 1};
    }

    offset = std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(text_.size()));

    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    const auto line_index = static_cast<std::size_t>(std::distance(line_starts_.begin(), it) - 1);
    const auto line_start = line_starts_[line_index];

    return {
        offset,
        static_cast<std::uint32_t>(line_index + 1),
        static_cast<std::uint32_t>(offset - line_start + 1),
    };
}

SourceSpan SourceFile::span(std::uint32_t begin, std::uint32_t end) const {
    return SourceSpan{this, begin, end};
}

std::string_view SourceFile::line_text(std::uint32_t line_number) const {
    if (line_number == 0 || line_number > line_starts_.size()) {
        return {};
    }

    const auto start = line_starts_[line_number - 1];
    auto end = static_cast<std::uint32_t>(text_.size());
    if (line_number < line_starts_.size()) {
        end = line_starts_[line_number] - 1;
    }
    if (end > start && text_[end - 1] == '\r') {
        --end;
    }
    return std::string_view(text_).substr(start, end - start);
}

} // namespace glscript
