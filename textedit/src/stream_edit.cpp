#include "textedit/edit.hpp"

#include "fileio/atomic_write.hpp"
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

using detail::CandidateWriter;
using detail::kMaxContextLines;
using detail::PreviewCapture;
using detail::StreamingMatcher;
using detail::TableCursor;

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
    // The replacement alone can exceed the budget: reject it before any file is
    // opened or any candidate byte is written.
    if (new_text.size() > max_file_bytes) {
        throw std::length_error("edited text exceeds the configured byte limit");
    }

    EditResult result;
    fileio::StagedReplacement staged(path);
    const bool changed = old_text != new_text;
    CandidateWriter writer(staged, max_file_bytes);
    // A no-op edit publishes nothing, so it writes nothing either.
    if (!changed) writer.enable(false);
    StreamingMatcher matcher(old_text, new_text, writer);

    // One sequential scan: the matcher alone, byte by byte. Preview rows come
    // from the verification pass, which re-reads the source anyway.
    std::size_t offset = 0;
    for (;;) {
        const auto chunk = staged.next_source();
        if (chunk.empty()) break;
        if (staged.source_bytes() > max_file_bytes) {
            throw std::length_error("file exceeds the configured byte limit");
        }
        for (const char raw : chunk) {
            matcher.step(static_cast<unsigned char>(raw), offset++);
        }
    }

    const auto source_bytes = static_cast<std::size_t>(staged.source_bytes());
    result.before_bytes = source_bytes;
    result.after_bytes = source_bytes;
    result.match_byte = matcher.match_byte();
    result.matches_at_least = matcher.found() ? (matcher.ambiguous() ? 2u : 1u) : 0u;
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

    result.status = changed ? EditStatus::Modified : EditStatus::Unchanged;
    detail::PreviewCapture capture(match_byte, old_text, new_text);
    if (changed) {
        try {
            writer.flush();
            // The verification pass reports the prefix and suffix it re-reads,
            // which is where the preview rows come from.
            staged.publish(match_byte, span, &capture);
        } catch (const fileio::ReplaceConflict&) {
            result.status = EditStatus::Conflict;
            result.before_excerpt.clear();
            result.after_excerpt.clear();
            result.preview_truncated = false;
            return result;
        } catch (const fileio::AtomicWriteError& error) {
            if (!error.published()) throw;
            result.status = EditStatus::PublishedSyncFailed;
            result.persistence_error = error.code();
        }
    } else {
        // Nothing is published, so the preview comes from a second pass over the
        // still-open source. The edit budget still counts that pass.
        staged.restart_source();
        for (;;) {
            const auto chunk = staged.next_source();
            if (chunk.empty()) break;
            capture.observe(
                static_cast<std::size_t>(staged.source_bytes() - chunk.size()), chunk);
        }
        if (staged.source_bytes() != source_bytes) {
            // The source changed between the scan and the preview pass; there is
            // still nothing to publish, but the coordinates no longer apply.
            result.status = EditStatus::Conflict;
            return result;
        }
    }
    capture.finish();

    const auto preview = capture.take(context_lines);
    result.before_position = preview.before_position;
    result.after_position = preview.after_position;
    const auto width = std::max(std::to_string(preview.before_range.last).size(),
                                std::to_string(preview.after_range.last).size());
    TableCursor before_cursor(preview.before_rows);
    result.before_excerpt = detail::render_excerpt(
        before_cursor, preview.before_range, width, changed ? '-' : '=',
        match_byte, old_text.size(), result.preview_truncated);
    TableCursor after_cursor(preview.after_rows);
    result.after_excerpt = detail::render_excerpt(
        after_cursor, preview.after_range, width, changed ? '+' : '=',
        match_byte, new_text.size(), result.preview_truncated);
    if (!changed) staged.discard();
    return result;
}

} // namespace textedit
