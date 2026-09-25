#include "textedit/line_index.hpp"
#include "line_break.hpp"

#include <algorithm>
#include <stdexcept>

namespace textedit {
LineIndex::LineIndex(std::string_view text)
    : _byte_count(text.size()), _line_starts{0}
{
    for (std::size_t offset = 0; offset < text.size();) {
        const auto width = detail::line_break_width(text.substr(offset));
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
