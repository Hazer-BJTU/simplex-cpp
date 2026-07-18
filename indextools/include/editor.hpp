/**
 * @file editor.hpp
 * @brief Source-text editing and diffing primitives for the indextools
 *        framework.
 *
 * This header exposes three operations on a SplitedString source view:
 *
 *   - levenshtein_distance: Myers-diff based edit-distance computation that
 *                          returns the indices of deleted/inserted items.
 *   - line_replace_edit:    replace (or insert) a contiguous line range with
 *                          new content, normalising line endings to LF.
 *   - str_replace_edit:     replace an exact substring match with new content.
 *   - check_difference:     diff two SplitedString views and produce a
 *                          structured JSON report of added/deleted lines.
 */

#pragma once

#include "split.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <vector>

namespace indextools {

/**
 * @brief Compute the diff between two sequences using Myers' algorithm,
 *        returning indices of deleted and inserted positions.
 *
 * Time:  O((M+N)·D) where D is the edit script length.
 * Space: O((M+N)·D) for the trace used during backtracking.
 *
 * Output:
 *   - pos_deleted: indices in the original sequence that were deleted.
 *   - pos_added:   indices in the modified sequence that were inserted.
 *   Both lists are in ascending order.
 *
 * @tparam Equal   Callable `bool(size_t i, size_t j)` testing item equality.
 * @param  M, N    Sizes of the original and modified sequences.
 * @return std::optional<size_t> Edit script length (deletions + insertions);
 *         std::nullopt on allocation failure.
 */
template <typename Equal>
[[nodiscard]]
std::optional<size_t> levenshtein_distance(
    const Equal& equal, size_t M, size_t N,
    std::vector<size_t>& pos_deleted,
    std::vector<size_t>& pos_added
) {
    pos_deleted.clear();
    pos_added.clear();
    // Edge cases: one or both sequences empty.
    if (M == 0 && N == 0) {
        return 0;
    }
    if (M == 0) {
        pos_added.reserve(N);
        for (size_t j = 0; j < N; ++j) {
            pos_added.push_back(j);
        }
        return N;
    }
    if (N == 0) {
        pos_deleted.reserve(M);
        for (size_t i = 0; i < M; ++i) {
            pos_deleted.push_back(i);
        }
        return M;
    }
    const size_t max_d = M + N;
    // V is indexed by diagonal k in [-max_d, max_d]; offset by max_d.
    const size_t offset = max_d;
    const size_t v_size = 2 * max_d + 1;
    std::vector<long long> V;
    // Snapshot of V after each D iteration, used for backtracking.
    std::vector<std::vector<long long>> trace;
    try {
        V.assign(v_size, 0);
        trace.reserve(max_d + 1);
    } catch (...) {
        return std::nullopt;
    }
    size_t edit_d = 0;
    bool found = false;
    // Forward pass: find shortest edit script length D.
    for (size_t d = 0; d <= max_d && !found; ++d) {
        try {
            trace.push_back(V);
        } catch (...) {
            return std::nullopt;
        }
        // k ranges over [-d, d] with step 2.
        for (long long k = -(long long)d; k <= (long long)d; k += 2) {
            long long x;
            // Choose to move down (insertion) or right (deletion).
            if (k == -(long long)d ||
                (k != (long long)d && V[offset + k - 1] < V[offset + k + 1])) {
                x = V[offset + k + 1];           // down: insertion
            } else {
                x = V[offset + k - 1] + 1;       // right: deletion
            }
            long long y = x - k;
            // Extend along the diagonal through matching elements.
            while (x < (long long)M && y < (long long)N &&
                   equal((size_t)x, (size_t)y)) {
                ++x;
                ++y;
            }
            V[offset + k] = x;
            if (x >= (long long)M && y >= (long long)N) {
                edit_d = d;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return std::nullopt; // should not happen for valid inputs
    }
    // Backtrack through trace to recover deletions/insertions.
    long long x = (long long)M;
    long long y = (long long)N;
    for (long long d = (long long)edit_d; d > 0; --d) {
        const auto& Vp = trace[(size_t)d];
        long long k = x - y;
        long long prev_k;
        if (k == -d || (k != d && Vp[offset + k - 1] < Vp[offset + k + 1])) {
            prev_k = k + 1; // came from insertion
        } else {
            prev_k = k - 1; // came from deletion
        }
        long long prev_x = Vp[offset + prev_k];
        long long prev_y = prev_x - prev_k;
        // Skip the diagonal (matching) segment.
        while (x > prev_x && y > prev_y) {
            --x;
            --y;
        }
        // Record the single non-diagonal edit.
        if (x == prev_x) {
            // insertion at modified[y-1]
            pos_added.push_back((size_t)(y - 1));
        } else {
            // deletion at original[x-1]
            pos_deleted.push_back((size_t)(x - 1));
        }
        x = prev_x;
        y = prev_y;
    }
    // Indices were collected in reverse; restore ascending order.
    std::reverse(pos_deleted.begin(), pos_deleted.end());
    std::reverse(pos_added.begin(), pos_added.end());
    return edit_d;
}

/**
 * @brief Replace or insert a contiguous line range within the source text.
 *
 * The source is treated as a sequence of lines (chunks of @p source_lines).
 * The result is a fully reconstructed string with line endings normalised
 * to LF: the inserted content is re-split by the default delimiters
 * ("\n", "\r\n") and re-joined with "\n", so any CRLF in either the
 * original or the inserted content is converted to LF.
 *
 * Two modes of operation:
 *
 *   - Insert mode (@p insert_mode == true):
 *       The content is inserted *between* lines @p line_start and
 *       @p line_start + 1. @p line_end is ignored (forced equal to
 *       @p line_start). If @p line_start is past the last line, the
 *       content is appended to the source.
 *
 *   - Replace mode (@p insert_mode == false):
 *       The inclusive range [@p line_start, @p line_end] is replaced by
 *       @p inserted_content. This mode is invalid for an empty source.
 *
 * @param source_lines      Read-only view of the original source, split into
 *                          lines by the default delimiters.
 * @param line_start        First line of the edit range (0-based).
 * @param line_end          Last line of the edit range (0-based, inclusive).
 *                          Ignored in insert mode.
 * @param inserted_content  New content to splice in. May contain any mix of
 *                          "\n" and "\r\n" line endings.
 * @param insert_mode       If true, insert rather than replace.
 * @return                  The edited source as a new std::string.
 * @throws std::runtime_error if @p insert_mode is false and the range
 *         [@p line_start, @p line_end] is out of bounds or inverted
 *         (line_end < line_start).
 */
std::string line_replace_edit(
    const SplitedString& source_lines,
    size_t line_start,
    size_t line_end,
    std::string_view inserted_content,
    bool insert_mode = false
);

/**
 * @brief Replace occurrences of an exact substring within the source text.
 *
 * Locates @p original_content as a plain substring of the source (via
 * SplitedString::locate_pattern) and replaces each occurrence with
 * @p inserted_content.
 *
 * Replacement is performed back-to-front on a mutable copy of the source
 * string, so earlier byte offsets remain valid while later matches are
 * being processed.
 *
 * @param source_lines      Read-only view of the original source.
 * @param original_content  The exact substring to search for.
 * @param inserted_content  The replacement text.
 * @param replace_all       If false (default), exactly one match must exist;
 *                          zero matches or more than one match raise an error.
 *                          If true, every match is replaced.
 * @return                  The edited source as a new std::string.
 * @throws std::runtime_error if @p original_content is not found, or if
 *         @p replace_all is false and the pattern matches more than once.
 */
std::string str_replace_edit(
    const SplitedString& source_lines,
    std::string_view original_content,
    std::string_view inserted_content,
    bool replace_all = false
);

/**
 * @brief Diff two source views and emit a structured JSON report.
 *
 * Computes a line-level diff between @p source_lines and @p edited_lines
 * using levenshtein_distance() (Myers' algorithm), then builds a JSON
 * array of two entries:
 *
 *   - entry 0: deleted lines — lines present in @p source_lines but absent
 *              from @p edited_lines.
 *   - entry 1: added lines   — lines present in @p edited_lines but absent
 *              from @p source_lines.
 *
 * Each entry contains:
 *
 *   - meta.field_name / meta.field_content: human-readable summary
 *     (file path and count of operated lines).
 *   - text.line_number:    the line indices included in the report.
 *   - text.line_content:   the corresponding line text.
 *   - text.line_type:      "delete"/"add" for operated lines, "base" for
 *                          the surrounding context lines.
 *
 * Operated lines are expanded by @p context_lines on each side (via
 * expand_interval) and overlapping windows are merged, so the report
 * shows context around each change.
 *
 * This function is noexcept: on allocation failure inside
 * levenshtein_distance, both index lists stay empty and the report simply
 * contains no operated lines (only context-free empty entries).
 *
 * @param source_lines   The original source, split into lines.
 * @param edited_lines   The edited source, split into lines.
 * @param file_path      Path used to label the report's "File" field.
 * @param context_lines  Number of context lines to expand around each
 *                       operated line.
 * @return               A JSON array of two objects (deletions, additions).
 */
nlohmann::json check_difference(
    const SplitedString& source_lines,
    const SplitedString& edited_lines,
    const std::filesystem::path& file_path,
    size_t context_lines
) noexcept;

}
