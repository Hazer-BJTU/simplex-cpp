#include "textedit/read.hpp"

#include "fileio/read_prefix.hpp"
#include "textedit/line_index.hpp"
#include "line_break.hpp"
#include "read_detail.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace textedit {
namespace {

/// Remove exactly one recognized terminator for indexed display only.
std::string_view without_terminator(std::string_view line)
{
    for (const std::string_view ending : {
             "\r\n", "\xc2\x85", "\xe2\x80\xa8", "\xe2\x80\xa9",
             "\n", "\r", "\v", "\f"}) {
        if (line.ends_with(ending)) {
            return line.substr(0, line.size() - ending.size());
        }
    }
    return line;
}

/// Decimal padding is locale-independent; byte offsets always use ASCII digits.
void append_index(std::string& output, std::size_t value, std::size_t width)
{
    const auto digits = std::to_string(value);
    output.append(width - digits.size(), ' ');
    output += digits;
}

} // namespace

std::string detail::load_file(const std::filesystem::path& path, std::size_t limit)
{
    if (limit == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("file byte limit must leave room for one lookahead byte");
    }
    auto bytes = fileio::read_prefix(path, limit + 1);
    if (bytes.size() > limit) {
        throw std::length_error("file exceeds the configured byte limit");
    }
    return bytes;
}

LineReadResult read_lines(
    std::string_view text,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format)
{
    if (format != LineReadFormat::Plain && format != LineReadFormat::LineIndex &&
        format != LineReadFormat::ByteRange) {
        throw std::invalid_argument("unknown line read format");
    }
    const LineIndex index(text);
    if (start_line > index.line_count()) {
        throw std::out_of_range("start line is beyond the indexed text");
    }

    LineReadResult result;
    result.total_bytes = text.size();
    result.total_lines = index.line_count();
    result.start_line = start_line;
    result.lines_read = std::min(line_count, index.line_count() - start_line);
    result.start_byte = start_line == index.line_count()
        ? text.size()
        : index.line_start(start_line);
    const auto end_line = start_line + result.lines_read;
    result.end_byte = end_line == index.line_count()
        ? text.size()
        : index.line_start(end_line);
    result.reached_end = end_line == index.line_count();
    if (result.lines_read == 0) {
        return result;
    }
    if (format == LineReadFormat::Plain) {
        result.text = text.substr(result.start_byte, result.end_byte - result.start_byte);
        return result;
    }

    const auto width = std::to_string(
        format == LineReadFormat::LineIndex ? end_line - 1 : result.end_byte).size();
    for (std::size_t line = start_line; line < end_line; ++line) {
        if (line != start_line) {
            result.text += '\n';
        }
        const auto start = index.line_start(line);
        const auto count = index.line_byte_count(line);
        if (format == LineReadFormat::LineIndex) {
            append_index(result.text, line, width);
        } else {
            result.text += '[';
            append_index(result.text, start, width);
            result.text += ':';
            append_index(result.text, start + count, width);
            result.text += ']';
        }
        result.text += " | ";
        result.text += without_terminator(text.substr(start, count));
    }
    return result;
}

ReadResult read_bytes(
    std::string_view text,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format)
{
    if (format != ByteReadFormat::Plain && format != ByteReadFormat::HexEscaped) {
        throw std::invalid_argument("unknown byte read format");
    }
    if (start_byte > text.size()) {
        throw std::out_of_range("start byte is beyond the input");
    }
    const auto count = std::min(byte_count, text.size() - start_byte);
    ReadResult result;
    result.total_bytes = text.size();
    result.total_lines = detail::count_lines(text);
    result.start_byte = start_byte;
    result.end_byte = start_byte + count;
    result.reached_end = result.end_byte == text.size();
    const auto selected = text.substr(start_byte, count);
    if (format == ByteReadFormat::Plain) {
        result.text = selected;
    } else {
        if (count > result.text.max_size() / 4) {
            throw std::length_error("hexadecimal byte output is too large");
        }
        result.text.reserve(count * 4);
        constexpr char digits[] = "0123456789ABCDEF";
        for (unsigned char byte : selected) {
            result.text += "\\x";
            result.text += digits[byte >> 4];
            result.text += digits[byte & 0x0f];
        }
    }
    return result;
}

LineReadResult read_file_lines(
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format,
    std::size_t max_file_bytes)
{
    const auto bytes = detail::load_file(path, max_file_bytes);
    return read_lines(bytes, start_line, line_count, format);
}

ReadResult read_file_bytes(
    const std::filesystem::path& path,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format,
    std::size_t max_file_bytes)
{
    const auto bytes = detail::load_file(path, max_file_bytes);
    return read_bytes(bytes, start_byte, byte_count, format);
}

} // namespace textedit
