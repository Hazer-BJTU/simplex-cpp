#include "textedit/read.hpp"

#include "display.hpp"
#include "line_break.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace textedit {
namespace {

using detail::DisplayWriter;

/** Find one logical line start without storing a vector of preceding starts. */
std::size_t line_start_at(std::string_view text, std::size_t target)
{
    if (target == 0) return 0;
    std::size_t line = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto width = detail::line_break_width(text.substr(offset));
        offset += width == 0 ? 1 : width;
        if (width != 0 && ++line == target) return offset;
    }
    return text.size(); // target == total_lines is the valid EOF selection.
}

/** Return the end of a line's content and the start of the next line. */
struct LineEnd {
    std::size_t content_end;
    std::size_t next_start;
};

LineEnd find_line_end(std::string_view text, std::size_t start)
{
    for (std::size_t offset = start; offset < text.size(); ++offset) {
        const auto width = detail::line_break_width(text.substr(offset));
        if (width != 0) return {offset, offset + width};
    }
    return {text.size(), text.size()};
}

std::string padded(std::size_t number, std::size_t width)
{
    auto digits = std::to_string(number);
    return std::string(width - digits.size(), ' ') + digits;
}

} // namespace

BoundedLineReadResult read_lines_bounded(
    std::string_view text,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format,
    std::size_t output_bytes)
{
    if (format != LineReadFormat::Plain && format != LineReadFormat::LineIndex &&
        format != LineReadFormat::ByteRange) {
        throw std::invalid_argument("unknown line read format");
    }

    BoundedLineReadResult result;
    result.total_bytes = text.size();
    result.total_lines = detail::count_lines(text);
    if (start_line > result.total_lines) {
        throw std::out_of_range("start line is beyond the indexed text");
    }
    result.start_line = start_line;
    result.lines_read = std::min(line_count, result.total_lines - start_line);
    const auto end_line = start_line + result.lines_read;
    result.start_byte = line_start_at(text, start_line);
    result.end_byte = line_start_at(text, end_line);
    result.reached_end = end_line == result.total_lines;

    DisplayWriter writer(output_bytes);
    if (result.lines_read != 0) {
        if (format == LineReadFormat::Plain) {
            writer.append(text.substr(result.start_byte,
                                      result.end_byte - result.start_byte));
        } else {
            const auto width = std::to_string(
                format == LineReadFormat::LineIndex ? end_line - 1 : result.end_byte).size();
            auto offset = result.start_byte;
            for (auto line = start_line; line < end_line; ++line) {
                const auto end = find_line_end(text, offset);
                if (line != start_line) writer.append("\n");
                if (writer.truncated()) break;
                if (format == LineReadFormat::LineIndex) {
                    writer.append(padded(line, width) + " | ");
                } else {
                    writer.append("[" + padded(offset, width) + ":" +
                                  padded(end.next_start, width) + "] | ");
                }
                if (writer.truncated()) break;
                writer.append(text.substr(offset, end.content_end - offset));
                if (writer.truncated()) break;
                offset = end.next_start;
            }
        }
    }
    result.output_truncated = writer.truncated();
    result.display_replaced = writer.replaced();
    result.text = writer.take();
    return result;
}

BoundedReadResult read_bytes_bounded(
    std::string_view text,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format,
    std::size_t output_bytes)
{
    if (format != ByteReadFormat::Plain && format != ByteReadFormat::HexEscaped) {
        throw std::invalid_argument("unknown byte read format");
    }
    if (start_byte > text.size()) {
        throw std::out_of_range("start byte is beyond the input");
    }
    BoundedReadResult result;
    result.total_bytes = text.size();
    result.total_lines = detail::count_lines(text);
    result.start_byte = start_byte;
    result.end_byte = start_byte + std::min(byte_count, text.size() - start_byte);
    result.reached_end = result.end_byte == text.size();

    DisplayWriter writer(output_bytes);
    const auto selected = text.substr(start_byte, result.end_byte - start_byte);
    if (format == ByteReadFormat::HexEscaped) writer.append_hex(selected);
    else writer.append(selected);
    result.output_truncated = writer.truncated();
    result.display_replaced = writer.replaced();
    result.text = writer.take();
    return result;
}

} // namespace textedit
