#include "textedit/edit.hpp"

#include "fileio/atomic_write.hpp"
#include "fileio/replace_existing.hpp"
#include "fileio/staged_replace.hpp"

#include "rows.hpp"
#include "stream_edit_preview.hpp"
#include "stream_edit_scan.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace textedit {

using detail::append_bounded;
using detail::build_span_rows;
using detail::CandidateWriter;
using detail::CapturedRow;
using detail::count_terminator_ends;
using detail::harvest;
using detail::kCapturedRowLimit;
using detail::kMaxContextLines;
using detail::merge_rows;
using detail::range_for;
using detail::RowTracker;
using detail::SpanPieces;
using detail::StreamingMatcher;
using detail::TableCursor;
using detail::ByteRing;
using detail::kRawTailBytes;
using detail::kSuffixCaptureBytes;

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
