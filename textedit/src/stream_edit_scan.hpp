#pragma once

// Internal building blocks of the streaming edit scan: the logical-row tracker
// used for bounded previews, the buffered candidate writer and the streaming
// exact matcher. Nothing here is installed or exported through textedit's
// public headers.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fileio/staged_replace.hpp"

#include "line_break.hpp"
#include "rows.hpp"

namespace textedit::detail {

/// Content bytes of a row kept before the match for a focused window.
constexpr std::size_t kFocusTailBytes = 128 + 4;
/// Stream bytes after the span captured for the last changed row.
constexpr std::size_t kSuffixCaptureBytes = kRowWindowBytes + 4;

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

/** A small fixed-capacity byte ring, for lookbehind without unbounded copying. */
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

/**
 * Row coordinates of one byte stream, advanced in order.
 *
 * push() handles one byte with up to two lookahead bytes, so a caller can take
 * an exact snapshot at any offset. A row ends when the last byte of its
 * terminator is consumed, which matches position_of()'s "terminator contained
 * in [0, offset)" rule. Retained state is the current row's head, a short tail
 * for focused windows, and the most recent completed rows; a listener can
 * observe every completed row without keeping an unbounded deque.
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

    /** Optional row observer; `row` is borrowed for the duration of the call. */
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void row_completed(const CapturedRow& row) = 0;
    };

    explicit RowTracker(std::uintmax_t start_offset = 0, Listener* listener = nullptr)
        : offset_(start_offset), row_start_(start_offset), tail_(kFocusTailBytes),
          listener_(listener)
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

    /**
     * Feed a chunk, carrying the last two bytes for lookahead.
     *
     * The carry is kept across calls, so consecutive feeds form one continuous
     * stream: a terminator split between two chunks is classified once.
     */
    void feed(std::string_view bytes) { feed_until(bytes, kNoStop); }

    /**
     * Feed a chunk, stopping as soon as offset() reaches `stop`.
     *
     * The unprocessed bytes stay in the carry for the next call, so a caller can
     * take an exact snapshot at any offset, including one that falls at the end
     * of the bytes supplied so far. Requires `stop >= offset()`.
     */
    void feed_until(std::string_view bytes, std::uintmax_t stop)
    {
        carry_buffer_.assign(carry_);
        carry_buffer_.append(bytes);
        carry_.clear();
        const auto usable = carry_buffer_.size() > 2 ? carry_buffer_.size() - 2 : 0;
        std::size_t index = 0;
        while (index < usable && offset_ < stop) push(carry_buffer_, index++);
        carry_.assign(carry_buffer_.substr(index));
    }

    /**
     * Resolve the remaining bytes at EOF, stopping once offset() reaches `stop`.
     *
     * A milestone that falls on the final bytes is still observed exactly: the
     * remaining bytes are consumed with end-of-file terminator semantics, one
     * at a time, so offset() never steps past `stop`.
     */
    void finish_until(std::uintmax_t stop)
    {
        std::size_t index = 0;
        while (index < carry_.size() && offset_ < stop) {
            push_final(std::string_view(carry_), index);
            ++index;
        }
        carry_.erase(0, index);
    }

    /** Resolve a trailing partial terminator at EOF and record the final row. */
    void finish()
    {
        finish_until(kNoStop);
        CapturedRow row;
        row.line = line_;
        row.start = static_cast<std::size_t>(row_start_);
        row.length = length_;
        row.head = head_;
        notify(row);
    }

    [[nodiscard]] std::size_t line() const noexcept { return line_; }
    [[nodiscard]] std::uintmax_t offset() const noexcept { return offset_; }
    [[nodiscard]] Snapshot snapshot() const
    {
        return {line_, static_cast<std::size_t>(row_start_), length_, head_, tail_.ordered()};
    }

    static constexpr std::uintmax_t kNoStop = std::numeric_limits<std::uintmax_t>::max();

private:
    void complete_row()
    {
        CapturedRow row;
        row.line = line_;
        row.start = static_cast<std::size_t>(row_start_);
        row.length = length_;
        row.head = head_;
        notify(row);
        ++line_;
        row_start_ = offset_ + 1;
        length_ = 0;
        head_.clear();
        tail_.clear();
    }

    void notify(const CapturedRow& row)
    {
        if (listener_ != nullptr) listener_->row_completed(row);
    }

    std::uintmax_t offset_ = 0;
    std::uintmax_t row_start_ = 0;
    std::size_t line_ = 0;
    std::size_t length_ = 0;
    std::size_t pending_left_ = 0;
    std::string head_;
    ByteRing tail_;
    std::string carry_;
    std::string carry_buffer_;
    Listener* listener_ = nullptr;
};

/**
 * Buffered candidate writes, bounded by the configured edit budget.
 *
 * Nothing is written before the match is known: begin() copies the untouched
 * prefix straight from the source and appends the replacement, and every later
 * append is suffix bytes. A missing match therefore leaves the candidate empty,
 * and no write can push the temporary file past the budget.
 */
class CandidateWriter {
public:
    CandidateWriter(fileio::StagedReplacement& staged, std::size_t limit)
        : staged_(staged), limit_(limit) {}

    /** Start the candidate at the match: copy the prefix, then the replacement. */
    void begin(std::uintmax_t prefix_bytes, std::string_view replacement)
    {
        if (!enabled_) return;
        if (prefix_bytes > limit_) throw std::length_error(
            "edited text exceeds the configured byte limit");
        staged_.copy_prefix(prefix_bytes);
        written_ = prefix_bytes;
        append(replacement);
    }

    void append(std::string_view bytes)
    {
        if (!enabled_) return;
        if (bytes.size() > limit_ - written_) throw std::length_error(
            "edited text exceeds the configured byte limit");
        written_ += bytes.size();
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
    std::size_t limit_ = 0;
    std::uintmax_t written_ = 0;
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
        if (stopped_) return;
        if (!found_) {
            while (q_ > 0 && static_cast<unsigned char>(pattern_[q_]) != byte) {
                drop(q_ - prefix_[q_ - 1]);
                q_ = prefix_[q_ - 1];
            }
            if (static_cast<unsigned char>(pattern_[q_]) == byte) {
                pending_.push_back(static_cast<char>(byte));
                ++q_;
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

    [[nodiscard]] bool found() const noexcept { return found_; }
    [[nodiscard]] bool ambiguous() const noexcept { return ambiguous_; }
    [[nodiscard]] std::size_t match_byte() const noexcept { return match_byte_; }
    [[nodiscard]] std::size_t second_match_byte() const noexcept { return second_match_byte_; }
    [[nodiscard]] const std::string& span() const noexcept { return span_; }

    /** Stop emitting suffix bytes; used by ambiguous results. */
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

    /**
     * Drop unmatched bytes from the front of the pending buffer.
     *
     * Bytes before the match are not written: if a match is found, begin()
     * copies that whole prefix from the source instead of replaying it here.
     */
    void drop(std::size_t count)
    {
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
            writer_.begin(start, replacement_);
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
};

} // namespace textedit::detail
