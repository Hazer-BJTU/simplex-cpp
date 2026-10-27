#pragma once

// Internal preview assembly for the streaming edit path: terminator accounting
// over bounded corpora, the changed-span row builder, excerpt ranges and the
// captured-row helpers shared with the in-memory renderer.

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "line_break.hpp"
#include "rows.hpp"
#include "stream_edit_scan.hpp"

namespace textedit::detail {

/** Number of terminators whose end offset lies in (from, to]. */
std::size_t count_terminator_ends(
    std::string_view corpus, std::size_t corpus_start,
    std::size_t from, std::size_t to)
{
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < corpus.size();) {
        const auto width = line_break_width(corpus.substr(offset));
        if (width == 0) {
            ++offset;
            continue;
        }
        const auto end = corpus_start + offset + width;
        if (end > from && end <= to) ++count;
        offset += width;
    }
    return count;
}

/** Append bytes while the accumulated display material stays within `limit`. */
void append_bounded(std::string& output, std::string_view bytes, std::size_t limit)
{
    if (output.size() >= limit || bytes.empty()) return;
    output.append(bytes.data(), std::min(limit - output.size(), bytes.size()));
}

/** Bounded pieces describing one changed span and its neighbouring bytes. */
struct SpanPieces {
    std::size_t first_line = 0;
    std::size_t match_byte = 0;
    /// Content bytes of the first changed row before the span.
    std::size_t prefix_length = 0;
    /// First kRowWindowBytes content bytes of that row.
    std::string_view prefix_head;
    /// Content bytes immediately before the span, from the row's tail.
    std::string_view prefix_tail;
    std::string_view middle;
    std::string_view suffix_head;
    /// Content bytes of the last changed row that lie past the span.
    std::size_t suffix_length = 0;
};

/**
 * Build the preview rows of one changed span.
 *
 * Only the first and last kMaxShownLines/2 rows are materialized, so a span of
 * millions of rows costs the same memory as a short one.
 */
std::vector<CapturedRow> build_span_rows(const SpanPieces& pieces)
{
    struct Start {
        std::size_t index;
        std::size_t offset;
    };
    std::vector<Start> head{{0, 0}};
    std::deque<Start> tail{{0, 0}};
    std::size_t index = 0;
    for (std::size_t offset = 0; offset < pieces.middle.size();) {
        const auto width = line_break_width(pieces.middle.substr(offset));
        if (width == 0) {
            ++offset;
            continue;
        }
        offset += width;
        ++index;
        if (offset < pieces.middle.size()) {
            if (head.size() < kMaxShownLines / 2) head.push_back({index, offset});
            tail.push_back({index, offset});
            if (tail.size() > kMaxShownLines / 2) tail.pop_front();
        }
    }
    const auto row_total = index + 1;
    std::vector<Start> wanted = head;
    for (const auto& start : tail) {
        const auto seen = std::any_of(wanted.begin(), wanted.end(),
            [&](const Start& other) { return other.index == start.index; });
        if (!seen) wanted.push_back(start);
    }
    std::sort(wanted.begin(), wanted.end(),
        [](const Start& left, const Start& right) { return left.index < right.index; });

    std::vector<CapturedRow> rows;
    rows.reserve(wanted.size());
    for (const auto& start : wanted) {
        const bool first = start.index == 0;
        const bool last = start.index + 1 == row_total;
        std::size_t next = pieces.middle.size();
        for (std::size_t offset = start.offset; offset < pieces.middle.size();) {
            const auto width = line_break_width(pieces.middle.substr(offset));
            if (width == 0) {
                ++offset;
                continue;
            }
            next = offset;
            break;
        }
        const auto part = pieces.middle.substr(start.offset, next - start.offset);
        const auto suffix = last
            ? pieces.suffix_head.substr(0, std::min(pieces.suffix_length, pieces.suffix_head.size()))
            : std::string_view{};
        const auto suffix_length = last ? pieces.suffix_length : 0;

        CapturedRow row;
        row.line = pieces.first_line + start.index;
        row.start = first ? pieces.match_byte - pieces.prefix_length
                          : pieces.match_byte + start.offset;
        row.length = part.size() + suffix_length + (first ? pieces.prefix_length : 0);
        // First and last changed rows carry a focus byte; other rows show their
        // head only, so their window start stays zero.

        if (first) append_bounded(row.head, pieces.prefix_head, kRowWindowBytes);
        append_bounded(row.head, part, kRowWindowBytes);
        append_bounded(row.head, suffix, kRowWindowBytes);

        const auto focus_column = first ? pieces.prefix_length
            : (last ? part.size() : 0);
        row.window_from = detail::focus_window_start(row.length, focus_column);
        if (row.window_from != 0) {
            if (first) {
                const auto skip = row.window_from;
                if (skip < pieces.prefix_length) {
                    // The window starts inside the prefix: only its tail is kept.
                    const auto missing = pieces.prefix_length - skip;
                    auto tail_bytes = pieces.prefix_tail;
                    if (tail_bytes.size() > missing) {
                        tail_bytes.remove_prefix(tail_bytes.size() - missing);
                    }
                    append_bounded(row.window, tail_bytes, kRowWindowBytes);
                    append_bounded(row.window, part, kRowWindowBytes);
                } else {
                    const auto inside = std::min(skip - pieces.prefix_length, part.size());
                    append_bounded(row.window, part.substr(inside), kRowWindowBytes);
                }
            } else {
                const auto inside = std::min(row.window_from, part.size());
                append_bounded(row.window, part.substr(inside), kRowWindowBytes);
            }
            append_bounded(row.window, suffix, kRowWindowBytes);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/** Excerpt range of a change, clamped to the text's line count. */
detail::ExcerptRange range_for(
    std::size_t total_lines, std::size_t first_changed, std::size_t last_changed,
    std::size_t context)
{
    return {
        first_changed > context ? first_changed - context : 0,
        last_changed + std::min(context, total_lines - 1 - last_changed),
        first_changed,
        last_changed
    };
}

/** Merge captured rows into one monotone table, keeping the first of a line. */
std::vector<CapturedRow> merge_rows(std::vector<CapturedRow> rows)
{
    std::sort(rows.begin(), rows.end(),
        [](const CapturedRow& left, const CapturedRow& right) { return left.line < right.line; });
    rows.erase(std::unique(rows.begin(), rows.end(),
        [](const CapturedRow& left, const CapturedRow& right) { return left.line == right.line; }),
        rows.end());
    return rows;
}

/** Collect newly completed rows from a tracker, bounded by kCapturedRowLimit. */
void harvest(
    const RowTracker& tracker, std::vector<CapturedRow>& out,
    std::size_t& next_line, std::size_t limit)
{
    for (const auto& row : tracker.recent()) {
        if (row.line < next_line) continue;
        next_line = row.line + 1;
        if (out.size() < limit) out.push_back(row);
    }
}

} // namespace textedit::detail
