#include "textedit/edit.hpp"

#include "fileio/atomic_write.hpp"
#include "fileio/replace_existing.hpp"
#include "fileio/staged_replace.hpp"

#include "line_break.hpp"
#include "rows.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace textedit {
namespace {

using detail::kMaxContextLines;
using detail::kMaxShownLines;
using detail::kRowWindowBytes;
using detail::line_break_width;
using detail::probe_break;
using detail::render_excerpt;

/// Raw bytes kept before the match: enough for a split terminator.
constexpr std::size_t kRawTailBytes = 3;
/// Content bytes of a row kept before the match for a focused window.
constexpr std::size_t kFocusTailBytes = 128 + 4;
/// Stream bytes after the span captured for the last changed row.
constexpr std::size_t kSuffixCaptureBytes = kRowWindowBytes + 4;
/// Completed rows recorded after a change: the last changed row plus its context.
constexpr std::size_t kCapturedRowLimit = kMaxContextLines + 2;
/// Largest change treated as a single "modified" span for preview purposes.
constexpr std::size_t kRecentRowLimit = kMaxContextLines;

/** A fixed-capacity byte ring, for lookbehind without unbounded copying. */
class ByteRing {
public:
    explicit ByteRing(std::size_t capacity) : bytes_(capacity) {}

    void push(unsigned char byte)
    {
        bytes_[next_] = byte;
        next_ = (next_ + 1) % bytes_.size();
        if (count_ < bytes_.size()) ++count_;
    }

    void clear()
    {
        next_ = 0;
        count_ = 0;
    }

    [[nodiscard]] std::string ordered() const
    {
        std::string text(count_, '\0');
        for (std::size_t index = 0; index < count_; ++index) {
            text[index] = static_cast<char>(
                bytes_[(next_ + bytes_.size() - count_ + index) % bytes_.size()]);
        }
        return text;
    }

private:
    std::vector<unsigned char> bytes_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

/** One logical row captured with bounded display material. */
struct CapturedRow {
    std::size_t line = 0;
    std::size_t start = 0;
    std::size_t length = 0;
    std::string head;
    std::size_t window_from = 0;
    std::string window;
};

/** Row provider over captured rows; callers seek monotonically. */
class TableCursor final : public detail::RowCursor {
public:
    explicit TableCursor(std::vector<CapturedRow> rows) : rows_(std::move(rows)) {}

    void seek(std::size_t line) override
    {
        while (index_ < rows_.size() && rows_[index_].line < line) ++index_;
        if (index_ >= rows_.size() || rows_[index_].line != line) {
            // The tables are built to cover exactly the rendered range, so a
            // missing row would silently show the wrong content instead.
            throw std::logic_error("preview row is missing from the captured table");
        }
    }

    [[nodiscard]] std::size_t line() const override { return rows_[index_].line; }
    [[nodiscard]] std::size_t start() const override { return rows_[index_].start; }
    [[nodiscard]] std::size_t length() const override { return rows_[index_].length; }

    [[nodiscard]] std::string_view content(std::size_t from) const override
    {
        const auto& row = rows_[index_];
        if (from == 0) return row.head;
        if (from == row.window_from) return row.window;
        return {};
    }

private:
    std::vector<CapturedRow> rows_;
    std::size_t index_ = 0;
};

/**
 * Row coordinates of one byte stream, advanced in order.
 *
 * push() handles one byte with up to two lookahead bytes, so a caller can take
 * an exact snapshot at any offset. A row ends when the last byte of its
 * terminator is consumed, which matches position_of()'s "terminator contained
 * in [0, offset)" rule. Retained state is the current row's head, a short tail
 * for focused windows, and the most recent completed rows.
 */
class RowTracker {
public:
    struct Snapshot {
        std::size_t line = 0;
        std::size_t start = 0;
        std::size_t length = 0;
        std::string head;
        std::string tail;
    };

    explicit RowTracker(std::uintmax_t start_offset = 0)
        : offset_(start_offset), row_start_(start_offset), tail_(kFocusTailBytes)
    {}

