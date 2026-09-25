#pragma once

#include <cstddef>
#include <string_view>

namespace textedit::detail {

/** Complete newline width at a nonempty byte suffix, or zero. */
inline std::size_t line_break_width(std::string_view remaining)
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

/** Count logical lines without retaining their offsets. */
inline std::size_t count_lines(std::string_view text)
{
    std::size_t lines = 1;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto width = line_break_width(text.substr(offset));
        if (width == 0) {
            ++offset;
        } else {
            offset += width;
            ++lines;
        }
    }
    return lines;
}

} // namespace textedit::detail
