#define BOOST_TEST_MODULE EditorTests
#include <boost/test/unit_test.hpp>

#include "editor.hpp"

#include <string>

using namespace indextools;

namespace {

/// Concatenate all line-content entries of a check_difference text block that
/// carry the given type ("add" / "delete" / "base").
std::string collect_typed(const nlohmann::json& block, const std::string& type) {
    std::string out;
    const auto& types = block["text"]["line_type"];
    const auto& content = block["text"]["line_content"];
    for (size_t i = 0; i < types.size(); ++i) {
        if (types[i] == type) {
            out += content[i].get<std::string>();
            out += "|";
        }
    }
    return out;
}

} // anonymous namespace

// ============================================================================
// str_replace_edit
// ============================================================================

BOOST_AUTO_TEST_SUITE(StrReplaceEditSuite)

BOOST_AUTO_TEST_CASE(replace_single_occurrence)
{
    auto s = SplitedString{}.load("aaa\nbbb\nccc\n");
    BOOST_CHECK_EQUAL(str_replace_edit(s, "bbb", "XYZ"), "aaa\nXYZ\nccc\n");
}

BOOST_AUTO_TEST_CASE(replace_all_occurrences)
{
    auto s = SplitedString{}.load("x=1\ny=1\nz=1\n");
    BOOST_CHECK_EQUAL(str_replace_edit(s, "=1", "=2", true), "x=2\ny=2\nz=2\n");
}

BOOST_AUTO_TEST_CASE(missing_pattern_throws)
{
    auto s = SplitedString{}.load("aaa\nbbb\n");
    BOOST_CHECK_THROW(str_replace_edit(s, "nope", "q"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ambiguous_pattern_without_replace_all_throws)
{
    auto s = SplitedString{}.load("a\na\na\n");
    BOOST_CHECK_THROW(str_replace_edit(s, "a", "b", false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(delete_by_replacing_with_empty)
{
    auto s = SplitedString{}.load("keep123drop");
    BOOST_CHECK_EQUAL(str_replace_edit(s, "123", ""), "keepdrop");
}

BOOST_AUTO_TEST_CASE(multiline_pattern_spanning_delimiter)
{
    // The pattern search runs over the raw source, delimiters included.
    auto s = SplitedString{}.load("first\nsecond\nthird");
    BOOST_CHECK_EQUAL(str_replace_edit(s, "first\nsecond", "merged"),
                      "merged\nthird");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// line_replace_edit
// ============================================================================

BOOST_AUTO_TEST_SUITE(LineReplaceEditSuite)

BOOST_AUTO_TEST_CASE(replace_middle_line)
{
    auto s = SplitedString{}.load("l0\nl1\nl2");
    BOOST_CHECK_EQUAL(line_replace_edit(s, 1, 1, "NEW"), "l0\nNEW\nl2");
}

BOOST_AUTO_TEST_CASE(replace_range_of_lines)
{
    auto s = SplitedString{}.load("l0\nl1\nl2\nl3");
    BOOST_CHECK_EQUAL(line_replace_edit(s, 1, 2, "X"), "l0\nX\nl3");
}

BOOST_AUTO_TEST_CASE(insert_mode_inserts_after_line)
{
    auto s = SplitedString{}.load("l0\nl1");
    BOOST_CHECK_EQUAL(line_replace_edit(s, 0, 0, "INS", true), "l0\nINS\nl1");
}

BOOST_AUTO_TEST_CASE(replacement_normalises_crlf_to_lf)
{
    auto s = SplitedString{}.load("a\nb");
    BOOST_CHECK_EQUAL(line_replace_edit(s, 0, 0, "x\r\ny"), "x\ny\nb");
}

BOOST_AUTO_TEST_CASE(out_of_range_replace_throws)
{
    auto s = SplitedString{}.load("only");
    BOOST_CHECK_THROW(line_replace_edit(s, 5, 5, "x"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(inverted_range_throws)
{
    auto s = SplitedString{}.load("a\nb\nc");
    BOOST_CHECK_THROW(line_replace_edit(s, 2, 1, "x"), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// check_difference
// ============================================================================

BOOST_AUTO_TEST_SUITE(CheckDifferenceSuite)

BOOST_AUTO_TEST_CASE(returns_two_blocks_deleted_then_added)
{
    auto a = SplitedString{}.load("x");
    auto b = SplitedString{}.load("y");
    auto j = check_difference(a, b, "f.txt", 0);
    BOOST_REQUIRE_EQUAL(j.size(), 2u);
    BOOST_CHECK_EQUAL(j[0]["meta"]["field_name"][1], "Lines deleted");
    BOOST_CHECK_EQUAL(j[1]["meta"]["field_name"][1], "Lines added");
    BOOST_CHECK_EQUAL(j[0]["meta"]["field_content"][0], "f.txt");
}

BOOST_AUTO_TEST_CASE(identical_sources_report_no_edits)
{
    auto a = SplitedString{}.load("l1\nl2\nl3");
    auto b = SplitedString{}.load("l1\nl2\nl3");
    auto j = check_difference(a, b, "f", 1);
    BOOST_CHECK_EQUAL(j[0]["meta"]["field_content"][1].get<size_t>(), 0u);
    BOOST_CHECK_EQUAL(j[1]["meta"]["field_content"][1].get<size_t>(), 0u);
}

BOOST_AUTO_TEST_CASE(counts_reflect_deleted_and_added_lines)
{
    auto a = SplitedString{}.load("keep\nold");
    auto b = SplitedString{}.load("keep\nnew1\nnew2");
    auto j = check_difference(a, b, "f", 0);
    // "old" deleted; "new1","new2" added.
    BOOST_CHECK_EQUAL(j[0]["meta"]["field_content"][1].get<size_t>(), 1u);
    BOOST_CHECK_EQUAL(j[1]["meta"]["field_content"][1].get<size_t>(), 2u);
    BOOST_CHECK_EQUAL(collect_typed(j[0], "delete"), "old|");
    BOOST_CHECK_EQUAL(collect_typed(j[1], "add"), "new1|new2|");
}

BOOST_AUTO_TEST_CASE(context_lines_include_surrounding_base_lines)
{
    auto a = SplitedString{}.load("l0\nl1\nl2\nl3\nl4");
    auto b = SplitedString{}.load("l0\nl1\nCHANGED\nl3\nl4");
    // With 1 context line, the deleted block should show l1 (base), l2 (delete), l3 (base).
    auto j = check_difference(a, b, "f", 1);
    const auto& nums = j[0]["text"]["line_number"];
    BOOST_REQUIRE_EQUAL(nums.size(), 3u);
    BOOST_CHECK_EQUAL(nums[0].get<size_t>(), 1u);
    BOOST_CHECK_EQUAL(nums[2].get<size_t>(), 3u);
    BOOST_CHECK_EQUAL(collect_typed(j[0], "delete"), "l2|");
}

BOOST_AUTO_TEST_CASE(empty_source_all_lines_added)
{
    SplitedString empty;  // no load(): size() == 0
    auto b = SplitedString{}.load("new1\nnew2");
    auto j = check_difference(empty, b, "f", 1);
    BOOST_CHECK_EQUAL(j[0]["meta"]["field_content"][1].get<size_t>(), 0u);
    BOOST_CHECK_EQUAL(j[1]["meta"]["field_content"][1].get<size_t>(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()
