#pragma once

/**
 * @file utils.hpp
 * @brief Utility functions for the indextools project.
 *
 * Provides helpers for formatting, rendering, glob-based file search,
 * and general-purpose operations that are not specific to any single subsystem.
 */

#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace indextools {

// =============================================================================
// JSON Rendering
// =============================================================================

/**
 * @brief Convert a schema-conforming JSON response into human-readable text.
 *
 * Convenience wrapper around render_response() that always uses
 * RenderMode::human_readable (Unicode box-drawing characters, spacious
 * layout). Provided for backward compatibility with callers that predate
 * the RenderMode enum.
 *
 * @param json_array  A JSON array of schema-conforming objects (DisplayBlock,
 *                    ProcessReport, ErrorReport, or ExternalRef), or a single
 *                    such object.
 * @param use_ascii   If true, delegates to RenderMode::ai_readable (compact
 *                    ASCII). If false (default), uses RenderMode::human_readable
 *                    (Unicode box-drawing characters, spacious layout).
 * @return Formatted multi-line string.
 */
std::string json_to_readable_text(const nlohmann::json& json_array, bool use_ascii = false);

// =============================================================================
// Schema-aware Response Rendering
// =============================================================================

/**
 * @brief Rendering mode for schema-to-text conversion.
 *
 *   - human_readable — spacious layout, Unicode box-drawing characters,
 *                      line-numbered source, section headers with rules.
 *   - ai_readable    — compact layout, ASCII only, dense information
 *                      packing to minimise context window cost.
 */
enum class RenderMode {
    human_readable,
    ai_readable
};

/**
 * @brief Convert a schema-conforming JSON response into readable text.
 *
 * Accepts a single JSON object or an array of objects. Each element is
 * auto-detected as one of the four schemas defined in schema.hpp —
 * DisplayBlock, ProcessReport, ErrorReport, or ExternalRef — and
 * rendered accordingly. Unknown objects fall back to json.dump().
 *
 * The meta table (field_name / field_content) is rendered generically
 * without interpreting specific key names, so all four schemas share
 * the same meta-formatting logic.
 *
 * @param response  A JSON object or array of schema objects.
 * @param mode      Rendering style (default: human_readable).
 * @return Formatted multi-line string.
 */
std::string render_response(const nlohmann::json& response,
                            RenderMode mode = RenderMode::human_readable);

// =============================================================================
// Glob Pattern Matching
// =============================================================================

/**
 * @brief Match a single path component against a glob pattern segment.
 *
 * Supports standard glob wildcards within a single path component
 * (no path separators):
 *   - `*`     : matches zero or more characters
 *   - `?`     : matches exactly one character
 *   - `[...]` : character class (supports ranges with `-`,
 *               negation with `!` or `^`)
 *
 * Consecutive `*` characters are equivalent to a single `*`.
 * An unmatched `[` is treated as a literal character.
 *
 * @param pattern  Glob pattern for a single path component.
 * @param name     The filename or directory name to test.
 * @return true if name matches pattern.
 */
bool glob_match(std::string_view pattern, std::string_view name);

// =============================================================================
// Glob-based File Traversal
// =============================================================================

/**
 * @brief Find all regular files under a root directory matching a glob pattern.
 *
 * Uses `std::filesystem` to traverse the directory tree. Directories whose
 * names do not satisfy the current pattern segment are **pruned** — the
 * traversal skips their entire subtree, avoiding unnecessary filesystem
 * operations.
 *
 * ## Supported glob syntax (relative to root_path)
 *
 * | Token   | Meaning                                      |
 * |---------|----------------------------------------------|
 * | `*`     | matches any characters except `/`            |
 * | `**`    | matches zero or more directory levels        |
 * | `?`     | matches any single character                 |
 * | `[...]` | character class (ranges, negation supported) |
 * | `/`     | path separator                               |
 *
 * ## Edge cases
 *
 * - An empty pattern returns an empty result.
 * - A non-existent or inaccessible `root_path` returns an empty result.
 * - Symlink cycles are detected via canonical path tracking and skipped.
 * - Permission errors on individual directories are silently skipped.
 * - Trailing `/` in the pattern is ignored.
 *
 * @param root_path    The root directory to start traversal from.
 * @param glob_pattern The glob pattern (relative to root_path).
 * @return Sorted vector of absolute `std::filesystem::path` objects, one per
 *         matching regular file. Paths are made absolute via
 *         `std::filesystem::absolute` and sorted lexicographically for
 *         deterministic output.
 */
std::vector<std::filesystem::path> glob_find(const std::filesystem::path& root_path,
                                             const std::string& glob_pattern);

} // namespace indextools
