#define BOOST_TEST_MODULE TextRead
#include <boost/test/unit_test.hpp>

#include "textedit/read.hpp"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace {
constexpr auto all = std::numeric_limits<std::size_t>::max();
using textedit::ByteReadFormat;
using textedit::LineReadFormat;

/// Own a temporary directory, including cleanup on failed assertions.
struct Scratch {
    std::filesystem::path root;
    Scratch()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "simplex-read-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) {
            throw std::runtime_error("cannot create read test directory");
        }
        root = directory;
    }
    ~Scratch()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path write(const std::string& bytes)
    {
        const auto path = root / "input";
        std::ofstream stream(path, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        BOOST_REQUIRE(stream.good());
        return path;
    }
};
}

BOOST_AUTO_TEST_CASE(plain_lines_preserve_original_bytes_and_ranges)
{
    const std::string text = "a\r\nb\rc\nD";
    const auto result = textedit::read_lines(text, 1, 2);
    BOOST_TEST(result.text == "b\rc\n");
    BOOST_TEST(result.start_byte == 3u);
    BOOST_TEST(result.end_byte == 7u);
    BOOST_TEST(result.start_line == 1u);
    BOOST_TEST(result.lines_read == 2u);
    BOOST_TEST(!result.reached_end);
    const auto tail = textedit::read_lines(text, 2, all);
    BOOST_TEST(tail.text == "c\nD");
    BOOST_TEST(tail.lines_read == 2u);
    BOOST_TEST(tail.reached_end);
}

BOOST_AUTO_TEST_CASE(line_numbers_align_across_decimal_boundaries)
{
    std::string text;
    for (unsigned line = 0; line < 12; ++line) {
        text += "x\n";
    }
    const auto result = textedit::read_lines(text, 8, 4, LineReadFormat::LineIndex);
    BOOST_TEST(result.text == " 8 | x\n 9 | x\n10 | x\n11 | x");
    BOOST_TEST(result.start_byte == 16u);
    BOOST_TEST(result.end_byte == 24u);
    BOOST_TEST(!result.reached_end); // The final zero-byte line is still available.
    BOOST_TEST(textedit::read_lines(text, 8, 1, LineReadFormat::LineIndex).text == "8 | x");
}

BOOST_AUTO_TEST_CASE(byte_ranges_align_both_fields_and_include_terminators)
{
    const auto result = textedit::read_lines("abc\r\ndefgh\nZ", 0, all, LineReadFormat::ByteRange);
    BOOST_TEST(result.text == "[ 0: 5] | abc\n[ 5:11] | defgh\n[11:12] | Z");
    BOOST_TEST(result.reached_end);
    const auto subset = textedit::read_lines("abc\r\ndefgh\nZ", 1, 1, LineReadFormat::ByteRange);
    BOOST_TEST(subset.text == "[ 5:11] | defgh");
}

BOOST_AUTO_TEST_CASE(mixed_newlines_normalize_only_in_indexed_display)
{
    for (const std::string ending : {
             "\n", "\r\n", "\r", "\v", "\f", "\xc2\x85", "\xe2\x80\xa8", "\xe2\x80\xa9"}) {
        const auto text = "a" + ending + "b";
        BOOST_TEST(textedit::read_lines(text, 0, all).text == text);
        BOOST_TEST(textedit::read_lines(text, 0, all, LineReadFormat::LineIndex).text ==
                   "0 | a\n1 | b");
        const auto last = textedit::read_lines("a" + ending, 1, 1, LineReadFormat::ByteRange);
        const auto size = std::to_string(1 + ending.size());
        BOOST_TEST(last.text == "[" + size + ":" + size + "] | ");
    }
    const std::string binary("\xef\xbb\xbf\0\xff\n", 6);
    BOOST_TEST(textedit::read_lines(binary, 0, 1).text == binary);
    BOOST_TEST(textedit::read_lines(binary, 0, 1, LineReadFormat::LineIndex).text ==
               "0 | " + binary.substr(0, 5));
}

