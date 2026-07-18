#define BOOST_TEST_MODULE ViewerTests
#include <boost/test/unit_test.hpp>

#include "viewer.hpp"
#include "split.hpp"

#include <string>

using namespace indextools;

namespace {

// A 5-line source, lines "L0".."L4" (0-based), LF-separated, no trailing NL.
SplitedString make_source() {
    SplitedString s;
    s.load("L0\nL1\nL2\nL3\nL4");
    return s;
}

// Fetch the single block from a read_* result and sanity-check the envelope.
const nlohmann::json& only_block(const nlohmann::json& result) {
    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE_EQUAL(result.size(), 1u);
    return result.at(0);
}

// Pull a meta value by its field_name label.
nlohmann::json meta_value(const nlohmann::json& block, const std::string& name) {
    const auto& meta = block.at("meta");
    const auto& names = meta.at("field_name");
    const auto& contents = meta.at("field_content");
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return contents[i];
    }
    return nlohmann::json(nullptr);
}

} // namespace

// ============================================================================
// read_lines
// ============================================================================

BOOST_AUTO_TEST_SUITE(ReadLinesSuite)

BOOST_AUTO_TEST_CASE(reads_a_window_in_the_middle) {
    auto src = make_source();
    auto block = only_block(read_lines(src, 1, 2, "/f.txt"));

    // Body is line-indexed ("text"), not whole-content.
    BOOST_REQUIRE(block.contains("text"));
    BOOST_CHECK(!block.contains("content"));

    const auto& text = block.at("text");
    BOOST_REQUIRE_EQUAL(text.at("line_content").size(), 2u);
    BOOST_CHECK_EQUAL(text["line_content"][0], "L1");
    BOOST_CHECK_EQUAL(text["line_content"][1], "L2");
    BOOST_CHECK_EQUAL(text["line_number"][0], 1);
    BOOST_CHECK_EQUAL(text["line_number"][1], 2);
    BOOST_CHECK_EQUAL(text["line_type"][0], "base");

    BOOST_CHECK_EQUAL(meta_value(block, "File"), "/f.txt");
    BOOST_CHECK_EQUAL(meta_value(block, "Lines"), "[1, 2]");
    BOOST_CHECK_EQUAL(meta_value(block, "Total Lines"), 5);
    // Lines 3,4 remain beyond the window.
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), true);
}

BOOST_AUTO_TEST_CASE(window_reaching_end_is_not_truncated) {
    auto src = make_source();
    auto block = only_block(read_lines(src, 3, 10, "/f.txt"));

    const auto& text = block.at("text");
    BOOST_REQUIRE_EQUAL(text.at("line_content").size(), 2u);   // L3, L4
    BOOST_CHECK_EQUAL(text["line_content"][0], "L3");
    BOOST_CHECK_EQUAL(text["line_content"][1], "L4");
    BOOST_CHECK_EQUAL(meta_value(block, "Lines"), "[3, 4]");
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), false);
}

BOOST_AUTO_TEST_CASE(start_past_end_yields_empty_body) {
    auto src = make_source();
    auto block = only_block(read_lines(src, 99, 10, "/f.txt"));

    BOOST_CHECK(block.at("text").at("line_content").empty());
    BOOST_CHECK_EQUAL(meta_value(block, "Lines"), "[]");
    BOOST_CHECK_EQUAL(meta_value(block, "Total Lines"), 5);
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), false);
}

BOOST_AUTO_TEST_CASE(zero_max_lines_yields_empty_but_reports_total) {
    auto src = make_source();
    auto block = only_block(read_lines(src, 0, 0, "/f.txt"));

    BOOST_CHECK(block.at("text").at("line_content").empty());
    BOOST_CHECK_EQUAL(meta_value(block, "Lines"), "[]");
    BOOST_CHECK_EQUAL(meta_value(block, "Total Lines"), 5);
    // Everything remains unread → truncated.
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), true);
}

BOOST_AUTO_TEST_CASE(reads_whole_file) {
    auto src = make_source();
    auto block = only_block(read_lines(src, 0, 100, "/f.txt"));
    BOOST_CHECK_EQUAL(block.at("text").at("line_content").size(), 5u);
    BOOST_CHECK_EQUAL(meta_value(block, "Lines"), "[0, 4]");
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), false);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// read_bytes
// ============================================================================

BOOST_AUTO_TEST_SUITE(ReadBytesSuite)

BOOST_AUTO_TEST_CASE(reads_a_byte_window_as_one_string) {
    auto src = make_source();   // "L0\nL1\nL2\nL3\nL4" — 14 bytes
    auto block = only_block(read_bytes(src, 0, 5, "/f.txt"));

    // Body is whole-content, not line-indexed.
    BOOST_REQUIRE(block.contains("content"));
    BOOST_CHECK(!block.contains("text"));

    BOOST_CHECK_EQUAL(block.at("content"), "L0\nL1");   // bytes [0,5)
    BOOST_CHECK_EQUAL(meta_value(block, "Bytes"), "[0, 4]");
    BOOST_CHECK_EQUAL(meta_value(block, "Total Bytes"), 14);
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), true);
    BOOST_CHECK_EQUAL(meta_value(block, "Binary"), false);
}

BOOST_AUTO_TEST_CASE(window_reaching_end_is_not_truncated) {
    auto src = make_source();
    auto block = only_block(read_bytes(src, 12, 100, "/f.txt"));
    BOOST_CHECK_EQUAL(block.at("content"), "L4");
    BOOST_CHECK_EQUAL(meta_value(block, "Bytes"), "[12, 13]");
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), false);
}

BOOST_AUTO_TEST_CASE(start_past_end_yields_empty_content) {
    auto src = make_source();
    auto block = only_block(read_bytes(src, 999, 100, "/f.txt"));
    BOOST_CHECK_EQUAL(block.at("content"), "");
    BOOST_CHECK_EQUAL(meta_value(block, "Bytes"), "[]");
    BOOST_CHECK_EQUAL(meta_value(block, "Total Bytes"), 14);
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), false);
}

BOOST_AUTO_TEST_CASE(detects_nul_byte_as_binary) {
    SplitedString src;
    src.load(std::string("ab\0cd", 5));   // embedded NUL
    auto block = only_block(read_bytes(src, 0, 5, "/bin"));
    BOOST_CHECK_EQUAL(meta_value(block, "Binary"), true);
    BOOST_CHECK_EQUAL(meta_value(block, "Total Bytes"), 5);
}

BOOST_AUTO_TEST_CASE(long_single_line_via_bytes) {
    // The byte reader's raison d'être: a huge single line paginated by bytes.
    SplitedString src;
    src.load(std::string(1000, 'x'));   // one 1000-byte line, no delimiter
    auto block = only_block(read_bytes(src, 100, 200, "/min.js"));
    BOOST_CHECK_EQUAL(block.at("content").get<std::string>().size(), 200u);
    BOOST_CHECK_EQUAL(meta_value(block, "Bytes"), "[100, 299]");
    BOOST_CHECK_EQUAL(meta_value(block, "Truncated"), true);
}

BOOST_AUTO_TEST_SUITE_END()
