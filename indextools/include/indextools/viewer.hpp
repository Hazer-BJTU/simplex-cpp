#pragma once

/**
 * @file viewer.hpp
 * @brief Read-only file/text viewing primitives for the indextools framework.
 *
 * The counterpart to editor.hpp: where editor.hpp mutates a source, viewer.hpp
 * only reads slices of it. Both operate on a read-only SplitedString view and
 * both emit the shared display-block JSON contract (see schema.hpp), so a read
 * result composes with search hits and diffs using the same coordinates and the
 * same wire format.
 *
 * Two addressing modes are offered:
 *
 *   - read_lines: LINE-level, the primary interface. Addresses a half-open
 *                 window of lines by 0-based index and returns a line-indexed
 *                 ("text") body. This is the natural unit for an AI coding
 *                 tool — edits, diffs and search hits are all line-addressed.
 *
 *   - read_bytes: BYTE-level, the escape hatch. Addresses a byte window and
 *                 returns the raw slice as a single ("content") string, NOT
 *                 split into lines. Use it when line semantics break down:
 *                 minified single-line files, precise slicing from a byte
 *                 offset, or resuming a large-file read at an exact position.
 *
 * Both are pagination-aware: the caller passes a maximum count, and the
 * returned block's meta reports the total size and a "Truncated" flag so a
 * frontend knows whether more data remains. Neither depends on language
 * plugins or the analyzer cache — any file SplitedString::read() can load
 * (source, config, logs, binary) is viewable.
 */

#include "indextools/split.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>

namespace indextools {

/**
 * @brief Read a window of lines and return a single line-indexed display block.
 *
 * Returns the lines in the half-open range [@p line_start, @p line_start +
 * @p max_lines), clamped to the available lines. Every returned line is tagged
 * "base". The result is a DisplayBlock[] of exactly one block whose "text" body
 * carries the lines and whose "meta" reports:
 *   - File        : @p file_path (label only; not read from disk here).
 *   - Lines       : "[first, last]" 0-based inclusive range actually returned
 *                   (or "[]" when the window is empty).
 *   - Total Lines : total number of lines in the source.
 *   - Truncated   : true if lines exist beyond the returned window.
 *
 * @param source_lines Read-only view of the source, split into lines.
 * @param line_start   0-based index of the first line to return.
 * @param max_lines    Maximum number of lines to return (page size). 0 yields
 *                     an empty body (but still reports totals/Truncated).
 * @param file_path    Path used only to label the block's "File" field.
 * @return A JSON array containing one display block (text body).
 */
nlohmann::json read_lines(
    const SplitedString& source_lines,
    size_t line_start,
    size_t max_lines,
    const std::filesystem::path& file_path
);

/**
 * @brief Read a window of bytes and return a single whole-content display block.
 *
 * Returns the bytes in the half-open range [@p byte_start, @p byte_start +
 * @p max_bytes), clamped to the source size, as one raw string (never split
 * into lines). The result is a DisplayBlock[] of exactly one block whose
 * "content" body is the slice and whose "meta" reports:
 *   - File        : @p file_path (label only).
 *   - Bytes       : "[first, last]" inclusive byte range returned (or "[]").
 *   - Total Bytes : total size of the source in bytes.
 *   - Truncated   : true if bytes exist beyond the returned window.
 *   - Binary      : true if the returned slice contains a NUL byte (a cheap
 *                   heuristic that the content may be binary/lossy as text).
 *
 * @param source_lines Read-only view of the source (only source() is used).
 * @param byte_start   0-based byte offset of the first byte to return.
 * @param max_bytes    Maximum number of bytes to return (page size). 0 yields
 *                     an empty content string (but still reports totals).
 * @param file_path    Path used only to label the block's "File" field.
 * @return A JSON array containing one display block (content body).
 */
nlohmann::json read_bytes(
    const SplitedString& source_lines,
    size_t byte_start,
    size_t max_bytes,
    const std::filesystem::path& file_path
);

} // namespace indextools
