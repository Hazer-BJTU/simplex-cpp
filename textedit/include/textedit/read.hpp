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

/** A model-safe bounded display. Metadata still describes the full selection. */
struct BoundedReadResult : ReadResult {
    /// The selection contains more display content than text could hold.
    bool output_truncated = false;
    /// At least one malformed UTF-8 byte was replaced in the displayed prefix.
    bool display_replaced = false;
};

struct BoundedLineReadResult : BoundedReadResult {
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
 * Computing total_lines scans the whole input, even for a small byte selection,
 * using constant additional space.
 */
[[nodiscard]] ReadResult read_bytes(
    std::string_view text,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format = ByteReadFormat::Plain);

/**
 * Read a regular file and apply read_lines to the observed bytes.
 *
 * Synchronous, follows symlinks, and does not modify content. The file is
 * scanned once with fixed buffers, so input memory does not scale with file
 * size; this variant still returns the whole selection it was asked for.
 * max_file_bytes (default 16 MiB) bounds the bytes read; a file that exceeds it,
 * including one that grows during the scan, throws std::length_error. The limit
 * is checked on observed bytes, never on metadata alone, and SIZE_MAX is invalid
 * because one lookahead byte must fit. Zero allows empty files. IO failures
 * throw std::system_error; invalid paths/limits throw std::invalid_argument.
 * A file modified by another actor while it is scanned is not a snapshot:
 * totals and the selection describe the bytes this scan observed.
 */
[[nodiscard]] LineReadResult read_file_lines(
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format = LineReadFormat::Plain,
    std::size_t max_file_bytes = 16 * 1024 * 1024);

/**
 * File counterpart of read_bytes with the same scan and error contract. Byte
 * selections may seek past uninteresting content, but total_lines is still an
 * exact count, so the source is scanned once end to end. For repeated
 * selections from one in-memory string, use read_bytes directly.
 */
[[nodiscard]] ReadResult read_file_bytes(
    const std::filesystem::path& path,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format = ByteReadFormat::Plain,
    std::size_t max_file_bytes = 16 * 1024 * 1024);

/**
 * Model-facing line read with an enforced display budget.
 *
 * Scans the whole input for totals and requested boundaries in O(bytes) time
 * and O(1) index storage. Only the selected prefix that fits output_bytes is
 * rendered; no full LineIndex or full formatted selection is constructed.
 * Valid UTF-8 scalars remain intact, malformed bytes become U+FFFD and set
 * display_replaced. Indexed separators and labels follow read_lines().
 * output_truncated means the selected display has more content; lines_read,
 * byte bounds, totals and reached_end still describe the full selection.
 * Zero output_bytes is allowed. The borrowed input is not retained.
 */
[[nodiscard]] BoundedLineReadResult read_lines_bounded(
    std::string_view text,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format,
    std::size_t output_bytes);

/**
 * Model-facing byte read with a bounded display and constant index storage.
 * Plain decodes like read_lines_bounded; HexEscaped uses four ASCII bytes per
 * source byte and never emits a partial escape. Input coordinates remain raw
 * byte offsets, so a selection may start inside a UTF-8 sequence.
 */
[[nodiscard]] BoundedReadResult read_bytes_bounded(
    std::string_view text,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format,
    std::size_t output_bytes);

/**
 * File counterparts with the same scan and byte-limit contract.
 *
 * These are the model-facing variants: only the displayed prefix within
 * output_bytes is ever rendered or retained, in addition to the row index state
 * of the selected rows. Malformed UTF-8 in that prefix is repaired, and rows cut
 * by the budget keep their true byte length so byte-range labels stay exact.
 */
[[nodiscard]] BoundedLineReadResult read_file_lines_bounded(
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format,
    std::size_t output_bytes,
    std::size_t max_file_bytes = 16 * 1024 * 1024);

[[nodiscard]] BoundedReadResult read_file_bytes_bounded(
    const std::filesystem::path& path,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format,
    std::size_t output_bytes,
    std::size_t max_file_bytes = 16 * 1024 * 1024);

} // namespace textedit
