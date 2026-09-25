#include "textedit/read.hpp"

#include "line_break.hpp"
#include "read_detail.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace textedit {
namespace {

/**
 * Append validated UTF-8 scalars while enforcing the output byte budget.
 *
 * Invalid source bytes each become U+FFFD. This decision is made before any
 * clipping, so an arbitrary run of continuation bytes cannot make the writer
 * retreat or hide display_replaced. The writer never retains input views.
 */
class DisplayWriter {
public:
    explicit DisplayWriter(std::size_t budget) : budget_(budget) {}

    void append(std::string_view source)
    {
        for (std::size_t offset = 0; offset < source.size();) {
            const auto first = static_cast<unsigned char>(source[offset]);
            std::size_t width = 1;
            bool valid = first < 0x80;
            if (!valid) {
                if (first >= 0xc2 && first <= 0xdf) width = 2;
                else if (first >= 0xe0 && first <= 0xef) width = 3;
                else if (first >= 0xf0 && first <= 0xf4) width = 4;
                if (width > 1 && source.size() - offset >= width) {
                    valid = true;
                    for (std::size_t index = 1; index < width; ++index) {
                        const auto byte = static_cast<unsigned char>(source[offset + index]);
                        if (byte < 0x80 || byte > 0xbf ||
                            (index == 1 && first == 0xe0 && byte < 0xa0) ||
                            (index == 1 && first == 0xed && byte > 0x9f) ||
                            (index == 1 && first == 0xf0 && byte < 0x90) ||
                            (index == 1 && first == 0xf4 && byte > 0x8f)) {
                            valid = false;
                            break;
                        }
                    }
                }
            }

            const std::size_t emitted = valid ? width : 3;
            if (emitted > budget_ - output_.size()) {
                truncated_ = true;
                return;
            }
            if (valid) {
                output_.append(source.data() + offset, width);
                offset += width;
            } else {
                output_ += "\xef\xbf\xbd";
                replaced_ = true;
                ++offset;
            }
        }
    }

    /** Hex output is ASCII and expands every byte by exactly four bytes. */
    void append_hex(std::string_view source)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        const auto fits = std::min(source.size(), (budget_ - output_.size()) / 4);
        for (std::size_t offset = 0; offset < fits; ++offset) {
            const auto byte = static_cast<unsigned char>(source[offset]);
            output_ += "\\x";
            output_ += digits[byte >> 4];
            output_ += digits[byte & 0x0f];
        }
        truncated_ |= fits != source.size();
    }

    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] bool replaced() const noexcept { return replaced_; }
    [[nodiscard]] std::string take() { return std::move(output_); }

private:
    std::size_t budget_;
    std::string output_;
    bool truncated_ = false;
    bool replaced_ = false;
};

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

BoundedLineReadResult read_file_lines_bounded(
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t line_count,
    LineReadFormat format,
    std::size_t output_bytes,
    std::size_t max_file_bytes)
{
    const auto bytes = detail::load_file(path, max_file_bytes);
    return read_lines_bounded(bytes, start_line, line_count, format, output_bytes);
}

BoundedReadResult read_file_bytes_bounded(
    const std::filesystem::path& path,
    std::size_t start_byte,
    std::size_t byte_count,
    ByteReadFormat format,
    std::size_t output_bytes,
    std::size_t max_file_bytes)
{
    const auto bytes = detail::load_file(path, max_file_bytes);
    return read_bytes_bounded(bytes, start_byte, byte_count, format,
                              output_bytes);
}

} // namespace textedit
