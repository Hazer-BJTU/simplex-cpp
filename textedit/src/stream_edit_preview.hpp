#pragma once

// Internal preview assembly for the streaming edit path.
//
// The rows and coordinates of both texts are collected from one continuous
// re-read of the source while the replacement is verified. Both texts are
// tracked byte by byte, so a terminator that straddles the changed span is
// classified exactly as the in-memory renderer classifies it; nothing is
// stitched together from separate pieces.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fileio/staged_replace.hpp"

#include "rows.hpp"
#include "stream_edit_scan.hpp"

namespace textedit::detail {

/** Excerpt range of a change, clamped to the text's line count. */
inline detail::ExcerptRange range_for(
    std::size_t total_lines, std::size_t first_changed, std::size_t last_changed,
    std::size_t context)
{
    const auto last = last_changed + std::min(context, total_lines - 1 - last_changed);
    return {
        first_changed > context ? first_changed - context : 0,
        last,
        first_changed,
        last_changed
    };
}

/** Merge captured rows into one monotone table, keeping the first of a line. */
inline std::vector<CapturedRow> merge_rows(std::vector<CapturedRow> rows)
{
    std::sort(rows.begin(), rows.end(),
        [](const CapturedRow& left, const CapturedRow& right) { return left.line < right.line; });
    rows.erase(std::unique(rows.begin(), rows.end(),
        [](const CapturedRow& left, const CapturedRow& right) { return left.line == right.line; }),
        rows.end());
    return rows;
}

/** Rows retained around one side of the change. */
struct PreviewRows {
    std::deque<CapturedRow> before;  ///< rows completed before the match
    std::deque<CapturedRow> head;    ///< first rows of the changed span
    std::deque<CapturedRow> tail;    ///< last rows of the changed span
    std::deque<CapturedRow> after;   ///< rows completed after the changed span
};

/**
 * One tracked text: the source, or the text after the replacement.
 *
 * Bytes are fed in stream order. The tracker stops exactly at the match start
 * and at the last changed byte so those coordinates are exact even when a
 * terminator straddles the boundary.
 */
class PreviewSide {
public:
    PreviewSide(std::string_view middle, std::size_t changed_bytes)
        : middle_(middle), changed_bytes_(changed_bytes), tracker_(0, &collector_)
    {
        collector_.side_ = this;
    }

    PreviewSide(const PreviewSide&) = delete;
    PreviewSide& operator=(const PreviewSide&) = delete;

    void start(std::uintmax_t match_byte)
    {
        next_stop_ = match_byte;
        last_stop_ = changed_bytes_ == 0
            ? RowTracker::kNoStop : match_byte + changed_bytes_ - 1;
        if (match_byte == 0) advance();
    }

    void feed(std::string_view bytes)
    {
        for (;;) {
            tracker_.feed_until(bytes, next_stop_);
            bytes = {};
            if (next_stop_ == RowTracker::kNoStop || tracker_.offset() != next_stop_) return;
            advance();
        }
    }

    /** Resolve EOF, recording a milestone that falls on the final bytes. */
    void finish()
    {
        for (;;) {
            if (next_stop_ != RowTracker::kNoStop && tracker_.offset() < next_stop_) {
                tracker_.finish_until(next_stop_);
            }
            if (next_stop_ == RowTracker::kNoStop || tracker_.offset() != next_stop_) break;
            advance();
        }
        tracker_.finish();
    }

    [[nodiscard]] const std::optional<RowTracker::Snapshot>& position() const noexcept
    {
        return position_;
    }
    [[nodiscard]] std::size_t last_changed() const noexcept { return last_changed_; }
    [[nodiscard]] bool last_taken() const noexcept { return last_taken_; }
    [[nodiscard]] std::size_t total_lines() const noexcept { return tracker_.line() + 1; }
    [[nodiscard]] std::string_view middle() const noexcept { return middle_; }
    [[nodiscard]] const PreviewRows& rows() const noexcept { return rows_; }
    [[nodiscard]] PreviewRows& rows() noexcept { return rows_; }

    /** Fall back to the match row when the span has no bytes of its own. */
    void assume_last_changed() noexcept
    {
        if (!last_taken_ && position_.has_value()) last_changed_ = position_->line;
    }

