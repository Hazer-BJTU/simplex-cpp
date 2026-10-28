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

/** Terminator probe result at one position of a lookahead window. */
struct BreakProbe {
    /// Complete terminator width, or zero when this byte starts content.
    std::size_t width = 0;
    /// The window ends inside a sequence that may still become a longer
    /// terminator; the caller must carry those bytes into the next window.
    bool incomplete = false;
};

/**
 * Probe a terminator starting at window[at].
 *
 * The caller must keep at least two bytes of lookahead after `at` when it wants
 * a decisive answer without carrying bytes. At EOF, line_break_width resolves
 * the same suffix with final semantics: a lone CR is a terminator and a
 * truncated UTF-8 prefix is content.
 */
inline BreakProbe probe_break(std::string_view window, std::size_t at)
{
    switch (static_cast<unsigned char>(window[at])) {
        case '\r':
            if (at + 1 == window.size()) return {0, true};
            return {window[at + 1] == '\n' ? 2u : 1u, false};
        case '\n':
        case '\v':
        case '\f':
            return {1, false};
        case 0xc2:
            if (at + 1 == window.size()) return {0, true};
            return {static_cast<unsigned char>(window[at + 1]) == 0x85 ? 2u : 0u, false};
        case 0xe2:
            if (at + 1 == window.size()) return {0, true};
            if (static_cast<unsigned char>(window[at + 1]) != 0x80) return {0, false};
            if (at + 2 == window.size()) return {0, true};
            {
                const auto third = static_cast<unsigned char>(window[at + 2]);
                return {third == 0xa8 || third == 0xa9 ? 3u : 0u, false};
            }
        default:
            return {0, false};
    }
}

} // namespace textedit::detail