BOOST_AUTO_TEST_CASE(empty_lines_empty_selections_and_eof_are_explicit)
{
    const auto empty = textedit::read_lines("", 0, 1, LineReadFormat::LineIndex);
    BOOST_TEST(empty.text == "0 | ");
    BOOST_TEST(empty.lines_read == 1u);
    BOOST_TEST(empty.reached_end);
    BOOST_TEST(textedit::read_lines("\n\n", 0, all, LineReadFormat::LineIndex).text ==
               "0 | \n1 | \n2 | ");
    const auto zero = textedit::read_lines("a\nb", 1, 0, LineReadFormat::ByteRange);
    BOOST_TEST(zero.text.empty());
    BOOST_TEST(zero.start_byte == 2u);
    BOOST_TEST(zero.end_byte == 2u);
    BOOST_TEST(!zero.reached_end);
    const auto eof = textedit::read_lines("a\nb", 2, all);
    BOOST_TEST(eof.text.empty());
    BOOST_TEST(eof.lines_read == 0u);
    BOOST_TEST(eof.start_byte == 3u);
    BOOST_TEST(eof.reached_end);
}

BOOST_AUTO_TEST_CASE(byte_mode_preserves_or_hex_escapes_every_selected_byte)
{
    const std::string bytes("A\0\n\xc3\xa9\xff", 6);
    BOOST_TEST(textedit::read_bytes(bytes, 0, all).text == bytes);
    const auto escaped = textedit::read_bytes(bytes, 1, all, ByteReadFormat::HexEscaped);
    BOOST_TEST(escaped.text == "\\x00\\x0A\\xC3\\xA9\\xFF");
    BOOST_TEST(escaped.start_byte == 1u);
    BOOST_TEST(escaped.end_byte == 6u);
    BOOST_TEST(escaped.reached_end);
    BOOST_TEST(textedit::read_bytes(bytes, 4, 1).text == std::string("\xa9"));
    BOOST_TEST(textedit::read_bytes(bytes, 3, 1, ByteReadFormat::HexEscaped).text == "\\xC3");
    BOOST_TEST(textedit::read_bytes(bytes, 2, 0).text.empty());
    BOOST_TEST(textedit::read_bytes(bytes, bytes.size(), all).reached_end);
    BOOST_TEST(textedit::read_bytes("", 0, all).reached_end);

    std::string every_byte;
    constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned byte = 0; byte < 256; ++byte) {
        every_byte += static_cast<char>(byte);
    }
    const auto output = textedit::read_bytes(every_byte, 0, all, ByteReadFormat::HexEscaped).text;
    BOOST_REQUIRE_EQUAL(output.size(), 1024u);
    for (unsigned byte = 0; byte < 256; ++byte) {
        BOOST_TEST(output[byte * 4] == '\\');
        BOOST_TEST(output[byte * 4 + 1] == 'x');
        BOOST_TEST(output[byte * 4 + 2] == digits[byte >> 4]);
        BOOST_TEST(output[byte * 4 + 3] == digits[byte & 15]);
    }
}

