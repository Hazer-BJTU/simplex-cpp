#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace textedit {

enum class LineReadFormat {
    Plain,
    LineIndex,
    ByteRange
};

enum class ByteReadFormat {
    Plain,
    HexEscaped
};

/** Rendered selection plus coordinates in the original, unformatted input. */
struct ReadResult {
    std::string text;
    /// Full input size, independent of the selection and rendered output.
    std::size_t total_bytes = 0;
    /// Full logical line count using LineIndex rules, including the final empty
    /// line after a terminator. Empty input has one line. Also set in byte mode.
    std::size_t total_lines = 0;
    /// Half-open byte range; offsets never refer to the formatted output.
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    /// True when no further entries remain: bytes in byte mode, logical lines
    /// in line mode (including a possible zero-byte final line).
    bool reached_end = false;
};

/** Line selection, including zero-byte lines defined by LineIndex. */
struct LineReadResult : ReadResult {
    std::size_t start_line = 0;
    std::size_t lines_read = 0;
};

/**
 * Read up to line_count logical lines, starting at a zero-based line index.
 *
 * Uses LineIndex's newline and trailing-empty-line rules. Plain returns the
 * exact selected bytes including original terminators. LineIndex prefixes each
 * line with a right-aligned decimal line number and " | ". ByteRange prefixes
 * each line with "[start:end] | ", with both decimal fields right-aligned to
 * one common width. Ranges are half-open and include original terminators.
 * Width is computed from this selection, not the whole input.
 *
 * Indexed formats remove each original terminator and join display rows with
 * LF, without adding a final LF. They do not escape other content (including
 * tabs, NUL or terminal controls), and are not a terminal-sanitization layer.
 * Columns/payload bytes are not padded or reflowed. A zero-byte logical line
 * still produces an indexed display row. Plain output for that line is empty.
 *
 * Counts clamp to available lines; zero selects nothing. start_line equal to
 * the number of logical lines is an empty EOF selection; greater values throw
 * std::out_of_range. Unknown format values throw std::invalid_argument.
 * The input view is borrowed only for this call. No UTF-8 validation is required.
 */
[[nodiscard]] LineReadResult read_lines(
    std::string_view text,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format = LineReadFormat::Plain);

/**
 * Read up to byte_count bytes starting at a zero-based byte offset.
 *
 * Plain preserves every byte; HexEscaped emits exactly four ASCII characters
 * per selected byte: backslash, lowercase x, and two uppercase hexadecimal
 * digits (for example "\\x00\\x41\\xFF"). No spaces or newlines are inserted.
 * Selection may split a UTF-8 character. Counts clamp to available bytes; zero
 * selects nothing. EOF is valid; offsets beyond EOF throw std::out_of_range.
 * Unknown format values throw std::invalid_argument. The view is not retained.
 * Computing total_lines scans the whole input, even for a small byte selection.
 */
[[nodiscard]] ReadResult read_bytes(
    std::string_view text,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format = ByteReadFormat::Plain);

/**
 * Read a regular file and apply read_lines to the loaded bytes.
 *
 * Synchronous, follows symlinks, and does not modify content. The whole file is
 * loaded to determine its logical lines. max_file_bytes (default 16 MiB) bounds
 * input memory; one extra byte detects oversize input, which throws
 * std::length_error instead of returning a misleading partial last line.
 * This bound does not include index/output memory. Zero allows empty files.
 * SIZE_MAX is invalid because the lookahead byte must fit. Files changing while
 * read do not provide a consistent filesystem snapshot. IO failures throw
 * std::system_error; invalid paths/limits throw std::invalid_argument.
 */
[[nodiscard]] LineReadResult read_file_lines(
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format = LineReadFormat::Plain,
    std::size_t max_file_bytes = 16 * 1024 * 1024);

/**
 * File counterpart of read_bytes. Uses the same bounded whole-file loading and
 * error contract as read_file_lines. For repeated selections from one loaded
 * string, use read_bytes directly to avoid repeated disk reads.
 */
[[nodiscard]] ReadResult read_file_bytes(
    const std::filesystem::path& path,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format = ByteReadFormat::Plain,
    std::size_t max_file_bytes = 16 * 1024 * 1024);

} // namespace textedit
