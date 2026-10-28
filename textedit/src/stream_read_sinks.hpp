#pragma once

// Internal selection sinks for the streaming readers: the scanner reports every
// logical row, and each sink keeps only what its result format needs. Nothing
// here is installed or exported through textedit's public headers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace textedit::detail {

/** Retain the raw bytes of a line selection, for LineReadFormat::Plain. */
struct PlainSelectionSink {
    std::size_t start_line = 0;
    std::size_t line_count = 0;
    std::size_t cap = 0;
    std::string bytes;
    bool started = false;
    std::optional<std::size_t> start_byte;
    std::optional<std::size_t> end_byte;

    PlainSelectionSink(std::size_t first_line, std::size_t rows, std::size_t budget)
        : start_line(first_line), line_count(rows), cap(budget) {}

    void begin_row(std::size_t line, std::size_t start)
    {
        if (line == start_line) {
            start_byte = start;
            started = true;
        }
        if (started && !end_byte && line >= start_line && line - start_line == line_count) {
            end_byte = start;
        }
    }

    void content(std::size_t offset, std::string_view piece) { keep(offset, piece); }
    void terminator(std::size_t offset, std::string_view terminator) { keep(offset, terminator); }

    void finish(std::size_t total)
    {
        if (!started) {
            start_byte = total;
            end_byte = total;
        } else if (!end_byte) {
            end_byte = total;
        }
    }

private:
    void keep(std::size_t, std::string_view piece)
    {
        if (!started || end_byte || piece.empty()) return;
        const auto room = cap - bytes.size();
        bytes.append(piece.data(), std::min(room, piece.size()));
    }
};

/**
 * Collect indexed rows as one '\n'-joined buffer plus one terminator width each.
 *
 * Rows never contain a terminator, so a trailing '\n' after each fully stored
 * row makes the layout self-describing without a per-row offset vector. The cap
 * bounds every retained byte, including the row separators: an empty row still
 * costs its separator, so a file of nothing but newlines cannot grow the sink
 * past the display budget. A row cut by the cap keeps its true length so
 * byte-range labels stay exact.
 */
struct RowSelectionSink {
    std::size_t start_line = 0;
    std::size_t line_count = 0;
    std::size_t cap = 0;
    std::string stored_content;
    std::vector<std::uint8_t> terminators;
    std::size_t stored = 0;
    bool clipped = false;
    std::size_t clipped_length = 0;
    std::size_t clipped_terminator = 0;
    bool started = false;
    std::optional<std::size_t> start_byte;
    std::optional<std::size_t> end_byte;
    bool selected = false;
    bool clip_recorded = false;
    std::size_t length = 0;

    RowSelectionSink(std::size_t first_line, std::size_t rows, std::size_t budget)
        : start_line(first_line), line_count(rows), cap(budget) {}

    void begin_row(std::size_t line, std::size_t start)
    {
        selected = line >= start_line && line - start_line < line_count;
        length = 0;
        if (line == start_line) {
            start_byte = start;
            started = true;
        }
        if (started && !end_byte && line >= start_line && line - start_line == line_count) {
            end_byte = start;
        }
    }

    void content(std::size_t, std::string_view piece)
    {
        if (!selected || clip_recorded || piece.empty()) return;
        length += piece.size();
        if (clipped) return;
        const auto take = std::min(cap - stored, piece.size());
        stored_content.append(piece.data(), take);
        stored += take;
        if (take != piece.size()) clipped = true;
    }

    void terminator(std::size_t, std::string_view terminator)
    {
        if (selected && !clip_recorded) {
            if (!clipped && stored < cap) {
                terminators.push_back(static_cast<std::uint8_t>(terminator.size()));
                stored_content += '\n';
                ++stored;
            } else {
                clipped = true;
                clipped_length = length;
                clipped_terminator = terminator.size();
                clip_recorded = true;
            }
        }
        selected = false;
        length = 0;
    }

    void finish(std::size_t total)
    {
        if (!started) {
            start_byte = total;
            end_byte = total;
        } else if (!end_byte) {
            end_byte = total;
        }
        if (selected && !clip_recorded) {
            if (!clipped && stored < cap) {
                // A final row without a terminator is still a complete stored row.
                terminators.push_back(0);
                stored_content += '\n';
                ++stored;
            } else {
                clipped = true;
                clipped_length = length;
                clipped_terminator = 0;
                clip_recorded = true;
            }
        }
        selected = false;
        length = 0;
    }
};

/** Retain a byte range of the raw stream, for ByteReadFormat reads. */
struct ByteSelectionSink {
    std::size_t start = 0;
    std::size_t count = 0;
    std::size_t cap = 0;
    std::string bytes;

    ByteSelectionSink(std::size_t first_byte, std::size_t bytes_wanted, std::size_t budget)
        : start(first_byte), count(bytes_wanted), cap(budget) {}

    void begin_row(std::size_t, std::size_t) {}
    void content(std::size_t offset, std::string_view piece) { keep(offset, piece); }
    void terminator(std::size_t offset, std::string_view terminator) { keep(offset, terminator); }
    void finish(std::size_t) {}

private:
    void keep(std::size_t offset, std::string_view piece)
    {
        if (piece.empty()) return;
        const auto skip = offset <= start ? start - offset : 0;
        if (skip >= piece.size()) return;
        const auto position = offset <= start ? 0 : offset - start;
        if (position >= count) return;
        const auto take = std::min(piece.size() - skip, count - position);
        const auto room = cap - bytes.size();
        bytes.append(piece.data() + skip, std::min(take, room));
    }
};

} // namespace textedit::detail
