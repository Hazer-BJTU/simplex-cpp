#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "textedit/line_index.hpp"

namespace textedit {

enum class EditStatus {
    Modified,
    Unchanged,
    NotFound,
    Ambiguous,
    Conflict,
    PublishedSyncFailed
};

[[nodiscard]] std::string_view edit_status_name(EditStatus status) noexcept;

/** Outcome and bounded, model-safe context around a precise byte replacement. */
struct EditResult {
    EditStatus status = EditStatus::NotFound;
    /// Zero, one, or two. Two means at least two, not the exact total.
    std::size_t matches_at_least = 0;
    std::optional<std::size_t> second_match_byte;
    std::size_t match_byte = 0;
    TextPosition before_position;
    TextPosition after_position;
    std::size_t before_bytes = 0;
    std::size_t after_bytes = 0;
    std::string before_excerpt;
    std::string after_excerpt;
    /// An excerpt omitted lines or clipped a long line; it says so inline.
    bool preview_truncated = false;
    /// Only set for PublishedSyncFailed; contents are visible, durability unclear.
    std::error_code persistence_error;
};

/** The pure edit decision and complete replacement bytes, for callers to commit. */
struct PreparedEdit {
    EditResult result;
    std::string updated_text;
};

/**
 * Find exactly one occurrence of old_text and prepare a byte-exact replacement.
 * Overlapping occurrences count separately. Empty old_text is invalid; empty
 * new_text deletes the match. No normalization or UTF-8 validation occurs for
 * the edited bytes. No match or at least two matches produces no updated_text.
 * A unique identical replacement is Unchanged and does not need publication.
 *
 * context_lines (default 3, maximum 20) controls rows on each side of the
 * changed span. Excerpts use aligned marker and decimal line-number gutters:
 * '-' before, '+' after, '=' for an unchanged match. Their shared width is
 * computed across both excerpts. Long changed lines focus near the match,
 * with explicit skipped-byte markers. Each line is limited to 512 display bytes;
 * large spans show their first and last 24 rows with an omission marker.
 * Invalid UTF-8 in the excerpt is repaired for display only. Coordinates and
 * edited content remain raw byte-based. No full-file line index is allocated.
 */
[[nodiscard]] PreparedEdit prepare_str_replace(
    std::string_view original,
    std::string_view old_text,
    std::string_view new_text,
    std::size_t context_lines = 3,
    std::size_t max_file_bytes = 16 * 1024 * 1024);

/**
 * Edit an existing single-link regular file, with the same pure semantics.
 * Missing/ambiguous matches and unchanged content never write. Before commit,
 * the original bytes and file identity are checked again. Conflict means no
 * replacement was published. PublishedSyncFailed means the replacement is
 * visible but directory fsync failed; do not retry blindly. Other IO failures
 * throw before publication. External writers that ignore this protocol can
 * still race after the final check. The path is not a workspace restriction.
 */
[[nodiscard]] EditResult str_replace_file(
    const std::filesystem::path& path,
    std::string_view old_text,
    std::string_view new_text,
    std::size_t context_lines = 3,
    std::size_t max_file_bytes = 16 * 1024 * 1024);

} // namespace textedit