    /**
     * Process window[at] with window[at+1] and window[at+2] as lookahead.
     *
     * The caller guarantees those two bytes except in the final positions,
     * which push_final() handles with end-of-file semantics.
     */
    void push(std::string_view window, std::size_t at)
    {
        const auto byte = static_cast<unsigned char>(window[at]);
        std::size_t width = 0;
        if (pending_left_ == 0) width = probe_break(window, at).width;
        if (pending_left_ == 0 && width == 0) {
            ++length_;
            if (head_.size() < kRowWindowBytes) head_ += static_cast<char>(byte);
            tail_.push(byte);
        }
        if (pending_left_ == 0 && width != 0) pending_left_ = width;
        if (pending_left_ > 0 && --pending_left_ == 0) complete_row();
        ++offset_;
    }

    /** Process one byte at EOF, where a lone CR ends a row and a truncated
     * UTF-8 newline prefix is content. */
    void push_final(std::string_view window, std::size_t at)
    {
        const auto byte = static_cast<unsigned char>(window[at]);
        std::size_t width = 0;
        if (pending_left_ == 0) width = line_break_width(window.substr(at));
        if (pending_left_ == 0 && width == 0) {
            ++length_;
            if (head_.size() < kRowWindowBytes) head_ += static_cast<char>(byte);
            tail_.push(byte);
        }
        if (pending_left_ == 0 && width != 0) pending_left_ = width;
        if (pending_left_ > 0 && --pending_left_ == 0) complete_row();
        ++offset_;
    }

    /** Feed a chunk, carrying the last two bytes for lookahead. */
    void feed(std::string_view bytes)
    {
        carry_buffer_.assign(carry_);
        carry_buffer_.append(bytes);
        carry_.clear();
        const auto processable = carry_buffer_.size() > 2 ? carry_buffer_.size() - 2 : 0;
        for (std::size_t index = 0; index < processable; ++index) {
            push(carry_buffer_, index);
        }
        carry_.assign(carry_buffer_.substr(processable));
    }

    /** Resolve a trailing partial terminator at EOF. */
    void finish()
    {
        std::string_view remaining(carry_);
        while (!remaining.empty()) {
            if (pending_left_ > 0) {
                if (--pending_left_ == 0) complete_row();
                remaining.remove_prefix(1);
                ++offset_;
                continue;
            }
            const auto width = line_break_width(remaining);
            if (width != 0) {
                pending_left_ = width;
                continue;
            }
            ++length_;
            if (head_.size() < kRowWindowBytes) head_ += remaining.front();
            tail_.push(static_cast<unsigned char>(remaining.front()));
            remaining.remove_prefix(1);
            ++offset_;
        }
        carry_.clear();
        // The final row has no terminator, so record it explicitly for capture.
        recent_.push_back(CapturedRow{line_, static_cast<std::size_t>(row_start_),
                                      length_, head_, 0, {}});
        if (recent_.size() > kRecentRowLimit) recent_.pop_front();
        ++generation_;
    }

    [[nodiscard]] std::size_t line() const noexcept { return line_; }
    [[nodiscard]] std::uintmax_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t generation() const noexcept { return generation_; }

    [[nodiscard]] Snapshot snapshot() const
    {
        return {line_, static_cast<std::size_t>(row_start_), length_, head_, tail_.ordered()};
    }

    /** Most recent completed rows, oldest first. */
    [[nodiscard]] const std::deque<CapturedRow>& recent() const noexcept { return recent_; }

private:
    void complete_row()
    {
        CapturedRow row;
        row.line = line_;
        row.start = static_cast<std::size_t>(row_start_);
        row.length = length_;
        row.head = head_;
        recent_.push_back(std::move(row));
        if (recent_.size() > kRecentRowLimit) recent_.pop_front();
        ++line_;
        row_start_ = offset_ + 1;
        length_ = 0;
        head_.clear();
        tail_.clear();
        ++generation_;
    }

