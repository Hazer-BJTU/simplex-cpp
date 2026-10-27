#include "textedit/read.hpp"

#include "display.hpp"
#include "line_break.hpp"

#include "fileio/streaming.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace textedit {
namespace {

using detail::DisplayWriter;

/// Bytes a complete UTF-8 scalar may add past a retention cap.
constexpr std::size_t kScalarSlack = 3;

/** Grow a retention cap without wrapping an unbounded (SIZE_MAX) budget. */
std::size_t with_slack(std::size_t cap, std::size_t slack)
{
    return cap > std::numeric_limits<std::size_t>::max() - slack ? cap : cap + slack;
}

struct ScanTotals {
    std::size_t bytes = 0;
    std::size_t lines = 1;
};

/**
 * One bounded sequential scan that reports every logical line to `sink`.
 *
 * The sink sees rows in order: begin_row(), any number of content() pieces,
 * then either terminator() (the row ended) or finish() for the final row. The
 * byte limit is enforced on observed bytes, so growth during the scan cannot
 * extend it; metadata from open never authorizes a longer read.
 */
template <typename Sink>
ScanTotals scan_file(
    const std::filesystem::path& path, std::size_t max_file_bytes, Sink& sink)
{
    auto file = fileio::SequentialFile::open(path);
    ScanTotals totals;
    std::string carry;
    std::string window;
    std::size_t line = 0;
    sink.begin_row(line, 0);
    for (;;) {
        const auto chunk = file.next();
        if (chunk.empty()) break;
        if (file.observed_bytes() > max_file_bytes) {
            throw std::length_error("file exceeds the configured byte limit");
        }
        window.assign(carry);
        window.append(chunk);
        carry.clear();
        const auto base = totals.bytes - (window.size() - chunk.size());
        totals.bytes += chunk.size();
        std::size_t scanned = 0;
        std::size_t offset = 0;
        while (offset < window.size()) {
            const auto probe = detail::probe_break(window, offset);
            if (probe.incomplete) break;
            if (probe.width == 0) {
                ++offset;
                continue;
            }
            if (offset > scanned) {
                sink.content(base + scanned,
                             std::string_view(window).substr(scanned, offset - scanned));
            }
            sink.terminator(base + offset,
                            std::string_view(window).substr(offset, probe.width));
            ++totals.lines;
            ++line;
            offset += probe.width;
            scanned = offset;
            sink.begin_row(line, base + offset);
        }
        if (offset > scanned) {
            sink.content(base + scanned,
                         std::string_view(window).substr(scanned, offset - scanned));
        }
        carry.assign(window.substr(offset));
    }
    if (!carry.empty()) {
        const auto base = totals.bytes - carry.size();
        const auto width = detail::line_break_width(carry);
        if (width != 0) {
            sink.terminator(base, std::string_view(carry).substr(0, width));
            ++totals.lines;
            ++line;
            sink.begin_row(line, base + width);
        } else {
            sink.content(base, carry);
        }
    }
    sink.finish(totals.bytes);
    return totals;
}

/**
 * Output accumulator.
 *
 * The bounded model-facing reads repair malformed UTF-8 and enforce a display
 * budget. The whole-selection reads return the exact selected bytes, matching
 * the string-based read_lines/read_bytes contracts, so they append raw bytes.
 */
class OutputText {
public:
    OutputText(std::size_t budget, bool repair) : writer_(budget), repair_(repair) {}

    void append(std::string_view bytes)
    {
        if (repair_) writer_.append(bytes);
        else raw_.append(bytes);
    }

    void append_hex(std::string_view bytes)
    {
        if (repair_) writer_.append_hex(bytes);
        else raw_ += hex_escaped(bytes);
    }

    [[nodiscard]] bool truncated() const noexcept { return repair_ && writer_.truncated(); }
    [[nodiscard]] bool replaced() const noexcept { return repair_ && writer_.replaced(); }
    void mark_truncated() noexcept { if (repair_) writer_.mark_truncated(); }
    [[nodiscard]] std::string take() { return repair_ ? writer_.take() : std::move(raw_); }

private:
    static std::string hex_escaped(std::string_view bytes)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string output;
        output.reserve(bytes.size() * 4);
        for (unsigned char byte : bytes) {
            output += "\\x";
            output += digits[byte >> 4];
            output += digits[byte & 0x0f];
        }
        return output;
    }

