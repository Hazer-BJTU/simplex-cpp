#define BOOST_TEST_MODULE LineIndex
#include <boost/test/unit_test.hpp>

#include "textedit/line_index.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/// Check both conversion directions for every byte and the unique EOF position.
/// Expected widths are specified by the test, independently of the scanner.
void check_layout(const std::string& text, const std::vector<std::size_t>& widths)
{
    const textedit::LineIndex index(text);
    BOOST_REQUIRE_EQUAL(index.line_count(), widths.size());
    BOOST_TEST(index.byte_count() == text.size());
    std::size_t offset = 0;
    for (std::size_t line = 0; line < widths.size(); ++line) {
        BOOST_TEST(index.line_start(line) == offset);
        BOOST_TEST(index.line_byte_count(line) == widths[line]);
        for (std::size_t column = 0; column < widths[line]; ++column) {
            const auto position = index.position_at(offset);
            BOOST_TEST(position.line == line);
            BOOST_TEST(position.column == column);
            BOOST_TEST(index.offset_at({line, column}) == offset);
            ++offset;
        }
        if (line + 1 < widths.size()) {
            BOOST_CHECK_THROW((void)index.offset_at({line, widths[line]}), std::out_of_range);
        }
    }
    BOOST_REQUIRE_EQUAL(offset, text.size());
    const auto eof = index.position_at(offset);
    BOOST_TEST(eof.line == widths.size() - 1);
    BOOST_TEST(eof.column == widths.back());
    BOOST_TEST(index.offset_at(eof) == offset);
}

} // namespace

BOOST_AUTO_TEST_CASE(empty_and_unterminated_text_have_a_final_line)
{
    check_layout("", {0});
    check_layout("abc", {3});
    check_layout("a\nb", {2, 1});
    check_layout("\n", {1, 0});
    check_layout("\n\n", {1, 1, 0});
}

BOOST_AUTO_TEST_CASE(all_terminator_bytes_belong_to_the_preceding_line)
{
    for (const std::string terminator : {
             "\n", "\r\n", "\r", "\v", "\f", "\xc2\x85", "\xe2\x80\xa8", "\xe2\x80\xa9"}) {
        check_layout("ab" + terminator + "cd", {2 + terminator.size(), 2});
        check_layout("ab" + terminator, {2 + terminator.size(), 0});
        check_layout(terminator + terminator, {terminator.size(), terminator.size(), 0});
    }
    check_layout("a\r\nb\rc\nd\v\f\xc2\x85\xe2\x80\xa8\xe2\x80\xa9",
                 {3, 2, 2, 2, 1, 2, 3, 3, 0});
    check_layout("\r\r\n\n\r", {1, 2, 1, 1, 0});
}

BOOST_AUTO_TEST_CASE(columns_count_bytes_including_multibyte_and_invalid_text)
{
    check_layout("\xef\xbb\xbf\xe4\xb8\xad\xf0\x9f\x98\x80\r\nX", {12, 1});
    check_layout(std::string("a\0b\xff\x85\n", 6), {6, 0});
    check_layout("\xc2", {1});
    check_layout("\xe2\x80", {2});
    check_layout("\xe2\x80\xa7", {3});
}

BOOST_AUTO_TEST_CASE(out_of_range_coordinates_never_alias_another_line)
{
    const textedit::LineIndex index("a\r\nb");
    const auto maximum = std::numeric_limits<std::size_t>::max();
    BOOST_CHECK_THROW((void)index.position_at(5), std::out_of_range);
    BOOST_CHECK_THROW((void)index.position_at(maximum), std::out_of_range);
    BOOST_CHECK_THROW((void)index.line_start(2), std::out_of_range);
    BOOST_CHECK_THROW((void)index.line_byte_count(maximum), std::out_of_range);
    BOOST_CHECK_THROW((void)index.offset_at({0, 3}), std::out_of_range);
    BOOST_CHECK_THROW((void)index.offset_at({1, 2}), std::out_of_range);
    BOOST_CHECK_THROW((void)index.offset_at({maximum, 0}), std::out_of_range);
    BOOST_CHECK_THROW((void)index.offset_at({0, maximum}), std::out_of_range);
    const textedit::LineIndex empty("");
    BOOST_CHECK_THROW((void)empty.position_at(1), std::out_of_range);
    BOOST_CHECK_THROW((void)empty.offset_at({0, 1}), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(index_owns_layout_without_retaining_or_copying_source_text)
{
    std::string text = "a\nb";
    const textedit::LineIndex index(text);
    text.assign(10000, 'x');
    BOOST_TEST(index.line_count() == 2u);
    BOOST_TEST(index.byte_count() == 3u);
    BOOST_TEST(index.offset_at({1, 0}) == 2u);
    const textedit::LineIndex temporary(std::string("a\r\nb"));
    BOOST_TEST(temporary.line_byte_count(0) == 3u);
}

BOOST_AUTO_TEST_CASE(long_lines_and_many_lines_round_trip)
{
    check_layout(std::string(100000, 'x') + "\n", {100001, 0});
    std::string text;
    std::vector<std::size_t> widths;
    for (std::size_t line = 0; line < 10000; ++line) {
        text += "x\r\n";
        widths.push_back(3);
    }
    widths.push_back(0);
    check_layout(text, widths);
}