    /** Record the focus window of the first and last changed rows. */
    void focus_windows(std::uintmax_t match_byte, std::string_view suffix_head);

private:
    struct Collector final : RowTracker::Listener {
        PreviewSide* side_ = nullptr;
        void row_completed(const CapturedRow& row) override { side_->completed(row); }
    };


    void advance()
    {
        if (!position_.has_value()) {
            position_ = tracker_.snapshot();
            next_stop_ = last_stop_;
            return;
        }
        if (!last_taken_ && last_stop_ != RowTracker::kNoStop) {
            last_taken_ = true;
            last_changed_ = tracker_.line();
        }
        next_stop_ = RowTracker::kNoStop;
    }

    void completed(const CapturedRow& row)
    {
        if (!position_.has_value()) {
            rows_.before.push_back(row);
            if (rows_.before.size() > kMaxContextLines) rows_.before.pop_front();
            return;
        }
        if (last_taken_ && row.line > last_changed_) {
            if (rows_.after.size() < kMaxContextLines) rows_.after.push_back(row);
            return;
        }
        if (rows_.head.size() < kMaxShownLines / 2) {
            rows_.head.push_back(row);
            return;
        }
        rows_.tail.push_back(row);
        if (rows_.tail.size() > kMaxShownLines / 2) rows_.tail.pop_front();
    }

    std::string_view middle_;
    std::size_t changed_bytes_ = 0;
    Collector collector_;
    RowTracker tracker_;
    PreviewRows rows_;
    std::optional<RowTracker::Snapshot> position_;
    std::uintmax_t next_stop_ = RowTracker::kNoStop;
    std::uintmax_t last_stop_ = RowTracker::kNoStop;
    std::size_t last_changed_ = 0;
    bool last_taken_ = false;
};

inline void PreviewSide::focus_windows(
    std::uintmax_t match_byte, std::string_view suffix_head)
{
    if (!position_.has_value()) return;
    const auto first = position_->line;
    const auto last = last_changed_;
    const auto middle = middle_;
    const auto last_byte = middle.empty() ? match_byte : match_byte + middle.size() - 1;
    const auto window_of = [&](const CapturedRow& row, std::size_t from) {
        const auto end = std::min(row.length, from + kRowWindowBytes);
        std::string text;
        if (from >= end) return text;
        text.reserve(end - from);
        const auto append = [&](std::string_view piece, std::size_t piece_offset) {
            if (piece.empty()) return;
            const auto begin = std::max(from, piece_offset);
            const auto stop = std::min(end, piece_offset + piece.size());
            if (begin < stop) text.append(piece, begin - piece_offset, stop - begin);
        };
        // Content offsets: the bytes before the match come from the row tail
        // held at the match, then the replaced span (or replacement), then the
        // bytes that survived after it.
        std::string_view prefix;
        std::size_t prefix_bytes = 0;
        if (row.start < match_byte) {
            const auto before = position_->length;
            const auto available = std::min(before, position_->tail.size());
            prefix = std::string_view(position_->tail).substr(position_->tail.size() - available);
            prefix_bytes = before - available;
        }
        const auto middle_from = row.start < match_byte ? 0 : row.start - match_byte;
        const auto middle_bytes = row.start < match_byte ? position_->length : 0;
        append(prefix, prefix_bytes);
        append(middle.substr(std::min(middle_from, middle.size())), middle_bytes);
        append(suffix_head, middle_bytes + middle.size());
        return text;
    };
    for (auto* rows : {&rows_.head, &rows_.tail}) {
        for (auto& row : *rows) {
            std::size_t focus = 0;
            if (row.line == first) focus = match_byte - row.start;
            else if (row.line == last && last_byte >= row.start) focus = last_byte - row.start;
            else continue;
            const auto from = focus_window_start(row.length, focus);
            if (from == 0) continue;
            row.window_from = from;
            row.window = window_of(row, from);
        }
    }
}

/** Preview of one edit, in final line and byte coordinates. */
struct EditPreview {
    TextPosition before_position;
    TextPosition after_position;
    ExcerptRange before_range;
    ExcerptRange after_range;
    std::vector<CapturedRow> before_rows;
    std::vector<CapturedRow> after_rows;
};

/**
 * Collects the preview of one edit from the verification re-read of the source.
 *
 * The verification reports the untouched prefix and the surviving suffix; the
 * replaced span and the replacement are supplied in memory. Both texts are
 * therefore tracked as continuous byte streams, which is what makes a terminator
 * straddling the changed span behave exactly like the in-memory renderer.
 */
class PreviewCapture final : public fileio::SourceObserver {
public:
    PreviewCapture(std::size_t match_byte, std::string_view old_text,
                   std::string_view new_text)
        : match_byte_(match_byte), source_(old_text, old_text.size()),
          edited_(new_text, new_text.size())
    {
        source_.start(match_byte_);
        edited_.start(match_byte_);
    }