    detail::DisplayWriter writer_;
    std::string raw_;
    bool repair_;
};

/** Retain the raw bytes of a line selection, for LineReadFormat::Plain. */
struct PlainSelectionSink {
    std::size_t start_line = 0;
    std::size_t line_count = 0;
    std::size_t cap = 0;
    std::string bytes;
    bool started = false;
    std::optional<std::size_t> start_byte;
    std::optional<std::size_t> end_byte;

    PlainSelectionSink(std::size_t first_line, std::size_t rows, std::size_t budget)
        : start_line(first_line), line_count(rows), cap(budget) {}

    void begin_row(std::size_t line, std::size_t start)
    {
        if (line == start_line) {
            start_byte = start;
            started = true;
        }
        if (started && !end_byte && line >= start_line && line - start_line == line_count) {
            end_byte = start;
        }
    }

    void content(std::size_t offset, std::string_view piece) { keep(offset, piece); }
    void terminator(std::size_t offset, std::string_view terminator) { keep(offset, terminator); }

    void finish(std::size_t total)
    {
        if (!started) {
            start_byte = total;
            end_byte = total;
        } else if (!end_byte) {
            end_byte = total;
        }
    }

private:
    void keep(std::size_t, std::string_view piece)
    {
        if (!started || end_byte || piece.empty()) return;
        const auto room = cap - bytes.size();
        bytes.append(piece.data(), std::min(room, piece.size()));
    }
};

/**
 * Collect indexed rows as one '\n'-joined buffer plus one terminator width each.
 *
 * Rows never contain a terminator, so a trailing '\n' after each fully stored
 * row makes the layout self-describing without a per-row offset vector. The
 * content cap bounds retained bytes by the display budget, and a row cut by the
 * cap keeps its true length so byte-range labels stay exact.
 */
struct RowSelectionSink {
    std::size_t start_line = 0;
    std::size_t line_count = 0;
    std::size_t cap = 0;
    std::string stored_content;
    std::vector<std::uint8_t> terminators;
    std::size_t stored = 0;
    bool clipped = false;
    std::size_t clipped_length = 0;
    std::size_t clipped_terminator = 0;
    bool started = false;
    std::optional<std::size_t> start_byte;
    std::optional<std::size_t> end_byte;
    bool selected = false;
    bool clip_recorded = false;
    std::size_t length = 0;

    RowSelectionSink(std::size_t first_line, std::size_t rows, std::size_t budget)
        : start_line(first_line), line_count(rows), cap(budget) {}

    void begin_row(std::size_t line, std::size_t start)
    {
        selected = line >= start_line && line - start_line < line_count;
        length = 0;
        if (line == start_line) {
            start_byte = start;
            started = true;
        }
        if (started && !end_byte && line >= start_line && line - start_line == line_count) {
            end_byte = start;
        }
    }

    void content(std::size_t, std::string_view piece)
    {
        if (!selected || clip_recorded || piece.empty()) return;
        length += piece.size();
        if (clipped) return;
        const auto take = std::min(cap - stored, piece.size());
        stored_content.append(piece.data(), take);
        stored += take;
        if (take != piece.size()) clipped = true;
    }

    void terminator(std::size_t, std::string_view terminator)
    {
        if (selected && clipped && !clip_recorded) {
            clipped_length = length;
            clipped_terminator = terminator.size();
            clip_recorded = true;
        } else if (selected && !clipped) {
            terminators.push_back(static_cast<std::uint8_t>(terminator.size()));
            stored_content += '\n';
        }
        selected = false;
        length = 0;
    }