    std::uintmax_t offset_ = 0;
    std::uintmax_t row_start_ = 0;
    std::size_t line_ = 0;
    std::size_t length_ = 0;
    std::size_t pending_left_ = 0;
    std::size_t generation_ = 0;
    std::string head_;
    ByteRing tail_;
    std::deque<CapturedRow> recent_;
    std::string carry_;
    std::string carry_buffer_;
};

/** Buffered candidate writes: one syscall per flush instead of per byte. */
class CandidateWriter {
public:
    explicit CandidateWriter(fileio::StagedReplacement& staged) : staged_(staged) {}

    void append(std::string_view bytes)
    {
        if (!enabled_) return;
        if (buffer_.size() + bytes.size() < kFlushBytes) {
            buffer_.append(bytes);
            return;
        }
        flush();
        if (bytes.size() >= kFlushBytes) {
            staged_.write(bytes);
            return;
        }
        buffer_.append(bytes);
    }

    void append_byte(unsigned char byte) { append(std::string_view(reinterpret_cast<char*>(&byte), 1)); }

    void flush()
    {
        if (!buffer_.empty()) {
            staged_.write(buffer_);
            buffer_.clear();
        }
    }

    void enable(bool value) { enabled_ = value; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

private:
    static constexpr std::size_t kFlushBytes = 64 * 1024;

    fileio::StagedReplacement& staged_;
    std::string buffer_;
    bool enabled_ = true;
};

/** Streaming KMP matcher that emits the original stream with one span replaced. */
class StreamingMatcher {
public:
    StreamingMatcher(std::string_view pattern, std::string_view replacement,
                     CandidateWriter& writer)
        : pattern_(pattern), replacement_(replacement), writer_(writer),
          prefix_(kmp_prefix(pattern))
    {}

    void step(unsigned char byte, std::size_t offset)
    {
        if (stopped_) {
            writer_.append_byte(byte);
            return;
        }
        if (!found_) {
            while (q_ > 0 && static_cast<unsigned char>(pattern_[q_]) != byte) {
                emit(q_ - prefix_[q_ - 1]);
                q_ = prefix_[q_ - 1];
            }
            if (static_cast<unsigned char>(pattern_[q_]) == byte) {
                if (q_ == 0) start_event_ = true;
                pending_.push_back(static_cast<char>(byte));
                ++q_;
            } else {
                writer_.append_byte(byte);
            }
        } else {
            writer_.append_byte(byte);
            while (q_ > 0 && static_cast<unsigned char>(pattern_[q_]) != byte) {
                q_ = prefix_[q_ - 1];
            }
            if (static_cast<unsigned char>(pattern_[q_]) == byte) ++q_;
        }
        if (q_ == pattern_.size()) complete(offset);
    }

    /** True once for the step that started a potential match. */
    [[nodiscard]] bool take_start_event() noexcept
    {
        return std::exchange(start_event_, false);
    }

    [[nodiscard]] bool found() const noexcept { return found_; }
    [[nodiscard]] bool ambiguous() const noexcept { return ambiguous_; }
    [[nodiscard]] std::size_t match_byte() const noexcept { return match_byte_; }
    [[nodiscard]] std::size_t second_match_byte() const noexcept { return second_match_byte_; }
    [[nodiscard]] const std::string& span() const noexcept { return span_; }

    /** Stop emitting generated bytes; used by no-op and ambiguous results. */
    void stop_writing() { writer_.enable(false); }

private:
    static std::vector<std::size_t> kmp_prefix(std::string_view pattern)
    {
        std::vector<std::size_t> prefix(pattern.size(), 0);
        for (std::size_t index = 1, length = 0; index < pattern.size();) {
            if (pattern[index] == pattern[length]) {
                prefix[index++] = ++length;
            } else if (length != 0) {
                length = prefix[length - 1];
            } else {
                prefix[index++] = 0;
            }
        }
        return prefix;
    }

    void emit(std::size_t count)
    {
        writer_.append(std::string_view(pending_).substr(begin_, count));
        begin_ += count;
        if (begin_ > 4096 && begin_ * 2 > pending_.size()) {
            pending_.erase(0, begin_);
            begin_ = 0;
        }
    }

