#define BOOST_TEST_MODULE TextRead
#include <boost/test/unit_test.hpp>

#include "textedit/read.hpp"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
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