    void finish(std::size_t total)
    {
        if (!started) {
            start_byte = total;
            end_byte = total;
        } else if (!end_byte) {
            end_byte = total;
        }
        if (selected && clipped && !clip_recorded) {
            clipped_length = length;
            clipped_terminator = 0;
            clip_recorded = true;
        } else if (selected && !clipped) {
            // A final row without a terminator is still a complete stored row.
            terminators.push_back(0);
            stored_content += '\n';
        }
        selected = false;
        length = 0;
    }
};

/** Retain a byte range of the raw stream, for ByteReadFormat reads. */
struct ByteSelectionSink {
    std::size_t start = 0;
    std::size_t count = 0;
    std::size_t cap = 0;
    std::string bytes;

    ByteSelectionSink(std::size_t first_byte, std::size_t bytes_wanted, std::size_t budget)
        : start(first_byte), count(bytes_wanted), cap(budget) {}

    void begin_row(std::size_t, std::size_t) {}
    void content(std::size_t offset, std::string_view piece) { keep(offset, piece); }
    void terminator(std::size_t offset, std::string_view terminator) { keep(offset, terminator); }
    void finish(std::size_t) {}

private:
    void keep(std::size_t offset, std::string_view piece)
    {
        if (piece.empty()) return;
        const auto skip = offset <= start ? start - offset : 0;
        if (skip >= piece.size()) return;
        const auto position = offset <= start ? 0 : offset - start;
        if (position >= count) return;
        const auto take = std::min(piece.size() - skip, count - position);
        const auto room = cap - bytes.size();
        bytes.append(piece.data() + skip, std::min(take, room));
    }
};

BoundedLineReadResult read_lines_stream(
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format,
    std::size_t output_bytes,
    std::size_t max_file_bytes,
    bool repair)
{
    if (format != LineReadFormat::Plain && format != LineReadFormat::LineIndex &&
        format != LineReadFormat::ByteRange) {
        throw std::invalid_argument("unknown line read format");
    }
    BoundedLineReadResult result;
    OutputText writer(output_bytes, repair);
    if (format == LineReadFormat::Plain) {
        PlainSelectionSink sink(start_line, line_count, with_slack(output_bytes, kScalarSlack));
        const auto totals = scan_file(path, max_file_bytes, sink);
        result.total_bytes = totals.bytes;
        result.total_lines = totals.lines;
        if (start_line > result.total_lines) {
            throw std::out_of_range("start line is beyond the indexed text");
        }
        result.start_line = start_line;
        result.lines_read = std::min(line_count, result.total_lines - start_line);
        result.start_byte = *sink.start_byte;
        result.end_byte = *sink.end_byte;
        result.reached_end = start_line + result.lines_read == result.total_lines;
        writer.append(sink.bytes);
    } else {
        RowSelectionSink sink(start_line, line_count, output_bytes);
        const auto totals = scan_file(path, max_file_bytes, sink);
        result.total_bytes = totals.bytes;
        result.total_lines = totals.lines;
        if (start_line > result.total_lines) {
            throw std::out_of_range("start line is beyond the indexed text");
        }
        result.start_line = start_line;
        result.lines_read = std::min(line_count, result.total_lines - start_line);
        const auto end_line = start_line + result.lines_read;
        result.start_byte = *sink.start_byte;
        result.end_byte = *sink.end_byte;
        result.reached_end = end_line == result.total_lines;
        if (result.lines_read != 0) {
            const auto width = std::to_string(
                format == LineReadFormat::LineIndex ? end_line - 1 : result.end_byte).size();
            const auto padded = [width](std::string& output, std::size_t value) {
                const auto digits = std::to_string(value);
                output.append(width - digits.size(), ' ');
                output += digits;
            };
            std::size_t offset = result.start_byte;
            std::size_t begin = 0;
            std::size_t row = 0;
            const auto render_row = [&](std::string_view piece, std::size_t length,
                                        std::size_t terminator, bool clipped_row) {
                if (row != 0) {
                    writer.append("\n");
                    if (writer.truncated()) return false;
                }
                std::string label;
                if (format == LineReadFormat::LineIndex) {
                    padded(label, start_line + row);
                } else {
                    label += '[';
                    padded(label, offset);
                    label += ':';
                    padded(label, offset + length + terminator);
                    label += ']';
                }
                label += " | ";
                writer.append(label);
                if (writer.truncated()) return false;
                writer.append(piece);
                if (clipped_row) {
                    writer.mark_truncated();
                    return false;
                }
                offset += length + terminator;
                return !writer.truncated();
            };
            for (; row < sink.terminators.size(); ++row) {
                const auto end = sink.stored_content.find('\n', begin);
                const auto piece = std::string_view(sink.stored_content).substr(begin, end - begin);
                const auto more = render_row(piece, piece.size(), sink.terminators[row], false);
                begin = end + 1;
                if (!more) break;
            }
            if (row == sink.terminators.size() && sink.clipped) {
                (void)render_row(sink.stored_content.substr(begin), sink.clipped_length,
                                 sink.clipped_terminator, true);
            }
        }
    }
    result.output_truncated = writer.truncated();
    result.display_replaced = writer.replaced();
    result.text = writer.take();
    return result;
}