BOOST_AUTO_TEST_CASE(invalid_ranges_and_formats_throw_without_overflow)
{
    BOOST_CHECK_THROW((void)textedit::read_lines("x", 2, 0), std::out_of_range);
    BOOST_CHECK_THROW((void)textedit::read_lines("x", all, all), std::out_of_range);
    BOOST_CHECK_THROW((void)textedit::read_bytes("x", 2, 0), std::out_of_range);
    BOOST_CHECK_THROW((void)textedit::read_bytes("x", all, all), std::out_of_range);
    BOOST_CHECK_THROW((void)textedit::read_lines("", 0, 0, static_cast<LineReadFormat>(99)),
                      std::invalid_argument);
    BOOST_CHECK_THROW((void)textedit::read_bytes("", 0, 0, static_cast<ByteReadFormat>(99)),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(totals_cover_the_whole_input_for_both_modes_and_empty_selections)
{
    for (const auto& [text, lines] : std::vector<std::pair<std::string, std::size_t>>{
             {"", 1}, {"abc", 1}, {"a\r\nb\n", 3},
             {"a\xc2\x85\xe2\x80\xa8z", 3}}) {
        for (const auto count : {0u, 1u}) {
            const auto line_result = textedit::read_lines(text, 0, count);
            BOOST_TEST(line_result.total_bytes == text.size());
            BOOST_TEST(line_result.total_lines == lines);
            const auto byte_result = textedit::read_bytes(text, 0, count);
            BOOST_TEST(byte_result.total_bytes == text.size());
            BOOST_TEST(byte_result.total_lines == lines);
        }
        const auto eof = textedit::read_bytes(text, text.size(), 1);
        BOOST_TEST(eof.total_bytes == text.size());
        BOOST_TEST(eof.total_lines == lines);
        const auto after_lines = textedit::read_lines(text, lines, 1);
        BOOST_TEST(after_lines.total_bytes == text.size());
        BOOST_TEST(after_lines.total_lines == lines);
    }
}

BOOST_FIXTURE_TEST_CASE(file_reading_obeys_limits_and_does_not_change_contents, Scratch)
{
    const auto path = write("a\r\nb");
    const auto before = std::filesystem::last_write_time(path);
    BOOST_TEST(textedit::read_file_lines(path, 0, all).text == "a\r\nb");
    BOOST_TEST(textedit::read_file_bytes(path, 1, 2, ByteReadFormat::HexEscaped).text ==
               "\\x0D\\x0A");
    BOOST_TEST(textedit::read_file_lines(path, 1, 1, LineReadFormat::ByteRange, 4).text ==
               "[3:4] | b");
    BOOST_CHECK_THROW((void)textedit::read_file_lines(path, 0, 1, LineReadFormat::Plain, 3),
                      std::length_error);
    BOOST_CHECK_THROW((void)textedit::read_file_bytes(path, 0, 1, ByteReadFormat::Plain, 3),
                      std::length_error);
    BOOST_CHECK_THROW((void)textedit::read_file_bytes(path, 0, 1, ByteReadFormat::Plain, all),
                      std::invalid_argument);
    BOOST_CHECK(std::filesystem::last_write_time(path) == before);
    BOOST_TEST(textedit::read_file_bytes(path, 0, all).text == "a\r\nb");
    std::filesystem::create_symlink(path, root / "link");
    BOOST_TEST(textedit::read_file_lines(root / "link", 0, 1).text == "a\r\n");
    BOOST_CHECK_THROW((void)textedit::read_file_lines(root / "missing", 0, 1), std::system_error);
    BOOST_CHECK_THROW((void)textedit::read_file_bytes(root, 0, 1), std::system_error);
    BOOST_REQUIRE(::mkfifo((root / "fifo").c_str(), 0600) == 0);
    BOOST_CHECK_THROW((void)textedit::read_file_lines(root / "fifo", 0, 1), std::system_error);
    BOOST_TEST(textedit::read_file_bytes(write(""), 0, all, ByteReadFormat::Plain, 0).text.empty());
}

BOOST_AUTO_TEST_CASE(bounded_read_agrees_with_full_rendering_when_it_fits)
{
    const std::string text = "a\r\nb\xc2\x85\xe4\xb8\xad\n";
    for (const auto format : {textedit::LineReadFormat::Plain,
                              textedit::LineReadFormat::LineIndex,
                              textedit::LineReadFormat::ByteRange}) {
        const auto full = textedit::read_lines(text, 1, all, format);
        const auto bounded = textedit::read_lines_bounded(text, 1, all, format, 1024);
        BOOST_TEST(bounded.text == full.text);
        BOOST_TEST(bounded.start_byte == full.start_byte);
        BOOST_TEST(bounded.end_byte == full.end_byte);
        BOOST_TEST(bounded.total_lines == full.total_lines);
        BOOST_TEST(bounded.lines_read == full.lines_read);
        BOOST_TEST(!bounded.output_truncated);
        BOOST_TEST(!bounded.display_replaced);
    }
    for (const auto format : {textedit::ByteReadFormat::Plain,
                              textedit::ByteReadFormat::HexEscaped}) {
        const auto full = textedit::read_bytes(text, 0, all, format);
        const auto bounded = textedit::read_bytes_bounded(text, 0, all, format, 1024);
        BOOST_TEST(bounded.text == full.text);
        BOOST_TEST(bounded.total_lines == full.total_lines);
        BOOST_TEST(!bounded.output_truncated);
    }
}

BOOST_AUTO_TEST_CASE(bounded_rendering_stops_before_materializing_large_selection)
{
    const std::string dense(16 * 1024 * 1024, '\n');
    const auto lines = textedit::read_lines_bounded(
        dense, 0, all, textedit::LineReadFormat::ByteRange, 65536);
    BOOST_TEST(lines.total_lines == dense.size() + 1);
    BOOST_TEST(lines.lines_read == dense.size() + 1);
    BOOST_TEST(lines.total_bytes == dense.size());
    BOOST_TEST(lines.reached_end);
    BOOST_TEST(lines.output_truncated);
    BOOST_TEST(lines.text.size() <= 65536u);
    BOOST_TEST(lines.text.starts_with("[       0:       1] | "));

    const auto hex = textedit::read_bytes_bounded(
        dense, 0, all, textedit::ByteReadFormat::HexEscaped, 65536);
    BOOST_TEST(hex.total_lines == dense.size() + 1);
    BOOST_TEST(hex.output_truncated);
    BOOST_TEST(hex.text.size() == 65536u);
    BOOST_TEST(hex.text.starts_with("\\x0A\\x0A"));
    BOOST_TEST(hex.text.ends_with("\\x0A"));
    const auto tiny = textedit::read_bytes_bounded(
        dense, 0, 1, textedit::ByteReadFormat::HexEscaped, 3);
    BOOST_TEST(tiny.text.empty());
    BOOST_TEST(tiny.output_truncated);
}

BOOST_AUTO_TEST_CASE(malformed_bytes_across_budget_are_repaired_before_clipping)
{
    const std::string malformed(70000, '\x80');
    const auto result = textedit::read_bytes_bounded(
        malformed, 0, all, textedit::ByteReadFormat::Plain, 65536);
    BOOST_TEST(result.text.size() == 65535u); // 21845 complete U+FFFD scalars.
    BOOST_TEST(result.text.starts_with("\xef\xbf\xbd\xef\xbf\xbd"));
    BOOST_TEST(result.display_replaced);
    BOOST_TEST(result.output_truncated);
    BOOST_TEST(result.end_byte == malformed.size());

    const auto valid = textedit::read_bytes_bounded(
        "\xe4\xb8\xad\xe4\xb8\xad", 0, all,
        textedit::ByteReadFormat::Plain, 4);
    BOOST_TEST(valid.text == "\xe4\xb8\xad");
    BOOST_TEST(valid.output_truncated);
    BOOST_TEST(!valid.display_replaced);
    const auto split = textedit::read_bytes_bounded(
        "\xe4\xb8\xad", 1, 1, textedit::ByteReadFormat::Plain, 3);
    BOOST_TEST(split.text == "\xef\xbf\xbd");
    BOOST_TEST(split.display_replaced);
    BOOST_TEST(!split.output_truncated);
}

BOOST_FIXTURE_TEST_CASE(file_bounded_read_has_same_limits_and_totals, Scratch)
{
    const auto path = write("a\r\nb\n");
    const auto lines = textedit::read_file_lines_bounded(
        path, 0, all, textedit::LineReadFormat::LineIndex, 5, 5);
    BOOST_TEST(lines.text == "0 | a");
    BOOST_TEST(lines.output_truncated);
    BOOST_TEST(lines.total_lines == 3u);
    BOOST_TEST(lines.total_bytes == 5u);
    const auto hex = textedit::read_file_bytes_bounded(
        path, 0, all, textedit::ByteReadFormat::HexEscaped, 4, 5);
    BOOST_TEST(hex.text == "\\x61");
    BOOST_TEST(hex.output_truncated);
    BOOST_CHECK_THROW((void)textedit::read_file_lines_bounded(
        path, 0, 1, textedit::LineReadFormat::Plain, 5, 4), std::length_error);
}
