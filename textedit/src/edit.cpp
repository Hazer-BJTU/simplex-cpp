#include "textedit/edit.hpp"


#include "line_break.hpp"
#include "rows.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace textedit {
namespace {

using detail::ExcerptRange;
using detail::kMaxContextLines;
using detail::render_excerpt;
using detail::TextViewCursor;

/** Locate a byte without misclassifying the second byte of CRLF. */
TextPosition position_of(std::string_view text, std::size_t byte)
{
    std::size_t line = 0;
    std::size_t start = 0;
    for (std::size_t offset = 0; offset < byte;) {
        const auto width = detail::line_break_width(text.substr(offset));
        if (width == 0) {
            ++offset;
        } else if (offset + width <= byte) {
            offset += width;
            ++line;
            start = offset;
        } else {
            break;
        }
    }
    return {line, byte - start};
}

ExcerptRange excerpt_range(
    std::string_view text,
    std::size_t start_byte,
    std::size_t changed_bytes,
    std::size_t context)
{
    const auto total = detail::count_lines(text);
    const auto first_changed = position_of(text, start_byte).line;
    const auto last_changed = changed_bytes == 0
        ? first_changed
        : position_of(text, start_byte + changed_bytes - 1).line;
    return {
        first_changed > context ? first_changed - context : 0,
        last_changed + std::min(context, total - 1 - last_changed),
        first_changed,
        last_changed
    };
}

} // namespace

std::string_view edit_status_name(EditStatus status) noexcept
{
    switch (status) {
        case EditStatus::Modified: return "modified";
        case EditStatus::Unchanged: return "unchanged";
        case EditStatus::NotFound: return "not_found";
        case EditStatus::Ambiguous: return "ambiguous";
        case EditStatus::Conflict: return "conflict";
        case EditStatus::PublishedSyncFailed: return "published_sync_failed";
    }
    return "unknown";
}

PreparedEdit prepare_str_replace(
    std::string_view original,
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
    if (original.size() > max_file_bytes) {
        throw std::length_error("original text exceeds the configured byte limit");
    }

    PreparedEdit plan;
    auto& result = plan.result;
    result.before_bytes = original.size();
    result.after_bytes = original.size();
    const auto first = original.find(old_text);
    if (first == std::string_view::npos) return plan;
    result.matches_at_least = 1;
    result.match_byte = first;
    const auto second = original.find(old_text, first + 1);
    if (second != std::string_view::npos) {
        result.status = EditStatus::Ambiguous;
        result.matches_at_least = 2;
        result.second_match_byte = second;
        return plan;
    }

    const auto base = original.size() - old_text.size();
    if (base > max_file_bytes || new_text.size() > max_file_bytes - base) {
        throw std::length_error("edited text exceeds the configured byte limit");
    }
    const bool changed = old_text != new_text;
    result.status = changed ? EditStatus::Modified : EditStatus::Unchanged;
    result.after_bytes = base + new_text.size();
    result.before_position = position_of(original, first);
    if (changed) {
        plan.updated_text = std::string(original);
        plan.updated_text.replace(first, old_text.size(), new_text);
    }
    const auto after = changed ? std::string_view(plan.updated_text) : original;
    result.after_position = position_of(after, first);

    const auto before_range = excerpt_range(
        original, first, old_text.size(), context_lines);
    const auto after_range = excerpt_range(
        after, first, new_text.size(), context_lines);
    const auto width = std::max(
        std::to_string(before_range.last).size(),
        std::to_string(after_range.last).size());
    TextViewCursor before_cursor(original);
    result.before_excerpt = render_excerpt(
        before_cursor, before_range, width, changed ? '-' : '=', first,
        old_text.size(), result.preview_truncated);
    TextViewCursor after_cursor(after);
    result.after_excerpt = render_excerpt(
        after_cursor, after_range, width, changed ? '+' : '=', first,
        new_text.size(), result.preview_truncated);
    return plan;
}

} // namespace textedit
