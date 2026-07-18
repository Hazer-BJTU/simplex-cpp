#include "editor.hpp"
#include "schema.hpp"

#include <ranges>
#include <sstream>
#include <stdexcept>

namespace indextools {

// See editor.hpp for the full contract. The implementation rebuilds the source
// string in three phases — copy leading lines, splice in the (re-split)
// inserted content, then copy trailing lines — joining every line with "\n"
// so that CRLF endings in either part are normalised to LF.

std::string line_replace_edit(
    const SplitedString& source_lines,
    size_t line_start,
    size_t line_end,
    std::string_view inserted_content,
    bool insert_mode
) {
    if (insert_mode) {
        // Insert `inserted_content` between line_start and line_start + 1 (if exists).
        // If line_start >= source_lines.size(), append `inserted_content` to source.
        line_end = line_start;
    } else {
        // Otherwise, replace [line_start, line_end] (inclusive) with `inserted_content`.
        // This operation is invalid when source is empty.
        if (line_start >= source_lines.size() || line_end >= source_lines.size()) {
            throw std::runtime_error(
                (std::ostringstream{} << "invalid edit range specified: ["
                    << line_start << ", " << line_end << "]; given total line number = "
                    << source_lines.size()).str()
            );
        }

        if (line_end < line_start) {
            throw std::runtime_error(
                (std::ostringstream{} << "invalid edit range specified: ["
                    << line_start << ", " << line_end
                    << "]; line_end >= line_start should hold").str()
            );
        }
    }

    std::string edited_source;
    edited_source.reserve(source_lines.source().size() + inserted_content.size());
    bool insert_new_line = false;

    // Copy lines before the edit range.
    for (size_t i = 0; i < (insert_mode ? line_start + 1 : line_start) && i < source_lines.size(); ++i) {
        if (insert_new_line) {
            edited_source.append("\n");
        }
        edited_source.append(source_lines[i]);
        insert_new_line = true;
    }

    // Insert the new content, split into lines by the default delimiters
    // ("\n", "\r\n"). Each chunk's content (without delimiter) becomes one
    // line, joined by "\n" — this normalises CRLF to LF.
    for (auto line: SplitedString{}.load(std::string(inserted_content))) {
        if (insert_new_line) {
            edited_source.append("\n");
        }
        edited_source.append(line);
        insert_new_line = true;
    }

    // Copy lines after the edit range.
    for (size_t i = line_end + 1; i < source_lines.size(); ++i) {
        if (insert_new_line) {
            edited_source.append("\n");
        }
        edited_source.append(source_lines[i]);
        insert_new_line = true;
    }

    return edited_source;
}

std::string str_replace_edit(
    const SplitedString& source_lines,
    std::string_view original_content,
    std::string_view inserted_content,
    bool replace_all
) {
    // Start from a mutable copy of the full source; we operate on raw byte
    // offsets returned by locate_pattern rather than on chunk indices.
    std::string edited_source(source_lines.source());
    auto matches = source_lines.locate_pattern(original_content);

    if (matches.size() == 0) {
        throw std::runtime_error((std::ostringstream{} << "pattern: " << original_content << " not found in target text").str());
    }

    if (!replace_all && matches.size() > 1) {
        throw std::runtime_error((std::ostringstream{} << "ambiguous pattern specified; found " << matches.size() << " matches").str());
    }

    // Replace back-to-front so that already-processed replacements cannot
    // shift the byte offsets of the remaining (earlier) matches.
    for (const auto& [byte_start, byte_end]: matches | std::views::reverse) {
        edited_source.replace(byte_start, byte_end + 1 - byte_start, inserted_content);
    }

    return edited_source;
}

nlohmann::json check_difference(
    const SplitedString& source_lines,
    const SplitedString& edited_lines,
    const std::filesystem::path& file_path,
    size_t context_lines
) noexcept {
    // Run a line-level Myers diff. Lines are compared by content (chunk text,
    // delimiters excluded), so two lines that differ only in their trailing
    // line ending still compare equal.
    std::vector<size_t> source_deleted, edited_added;
    // On allocation failure levenshtein_distance returns nullopt and leaves both
    // index lists empty; the diff below then simply reports no edits.
    (void) levenshtein_distance(
        [&source_lines, &edited_lines](size_t i, size_t j) -> bool {
            return source_lines[i] == edited_lines[j];
        },
        source_lines.size(),
        edited_lines.size(),
        source_deleted,
        edited_added
    );

    // Build one JSON entry for a side of the diff (deletions from the source,
    // or additions to the edited text). `is_add` selects which label is used
    // for operated lines in the "field_name"/"line_type" fields.
    auto check_difference_inner = [&file_path, context_lines](
        const SplitedString& lines,
        const std::vector<size_t>& line_operated,
        bool is_add
    ) -> nlohmann::json {
        size_t operated_cnt = 0;
        // line_marked distinguishes truly-operated lines from the surrounding
        // context lines that expand_interval will pull in.
        std::vector<bool> line_marked(lines.size(), false);
        std::vector<std::tuple<size_t, size_t>> intervals;
        for (auto pos: line_operated) {
            intervals.emplace_back(pos, pos);
            line_marked[pos] = true;
            operated_cnt ++;
        }

        // Grow each operated line by `context_lines` on both sides and merge
        // overlapping windows into contiguous intervals.
        auto expanded_intervals = expand_interval(std::move(intervals), lines.size(), context_lines);

        // Assemble the entry via the shared schema builders (see schema.hpp):
        // meta carries the file path + operated-line count, text carries
        // parallel line number / content / type arrays. Operated lines are
        // tagged add/delete, pulled-in context lines base.
        nlohmann::json meta = schema::MetaBuilder()
            .field("File", file_path.string())
            .field(is_add ? "Lines added" : "Lines deleted", operated_cnt)
            .build();

        schema::TextBody body;
        for (const auto& [line_start, line_end]: expanded_intervals) {
            for (size_t line_idx = line_start; line_idx <= line_end; ++line_idx) {
                std::string_view type = line_marked[line_idx]
                    ? (is_add ? schema::line_type::add : schema::line_type::remove)
                    : schema::line_type::base;
                body.line(lines[line_idx], line_idx, type);
            }
        }

        return schema::text_block(std::move(meta), body.build());
    };

    // entry 0: deletions (from source); entry 1: additions (to edited text).
    auto result = nlohmann::json::array();
    result.push_back(check_difference_inner(source_lines, source_deleted, false));
    result.push_back(check_difference_inner(edited_lines, edited_added, true));
    return result;
}

}
