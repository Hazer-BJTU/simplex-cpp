#include "textedit/line_index.hpp"

#include <algorithm>
#include <stdexcept>

namespace textedit {
namespace {

/// Return the complete terminator width, or zero for an ordinary byte.
/// Inspect only available bytes; a partial UTF-8 newline is ordinary content.
std::size_t newline_width(std::string_view remaining)
{
    const auto first = static_cast<unsigned char>(remaining.front());
    if (first == '\r') {
        return remaining.size() >= 2 && remaining[1] == '\n' ? 2 : 1;
    }
    if (first == '\n' || first == '\v' || first == '\f') {
        return 1;
    }
    if (remaining.starts_with("\xc2\x85")) {
        return 2;
    }
    if (remaining.starts_with("\xe2\x80\xa8") ||
        remaining.starts_with("\xe2\x80\xa9")) {
        return 3;
    }
    return 0;
}

} // namespace

LineIndex::LineIndex(std::string_view text)
    : _byte_count(text.size()), _line_starts{0}
{
    for (std::size_t offset = 0; offset < text.size();) {
        const auto width = newline_width(text.substr(offset));
        if (width == 0) {
            ++offset;
        } else {
            offset += width;
            _line_starts.push_back(offset);
        }
    }
}

std::size_t LineIndex::byte_count() const noexcept
{
    return _byte_count;
}

std::size_t LineIndex::line_count() const noexcept
{
    return _line_starts.size();
}

std::size_t LineIndex::line_start(std::size_t line) const
{
    return _line_starts.at(line);
}

std::size_t LineIndex::line_byte_count(std::size_t line) const
{
    const auto start = line_start(line);
    const auto end = line == _line_starts.size() - 1
        ? _byte_count
        : _line_starts[line + 1];
    return end - start;
}

TextPosition LineIndex::position_at(std::size_t offset) const
{
    if (offset > _byte_count || _line_starts.empty()) {
        throw std::out_of_range("byte offset is outside the indexed text");
    }
    const auto next = std::upper_bound(_line_starts.begin(), _line_starts.end(), offset);
    const auto line = static_cast<std::size_t>(next - _line_starts.begin() - 1);
    return {line, offset - _line_starts[line]};
}

std::size_t LineIndex::offset_at(TextPosition position) const
{
    const auto count = line_byte_count(position.line);
    const bool final_line = position.line == _line_starts.size() - 1;
    if (position.column > count || (position.column == count && !final_line)) {
        throw std::out_of_range("byte column is outside the indexed line");
    }
    return _line_starts[position.line] + position.column;
}

} // namespace textedit