BoundedReadResult read_bytes_stream(
    const std::filesystem::path& path,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format,
    std::size_t output_bytes,
    std::size_t max_file_bytes,
    bool repair)
{
    if (format != ByteReadFormat::Plain && format != ByteReadFormat::HexEscaped) {
        throw std::invalid_argument("unknown byte read format");
    }
    const auto cap = format == ByteReadFormat::HexEscaped
        ? with_slack(output_bytes / 4, kScalarSlack)
        : with_slack(output_bytes, kScalarSlack);
    ByteSelectionSink sink(start_byte, byte_count, cap);
    const auto totals = scan_file(path, max_file_bytes, sink);
    if (start_byte > totals.bytes) {
        throw std::out_of_range("start byte is beyond the input");
    }
    BoundedReadResult result;
    result.total_bytes = totals.bytes;
    result.total_lines = totals.lines;
    result.start_byte = start_byte;
    result.end_byte = start_byte + std::min(byte_count, totals.bytes - start_byte);
    result.reached_end = result.end_byte == totals.bytes;
    OutputText writer(output_bytes, repair);
    if (format == ByteReadFormat::HexEscaped) writer.append_hex(sink.bytes);
    else writer.append(sink.bytes);
    if (sink.bytes.size() < std::min(byte_count, totals.bytes - start_byte)) {
        writer.mark_truncated();
    }
    result.output_truncated = writer.truncated();
    result.display_replaced = writer.replaced();
    result.text = writer.take();
    return result;
}

void check_limit(std::size_t max_file_bytes)
{
    if (max_file_bytes == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("file byte limit must leave room for one lookahead byte");
    }
}

} // namespace

LineReadResult read_file_lines(
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format,
    std::size_t max_file_bytes)
{
    check_limit(max_file_bytes);
    const auto bounded = read_lines_stream(
        path, start_line, line_count, format,
        std::numeric_limits<std::size_t>::max(), max_file_bytes, false);
    LineReadResult result;
    static_cast<ReadResult&>(result) = static_cast<const ReadResult&>(bounded);
    result.start_line = bounded.start_line;
    result.lines_read = bounded.lines_read;
    return result;
}

ReadResult read_file_bytes(
    const std::filesystem::path& path,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format,
    std::size_t max_file_bytes)
{
    check_limit(max_file_bytes);
    const auto bounded = read_bytes_stream(
        path, start_byte, byte_count, format,
        std::numeric_limits<std::size_t>::max(), max_file_bytes, false);
    return static_cast<const ReadResult&>(bounded);
}

BoundedLineReadResult read_file_lines_bounded(
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format,
    std::size_t output_bytes,
    std::size_t max_file_bytes)
{
    check_limit(max_file_bytes);
    return read_lines_stream(path, start_line, line_count, format,
                             output_bytes, max_file_bytes, true);
}

BoundedReadResult read_file_bytes_bounded(
    const std::filesystem::path& path,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format,
    std::size_t output_bytes,
    std::size_t max_file_bytes)
{
    check_limit(max_file_bytes);
    return read_bytes_stream(path, start_byte, byte_count, format,
                             output_bytes, max_file_bytes, true);
}

} // namespace textedit
