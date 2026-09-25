#include "textedit/edit.hpp"

#include "fileio/atomic_write.hpp"
#include "fileio/replace_existing.hpp"
#include "textedit/read.hpp"

#include "line_break.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace textedit {
namespace {

constexpr std::size_t kMaxContextLines = 20;
constexpr std::size_t kMaxShownLines = 48;
constexpr std::size_t kMaxLineDisplayBytes = 512;

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

struct ExcerptRange {
    std::size_t first;
    std::size_t last;
    std::size_t changed_first;
    std::size_t changed_last;
};

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

std::string padded(std::size_t value, std::size_t width)
{
    auto digits = std::to_string(value);
    return std::string(width - digits.size(), ' ') + digits;
}

/** Append one physical line with a stable marker/number/pipe gutter. */
void append_line(
    std::string& output,
    std::string_view content,
    std::size_t number,
    std::size_t width,
    char marker,
    std::size_t focus_column,
    bool& clipped)
{
    if (!output.empty()) {
        output += '\n';
    }
    output += marker;
    output += ' ';
    output += padded(number, width);
    output += " | ";
    const auto shown_from = content.size() > kMaxLineDisplayBytes &&
                                    focus_column > 128
        ? focus_column - 128 : 0;
    if (shown_from > 0) {
        output += "[... " + std::to_string(shown_from) + " bytes skipped ...] ";
        clipped = true;
    }
    content.remove_prefix(shown_from);
    auto rendered = read_bytes_bounded(
        content, 0, content.size(), ByteReadFormat::Plain,
        kMaxLineDisplayBytes);
    output += rendered.text;
    if (rendered.output_truncated) {
        output += " ... [line clipped]";
        clipped = true;
    }
}

/** Keep both ends of a long changed span, with an explicit omission row. */
std::string render_excerpt(
    std::string_view text,
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
    std::size_t number = 0;
    std::size_t line_start = 0;

    auto consume = [&](std::size_t content_end, std::size_t next_start) {
        if (number >= range.first && number <= range.last) {
            if (omit_middle && number == head_end) {
                if (!output.empty()) output += '\n';
                output += "! " + std::string(width, ' ') + " | ... " +
                          std::to_string(tail_start - head_end) +
                          " lines omitted ...";
                clipped = true;
            }
            if (!omit_middle || number < head_end || number >= tail_start) {
                const auto marker = number >= range.changed_first &&
                                    number <= range.changed_last
                    ? changed_marker : ' ';
                std::size_t focus_column = 0;
                if (number == range.changed_first) {
                    focus_column = changed_start_byte - line_start;
                } else if (number == range.changed_last && changed_bytes > 0) {
                    focus_column = changed_start_byte + changed_bytes - 1 - line_start;
                }
                append_line(output, text.substr(line_start, content_end - line_start),
                            number, width, marker, focus_column, clipped);
            }
        }
        ++number;
        line_start = next_start;
    };

    for (std::size_t offset = 0; offset < text.size() && number <= range.last;) {
        const auto width_of_break = detail::line_break_width(text.substr(offset));
        if (width_of_break == 0) {
            ++offset;
        } else {
            consume(offset, offset + width_of_break);
            offset += width_of_break;
        }
    }
    if (number <= range.last) consume(text.size(), text.size());
    return output;
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
    result.before_excerpt = render_excerpt(
        original, before_range, width, changed ? '-' : '=', first,
        old_text.size(), result.preview_truncated);
    result.after_excerpt = render_excerpt(
        after, after_range, width, changed ? '+' : '=', first,
        new_text.size(), result.preview_truncated);
    return plan;
}

EditResult str_replace_file(
    const std::filesystem::path& path,
    std::string_view old_text,
    std::string_view new_text,
    std::size_t context_lines,
    std::size_t max_file_bytes)
{
    const auto original = fileio::read_editable_file(path, max_file_bytes);
    auto plan = prepare_str_replace(
        original, old_text, new_text, context_lines, max_file_bytes);
    if (plan.result.status != EditStatus::Modified) {
        return std::move(plan.result);
    }
    try {
        fileio::replace_existing_file(path, original, plan.updated_text);
    } catch (const fileio::ReplaceConflict&) {
        plan.result.status = EditStatus::Conflict;
        plan.result.before_excerpt.clear();
        plan.result.after_excerpt.clear();
        plan.result.preview_truncated = false;
    } catch (const fileio::AtomicWriteError& error) {
        if (!error.published()) throw;
        plan.result.status = EditStatus::PublishedSyncFailed;
        plan.result.persistence_error = error.code();
    }
    return std::move(plan.result);
}

} // namespace textedit
