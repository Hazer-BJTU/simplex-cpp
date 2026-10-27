#pragma once

// Bounded logical-row access and excerpt rendering shared by the in-memory and
// streaming edit paths. One renderer keeps their previews byte-identical.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "display.hpp"
#include "line_break.hpp"

namespace textedit::detail {

constexpr std::size_t kMaxContextLines = 20;
constexpr std::size_t kMaxShownLines = 48;
constexpr std::size_t kMaxLineDisplayBytes = 512;
/// Content bytes kept per shown row: the display budget plus scalar lookahead.
constexpr std::size_t kRowWindowBytes = kMaxLineDisplayBytes + 4;

/** Row window of an excerpt plus the changed span it marks. */
struct ExcerptRange {
    std::size_t first = 0;
    std::size_t last = 0;
    std::size_t changed_first = 0;
    std::size_t changed_last = 0;
};

/** Bytes to skip before a row's display window starts. */
inline std::size_t focus_window_start(std::size_t length, std::size_t focus_column)
{
    return length > kMaxLineDisplayBytes && focus_column > 128
        ? focus_column - 128 : 0;
}

inline std::string padded(std::size_t value, std::size_t width)
{
    auto digits = std::to_string(value);
    return std::string(width - digits.size(), ' ') + digits;
}

/**
 * Forward-only access to the logical rows of one text.
 *
 * Callers seek monotonically and read only the rows they display, so a cursor
 * may derive rows lazily. content(from) returns at most kRowWindowBytes bytes
 * starting at `from`, where `from` is 0 or a previously computed focus window
 * start; the view is only valid until the next seek.
 */
class RowCursor {
public:
    virtual ~RowCursor() = default;
    virtual void seek(std::size_t line) = 0;
    [[nodiscard]] virtual std::size_t line() const = 0;
    /** Absolute byte offset of the current row's first content byte. */
    [[nodiscard]] virtual std::size_t start() const = 0;
    /** Content bytes of the current row, excluding its terminator. */
    [[nodiscard]] virtual std::size_t length() const = 0;
    [[nodiscard]] virtual std::string_view content(std::size_t from) const = 0;
};

/** Row access over a borrowed text view, walking terminators as it advances. */
class TextViewCursor final : public RowCursor {
public:
    explicit TextViewCursor(std::string_view text) : text_(text)
    {
        start_ = 0;
        next_ = 0;
        measure();
    }

    void seek(std::size_t line) override
    {
        while (current_ < line && next_ > start_) {
            start_ = next_;
            ++current_;
            measure();
        }
    }

    [[nodiscard]] std::size_t line() const override { return current_; }
    [[nodiscard]] std::size_t start() const override { return start_; }

    [[nodiscard]] std::size_t length() const override { return row_end_ - start_; }

    [[nodiscard]] std::string_view content(std::size_t from) const override
    {
        const auto size = length();
        if (from >= size) return {};
        return text_.substr(start_ + from, std::min(kRowWindowBytes, size - from));
    }

private:
    /// Locate the terminator of the row starting at start_.
    void measure()
    {
        row_end_ = text_.size();
        next_ = text_.size();
        for (auto offset = start_; offset < text_.size();) {
            const auto width = line_break_width(text_.substr(offset));
            if (width == 0) {
                ++offset;
                continue;
            }
            row_end_ = offset;
            next_ = offset + width;
            return;
        }
    }

    std::string_view text_;
    std::size_t current_ = 0;
    std::size_t start_ = 0;
    std::size_t row_end_ = 0;
    std::size_t next_ = 0;
};

/** Append one physical line with the stable marker/number/pipe gutter. */
inline void append_line(
    std::string& output,
    RowCursor& cursor,
    std::size_t number,
    std::size_t width,
    char marker,
    std::size_t focus_column,
    bool& clipped)
{
    const auto length = cursor.length();
    if (!output.empty()) output += '\n';
    output += marker;
    output += ' ';
    output += padded(number, width);
    output += " | ";
    const auto shown_from = focus_window_start(length, focus_column);
    if (shown_from > 0) {
        output += "[... " + std::to_string(shown_from) + " bytes skipped ...] ";
        clipped = true;
    }
    DisplayWriter writer(kMaxLineDisplayBytes);
    writer.append(cursor.content(shown_from));
    output += writer.text();
    if (writer.truncated() || shown_from + writer.consumed() < length) {
        output += " ... [line clipped]";
        clipped = true;
    }
}

/**
 * Render one excerpt.
 *
 * Rows outside the changed span use the unchanged marker; rows inside use
 * `changed_marker`. Spans longer than kMaxShownLines keep their first and last
 * kMaxShownLines/2 rows with an explicit omission row, so a large span never
 * grows memory with its length.
 */
inline std::string render_excerpt(
    RowCursor& cursor,
    const ExcerptRange& range,
    std::size_t width,
    char changed_marker,
    std::size_t changed_start_byte,
    std::size_t changed_bytes,
    bool& clipped)
{
    const auto rows = range.last - range.first + 1;
    const bool omit_middle = rows > kMaxShownLines;
    const auto head_end = range.first + kMaxShownLines / 2;
    const auto tail_start = range.last - kMaxShownLines / 2 + 1;
    std::string output;
    for (auto number = range.first;; ++number) {
        if (omit_middle && number == head_end) {
            if (!output.empty()) output += '\n';
            output += "! " + std::string(width, ' ') + " | ... " +
                      std::to_string(tail_start - head_end) + " lines omitted ...";
            clipped = true;
        }
        if (!omit_middle || number < head_end || number >= tail_start) {
            cursor.seek(number);
            const auto marker = number >= range.changed_first &&
                                number <= range.changed_last
                ? changed_marker : ' ';
            std::size_t focus_column = 0;
            if (number == range.changed_first) {
                focus_column = changed_start_byte >= cursor.start()
                    ? changed_start_byte - cursor.start() : 0;
            } else if (number == range.changed_last && changed_bytes > 0) {
                const auto last = changed_start_byte + changed_bytes - 1;
                focus_column = last >= cursor.start() ? last - cursor.start() : 0;
            }
            append_line(output, cursor, number, width, marker, focus_column, clipped);
        }
        if (number >= range.last) break;
    }
    return output;
}

} // namespace textedit::detail
