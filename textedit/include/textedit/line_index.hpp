#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace textedit {

/** Zero-based line and byte column, not a Unicode or display-cell coordinate. */
struct TextPosition {
    std::size_t line = 0;
    std::size_t column = 0;

    bool operator==(const TextPosition&) const = default;
};

/**
 * Immutable line layout of the bytes supplied at construction.
 *
 * Recognizes LF, CRLF (one break), CR, VT, FF, and UTF-8 encodings of NEL
 * (U+0085), LS (U+2028) and PS (U+2029). Every byte of a terminator belongs to
 * the line it ends. Other bytes, including UTF-8 continuation bytes, NUL and
 * malformed UTF-8, each occupy one column. No decoding or normalization occurs;
 * raw 0x85 alone is not a UTF-8 NEL. The UTF-8 newline byte patterns are recognized
 * even in otherwise malformed input; this class does not validate encoding.
 *
 * There is always at least one line. A final terminator introduces a trailing
 * zero-byte line; empty input likewise has one zero-byte line. EOF is a valid
 * position: the final line at its byte count. Other lines do not accept a column
 * equal to their byte count, so each offset in [0, byte_count()] has exactly one
 * coordinate and round-trips without aliases.
 *
 * Construction is O(bytes); storage is O(lines). Offset-to-position lookup is
 * O(log lines); reverse lookup and per-line statistics are O(1). The original
 * text is neither copied nor retained and may be destroyed after construction.
 * If its contents change, build a new index before applying coordinates to it.
 * Concurrent const lookups are safe while this object remains alive and is not
 * assigned to or moved from. Construction may throw allocation/length errors.
 */
class LineIndex {
public:
    /// Build the layout once; the view is borrowed only for this constructor.
    explicit LineIndex(std::string_view text);

    /// Total original byte length; also the valid one-past-the-last-byte offset.
    [[nodiscard]] std::size_t byte_count() const noexcept;

    /// Number of logical lines, including any trailing empty line.
    [[nodiscard]] std::size_t line_count() const noexcept;

    /// Absolute start of a line. Throws std::out_of_range for an invalid line.
    [[nodiscard]] std::size_t line_start(std::size_t line) const;

    /// Bytes in a line, including its complete terminator (if any).
    /// Throws std::out_of_range for an invalid line.
    [[nodiscard]] std::size_t line_byte_count(std::size_t line) const;

    /// Map a byte offset, including EOF, to a unique line and byte column.
    /// Throws std::out_of_range if offset exceeds byte_count().
    [[nodiscard]] TextPosition position_at(std::size_t offset) const;

    /// Map a coordinate back to its byte offset. Only the final line accepts
    /// its end column (EOF). Throws std::out_of_range for an invalid coordinate.
    [[nodiscard]] std::size_t offset_at(TextPosition position) const;

private:
    std::size_t _byte_count;
    std::vector<std::size_t> _line_starts;
};

} // namespace textedit