    void observe(std::uintmax_t offset, std::string_view bytes) override
    {
        const auto span_end = match_byte_ + source_.middle().size();
        const auto feed_both = [&](std::string_view piece) {
            source_.feed(piece);
            edited_.feed(piece);
        };
        std::size_t used = 0;
        if (offset < match_byte_) {
            const auto take = static_cast<std::size_t>(
                std::min<std::uintmax_t>(bytes.size(), match_byte_ - offset));
            feed_both(bytes.substr(0, take));
            used = take;
        }
        if (offset + used < span_end) {
            // Bytes inside the replaced span are already held in memory.
            used += static_cast<std::size_t>(std::min<std::uintmax_t>(
                span_end - (offset + used), bytes.size() - used));
        }
        if (used >= bytes.size()) return;
        begin_span();
        feed_both(bytes.substr(used));
        if (suffix_head_.size() < kSuffixCaptureBytes) {
            suffix_head_.append(bytes.data() + used,
                std::min(kSuffixCaptureBytes - suffix_head_.size(), bytes.size() - used));
        }
    }

    void finish()
    {
        begin_span();
        source_.finish();
        edited_.finish();
        const auto& before = source_.position();
        const auto& after = edited_.position();
        if (!before.has_value() || !after.has_value()) {
            throw std::logic_error("edit preview could not locate the match");
        }
        source_.assume_last_changed();
        edited_.assume_last_changed();
        source_.focus_windows(match_byte_, suffix_head_);
        edited_.focus_windows(match_byte_, suffix_head_);
        before_position_ = {
            before->line, static_cast<std::size_t>(match_byte_ - before->start)};
        after_position_ = {
            after->line, static_cast<std::size_t>(match_byte_ - after->start)};
        before_total_ = source_.total_lines();
        after_total_ = edited_.total_lines();
    }

    /** Rows and ranges of the excerpt for the given context size. */
    [[nodiscard]] EditPreview take(std::size_t context) const
    {
        EditPreview preview;
        preview.before_position = before_position_;
        preview.after_position = after_position_;
        preview.before_range = range_for(
            before_total_, before_position_.line, source_.last_changed(), context);
        preview.after_range = range_for(
            after_total_, after_position_.line, edited_.last_changed(), context);
        preview.before_rows = table(
            source_.rows(), before_position_.line, source_.last_changed(), context);
        preview.after_rows = table(
            edited_.rows(), after_position_.line, edited_.last_changed(), context);
        return preview;
    }

private:
    void begin_span()
    {
        if (span_fed_) return;
        span_fed_ = true;
        source_.feed(source_.middle());
        edited_.feed(edited_.middle());
    }

    static std::vector<CapturedRow> table(
        const PreviewRows& rows, std::size_t first, std::size_t last, std::size_t context)
    {
        std::vector<CapturedRow> table;
        table.reserve(rows.head.size() + rows.tail.size() + 2 * context);
        table.insert(table.end(), rows.head.begin(), rows.head.end());
        table.insert(table.end(), rows.tail.begin(), rows.tail.end());
        std::size_t added = 0;
        for (auto row = rows.before.rbegin(); row != rows.before.rend() && added < context; ++row) {
            if (row->line >= first) continue;
            table.push_back(*row);
            ++added;
        }
        added = 0;
        for (const auto& row : rows.after) {
            if (row.line <= last || added >= context) continue;
            table.push_back(row);
            ++added;
        }
        return merge_rows(std::move(table));
    }

    std::size_t match_byte_ = 0;
    PreviewSide source_;
    PreviewSide edited_;
    std::string suffix_head_;
    TextPosition before_position_;
    TextPosition after_position_;
    std::size_t before_total_ = 1;
    std::size_t after_total_ = 1;
    bool span_fed_ = false;
};

} // namespace textedit::detail