    void complete(std::size_t offset)
    {
        const auto start = offset + 1 - pattern_.size();
        if (!found_) {
            found_ = true;
            match_byte_ = start;
            span_.assign(pending_, begin_, pattern_.size());
            pending_.clear();
            begin_ = 0;
            writer_.append(replacement_);
            q_ = prefix_[pattern_.size() - 1];
        } else {
            // Only the first second-match offset is reported, and the candidate
            // is useless afterwards, so matching stops here.
            ambiguous_ = true;
            second_match_byte_ = start;
            q_ = 0;
            stopped_ = true;
            writer_.enable(false);
        }
    }

    std::string_view pattern_;
    std::string_view replacement_;
    CandidateWriter& writer_;
    std::vector<std::size_t> prefix_;
    std::string pending_;
    std::string span_;
    std::size_t begin_ = 0;
    std::size_t q_ = 0;
    std::size_t match_byte_ = 0;
    std::size_t second_match_byte_ = 0;
    bool found_ = false;
    bool ambiguous_ = false;
    bool stopped_ = false;
    bool start_event_ = false;
};

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

} // namespace

EditResult str_replace_file(
    const std::filesystem::path& path,
    std::string_view old_text,
    std::string_view new_text,
    std::size_t context_lines,
    std::size_t max_file_bytes)
{
    if (old_text.empty()) {
        throw std::invalid_argument("old_text must not be empty");
    }
    if (context_lines > kMaxContextLines) {
        throw std::invalid_argument("context_lines must not exceed 20");
    }
    if (max_file_bytes == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("file byte limit must leave room for one lookahead byte");
    }

    EditResult result;
    fileio::StagedReplacement staged(path);
    CandidateWriter writer(staged);
    StreamingMatcher matcher(old_text, new_text, writer);
    RowTracker tracker;
    RowTracker edited;
    ByteRing raw_tail(kRawTailBytes);
    std::string match_raw_tail;
    std::optional<RowTracker::Snapshot> snapshot;
    std::deque<CapturedRow> pre_snapshot;
    std::size_t pre_generation = std::numeric_limits<std::size_t>::max();
    std::vector<CapturedRow> captured;
    std::vector<CapturedRow> edited_captured;
    std::size_t captured_next = 0;
    std::size_t edited_next = 0;
    std::string suffix_head;
    bool edited_active = false;
    std::size_t before_gate = 0;
    std::size_t edited_gate = 0;

    // One byte of the source: matcher, row coordinates and pre/post capture.
    std::size_t suffix_from = 0;
    const auto process_byte = [&](std::string_view window, std::size_t index,
                                  std::size_t offset, bool final) {
        const auto byte = static_cast<unsigned char>(window[index]);
        matcher.step(byte, offset);
        if (matcher.take_start_event()) {
            snapshot = tracker.snapshot();
            if (pre_generation != tracker.generation()) {
                pre_snapshot = tracker.recent();
                pre_generation = tracker.generation();
            }
            match_raw_tail = raw_tail.ordered();
        }
        if (!edited_active && matcher.found() &&
            offset + 1 == matcher.match_byte() + old_text.size()) {
            edited_active = true;
            suffix_from = index + 1;
            before_gate = matcher.match_byte() + old_text.size();
            edited_gate = matcher.match_byte() + new_text.size();
            captured_next = tracker.line();
        }
        if (final) tracker.push_final(window, index);
        else tracker.push(window, index);
        raw_tail.push(byte);
        if (edited_active && tracker.offset() > before_gate) {
            harvest(tracker, captured, captured_next, kCapturedRowLimit);
        }
    };

    // Feed the edited stream (replacement + suffix) once the span is complete.
    const auto finish_window = [&](std::string_view window, std::size_t limit) {
        if (!edited_active) return;
        if (suffix_from < limit) {
            // Only the processed bytes are suffix: the trailing carry is fed
            // again by the next window, or once at EOF.
            const auto rest = window.substr(suffix_from, limit - suffix_from);
            if (edited.offset() == 0) {
                edited = RowTracker(matcher.match_byte() - match_raw_tail.size());
                edited.feed(match_raw_tail);
                edited.feed(new_text);
                edited_next = edited.line();
            }
            edited.feed(rest);
            append_bounded(suffix_head, rest, kSuffixCaptureBytes);
        }
        if (edited.offset() > edited_gate) {
            harvest(edited, edited_captured, edited_next, kCapturedRowLimit);
        }
    };

    std::string window;
    std::string carry;
    std::string trailing;
    for (;;) {
        const auto chunk = staged.next_source();
        if (chunk.empty()) break;
        if (staged.source_bytes() > max_file_bytes) {
            throw std::length_error("file exceeds the configured byte limit");
        }
        window.assign(carry);
        window.append(chunk);
        const auto carried = window.size() - chunk.size();
        carry.clear();
        const auto base = static_cast<std::size_t>(staged.source_bytes())
            - chunk.size() - carried;
        const auto processable = window.size() > 2 ? window.size() - 2 : 0;
        // A window that starts after the span is entirely suffix bytes.
        suffix_from = edited_active ? 0 : window.size();
        for (std::size_t index = 0; index < processable; ++index) {
            process_byte(window, index, base + index, false);
        }
        finish_window(window, processable);
        carry.assign(window.substr(processable));
    }
    // The last two bytes had no lookahead; resolve them with EOF semantics.
    if (!carry.empty()) {
        const auto base = static_cast<std::size_t>(staged.source_bytes()) - carry.size();
        suffix_from = edited_active ? 0 : carry.size();
        for (std::size_t index = 0; index < carry.size(); ++index) {
            process_byte(carry, index, base + index, true);
        }
        finish_window(carry, carry.size());
    }
    tracker.finish();
    if (edited_active) {
        if (edited.offset() == 0) {
            edited = RowTracker(matcher.match_byte() - match_raw_tail.size());
            edited.feed(match_raw_tail);
            edited.feed(new_text);
        }
        edited.finish();
    }
    if (edited_active) {
        harvest(tracker, captured, captured_next, kCapturedRowLimit);
        harvest(edited, edited_captured, edited_next, kCapturedRowLimit);
    }

    const auto source_bytes = static_cast<std::size_t>(staged.source_bytes());
    result.before_bytes = source_bytes;
    result.after_bytes = source_bytes;
    result.matches_at_least = matcher.found() ? (matcher.ambiguous() ? 2u : 1u) : 0u;
    result.match_byte = matcher.match_byte();
    if (matcher.ambiguous()) {
        result.status = EditStatus::Ambiguous;
        result.second_match_byte = matcher.second_match_byte();
        staged.discard();
        return result;
    }
    if (!matcher.found()) {
        result.status = EditStatus::NotFound;
        staged.discard();
        return result;
    }

    const auto match_byte = matcher.match_byte();
    const auto& span = matcher.span();
    if (span.size() != old_text.size()) {
        throw std::logic_error("streaming matcher lost the matched span");
    }
    if (span.size() > source_bytes) {
        throw std::logic_error("matched span exceeds the scanned source");
    }
    const auto base_size = source_bytes - span.size();
    if (base_size > max_file_bytes || new_text.size() > max_file_bytes - base_size) {
        throw std::length_error("edited text exceeds the configured byte limit");
    }
    result.after_bytes = base_size + new_text.size();
    if (!snapshot.has_value()) {
        throw std::logic_error("missing row coordinates for the match");
    }
    result.before_position = {snapshot->line, snapshot->length};
    result.after_position = result.before_position;

    const auto prefix_tail = match_raw_tail;
    const auto corpus_start = match_byte - prefix_tail.size();
    std::string before_corpus(prefix_tail);
    before_corpus += span;
    before_corpus += suffix_head;
    std::string after_corpus(prefix_tail);
    after_corpus += new_text;
    after_corpus += suffix_head;

    const auto before_last = snapshot->line + count_terminator_ends(
        before_corpus, corpus_start, match_byte, match_byte + span.size() - 1);
    const auto after_last = new_text.empty()
        ? snapshot->line
        : snapshot->line + count_terminator_ends(
            after_corpus, corpus_start, match_byte,
            match_byte + new_text.size() - 1);
    const auto before_total = tracker.line() + 1;
    const auto prefix_terminators = count_terminator_ends(
        prefix_tail, corpus_start, corpus_start, match_byte);
    const auto after_total = 1 + snapshot->line +
        (edited.line() - prefix_terminators);
    // The edited tracker counts rows from just before the match, so its own row
    // numbers are relative; shift the captured rows into edited-text coordinates.
    for (auto& row : edited_captured) row.line += snapshot->line - prefix_terminators;

    const auto before_suffix_length = [&]() -> std::size_t {
        if (captured.empty() || captured.front().line != before_last) return 0;
        const auto end = captured.front().start + captured.front().length;
        const auto span_end = match_byte + span.size();
        return end > span_end ? end - span_end : 0;
    }();
    const auto after_suffix_length = [&]() -> std::size_t {
        if (edited_captured.empty() || edited_captured.front().line != after_last) return 0;
        const auto end = edited_captured.front().start + edited_captured.front().length;
        const auto span_end = match_byte + new_text.size();
        return end > span_end ? end - span_end : 0;
    }();

    SpanPieces pieces;
    pieces.first_line = snapshot->line;
    pieces.match_byte = match_byte;
    pieces.prefix_length = snapshot->length;
    pieces.prefix_head = snapshot->head;
    pieces.prefix_tail = snapshot->tail;
    pieces.middle = span;
    pieces.suffix_head = suffix_head;
    pieces.suffix_length = before_suffix_length;
    auto before_span_rows = build_span_rows(pieces);
    pieces.middle = new_text;
    pieces.suffix_length = after_suffix_length;
    auto after_span_rows = build_span_rows(pieces);

    std::vector<CapturedRow> pre_rows;
    for (const auto& row : pre_snapshot) {
        if (row.line < snapshot->line) pre_rows.push_back(row);
    }
    if (pre_rows.size() > context_lines) {
        pre_rows.erase(pre_rows.begin(), pre_rows.end() - static_cast<std::ptrdiff_t>(context_lines));
    }

    const auto changed = old_text != new_text;
    const auto before_range = range_for(before_total, snapshot->line, before_last, context_lines);
    const auto after_range = range_for(after_total, snapshot->line, after_last, context_lines);
    const auto width = std::max(std::to_string(before_range.last).size(),
                                std::to_string(after_range.last).size());

    const auto build_table = [&](std::vector<CapturedRow> span_rows,
                                 const std::vector<CapturedRow>& post,
                                 std::size_t last_changed) {
        std::vector<CapturedRow> table = std::move(span_rows);
        const auto add = [&](const CapturedRow& row) {
            const auto seen = std::any_of(table.begin(), table.end(),
                [&](const CapturedRow& other) { return other.line == row.line; });
            if (!seen) table.push_back(row);
        };
        for (const auto& row : pre_rows) add(row);
        std::size_t added = 0;
        for (const auto& row : post) {
            if (row.line <= last_changed || added >= context_lines) continue;
            add(row);
            ++added;
        }
        return merge_rows(std::move(table));
    };

    TableCursor before_cursor(build_table(std::move(before_span_rows), captured, before_last));
    result.before_excerpt = render_excerpt(
        before_cursor, before_range, width, changed ? '-' : '=', match_byte,
        old_text.size(), result.preview_truncated);
    TableCursor after_cursor(build_table(std::move(after_span_rows), edited_captured, after_last));
    result.after_excerpt = render_excerpt(
        after_cursor, after_range, width, changed ? '+' : '=', match_byte,
        new_text.size(), result.preview_truncated);

    if (!changed) {
        result.status = EditStatus::Unchanged;
        staged.discard();
        return result;
    }
    result.status = EditStatus::Modified;
    try {
        writer.flush();
        staged.publish(match_byte, span);
    } catch (const fileio::ReplaceConflict&) {
        result.status = EditStatus::Conflict;
        result.before_excerpt.clear();
        result.after_excerpt.clear();
        result.preview_truncated = false;
    } catch (const fileio::AtomicWriteError& error) {
        if (!error.published()) throw;
        result.status = EditStatus::PublishedSyncFailed;
        result.persistence_error = error.code();
    }
    return result;
}

} // namespace textedit
