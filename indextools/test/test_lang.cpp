#define BOOST_TEST_MODULE LangAnalyzeTests
#include <boost/test/unit_test.hpp>

#include "indextools/lang.hpp"

#include <fstream>
#include <filesystem>
#include <set>

using namespace indextools;

// ============================================================================
// Mock implementation of LangAnalyze for testing the abstract interface
// ============================================================================

namespace {

class MockLangAnalyze final : public indextools::LangAnalyze {
public:
    bool analyze_called = false;
    bool reset_called = false;

    LangAnalyze* analyze() noexcept override {
        analyze_called = true;
        return this;
    }

    const EntityList& result() const noexcept override {
        static const EntityList empty;
        return empty;
    }

    nlohmann::json locate_identifier(std::string_view) const noexcept override {
        return nlohmann::json::array();
    }

    nlohmann::json locate_entity(std::string_view entity_key) const noexcept override {
        return {};
    }

    nlohmann::json get_full_structure() const noexcept override {
        return nlohmann::json::array();
    }

    LangAnalyze* reset() noexcept override {
        LangAnalyze::reset();
        reset_called = true;
        return this;
    }
};

// RAII temporary file helper
struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const std::string& content) {
        path = std::filesystem::temp_directory_path()
               / "cpptools_test_lang.txt";
        std::ofstream ofs(path);
        ofs << content;
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // Non-copyable, movable
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

} // anonymous namespace

// ============================================================================
// Suite: LangAnalyzeLoadSuite — load() variants
// ============================================================================

BOOST_AUTO_TEST_SUITE(LangAnalyzeLoadSuite)

BOOST_AUTO_TEST_CASE(load_string_populates_source)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("hello world"));
    BOOST_CHECK_EQUAL(analyzer.source(), "hello world");
}

BOOST_AUTO_TEST_CASE(load_replaces_previous_source)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("first"));
    BOOST_CHECK_EQUAL(analyzer.source(), "first");
    analyzer.load(std::string("second"));
    BOOST_CHECK_EQUAL(analyzer.source(), "second");
}

BOOST_AUTO_TEST_CASE(load_empty_string)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string(""));
    BOOST_CHECK(analyzer.source().empty());
    // Empty source: get_dedented_lines returns empty
    BOOST_CHECK(analyzer.get_dedented_lines(0, 0).empty());
}

BOOST_AUTO_TEST_CASE(load_creates_correct_lines)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb\nc"));
    auto lines = analyzer.get_dedented_lines(0, 2);
    BOOST_REQUIRE_EQUAL(lines.size(), 3u);
    BOOST_CHECK_EQUAL(lines[0], "a");
    BOOST_CHECK_EQUAL(lines[1], "b");
    BOOST_CHECK_EQUAL(lines[2], "c");
}

BOOST_AUTO_TEST_CASE(load_handles_crlf_line_endings)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\r\nb\r\nc"));
    auto lines = analyzer.get_dedented_lines(0, 2);
    BOOST_REQUIRE_EQUAL(lines.size(), 3u);
    BOOST_CHECK_EQUAL(lines[0], "a");
    BOOST_CHECK_EQUAL(lines[1], "b");
    BOOST_CHECK_EQUAL(lines[2], "c");
}

BOOST_AUTO_TEST_CASE(load_handles_trailing_newline)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb\n"));
    auto lines = analyzer.get_dedented_lines(0, 1);
    BOOST_REQUIRE_EQUAL(lines.size(), 2u);
    BOOST_CHECK_EQUAL(lines[0], "a");
    BOOST_CHECK_EQUAL(lines[1], "b");
}

BOOST_AUTO_TEST_CASE(load_handles_only_newlines)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("\n\n\n"));
    auto lines = analyzer.get_dedented_lines(0, 2);
    BOOST_REQUIRE_EQUAL(lines.size(), 3u);
    BOOST_CHECK(lines[0].empty());
    BOOST_CHECK(lines[1].empty());
    BOOST_CHECK(lines[2].empty());
}

BOOST_AUTO_TEST_CASE(load_multiline_with_empty_lines)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\n\nb\n\nc\n"));
    auto lines = analyzer.get_dedented_lines(0, 4);
    BOOST_REQUIRE_EQUAL(lines.size(), 5u);
    BOOST_CHECK_EQUAL(lines[0], "a");
    BOOST_CHECK(lines[1].empty());
    BOOST_CHECK_EQUAL(lines[2], "b");
    BOOST_CHECK(lines[3].empty());
    BOOST_CHECK_EQUAL(lines[4], "c");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: LangAnalyzeOpenSuite — open() file loading
// ============================================================================

BOOST_AUTO_TEST_SUITE(LangAnalyzeOpenSuite)

BOOST_AUTO_TEST_CASE(open_loads_file_and_populates_source)
{
    TempFile tmp("hello from file");
    MockLangAnalyze analyzer;
    analyzer.open(tmp.path);
    BOOST_CHECK_EQUAL(analyzer.source(), "hello from file");
}

BOOST_AUTO_TEST_CASE(open_empty_file)
{
    TempFile tmp("");
    MockLangAnalyze analyzer;
    analyzer.open(tmp.path);
    BOOST_CHECK(analyzer.source().empty());
    // Empty source: get_dedented_lines returns empty
    BOOST_CHECK(analyzer.get_dedented_lines(0, 0).empty());
}

BOOST_AUTO_TEST_CASE(open_nonexistent_file_throws)
{
    MockLangAnalyze analyzer;
    BOOST_CHECK_THROW(
        analyzer.open(std::filesystem::path("/nonexistent/path/that/does/not/exist.txt")),
        std::runtime_error
    );
}

BOOST_AUTO_TEST_CASE(open_multiline_file_creates_lines)
{
    TempFile tmp("line1\nline2\nline3\n");
    MockLangAnalyze analyzer;
    analyzer.open(tmp.path);
    auto lines = analyzer.get_dedented_lines(0, 2);
    BOOST_REQUIRE_EQUAL(lines.size(), 3u);
    BOOST_CHECK_EQUAL(lines[0], "line1");
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: LangAnalyzeResetSuite — reset() state clearing
// ============================================================================

BOOST_AUTO_TEST_SUITE(LangAnalyzeResetSuite)

BOOST_AUTO_TEST_CASE(reset_clears_all_state)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("some content"));
    analyzer.analyze();
    analyzer.reset();

    BOOST_CHECK(analyzer.source().empty());
    BOOST_CHECK(analyzer.get_dedented_lines(0, 0).empty());
    BOOST_CHECK(analyzer.reset_called);
}

BOOST_AUTO_TEST_CASE(reset_double_reset_is_safe)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("content"));
    analyzer.reset();
    BOOST_CHECK_NO_THROW(analyzer.reset());
}

BOOST_AUTO_TEST_CASE(reset_then_reuse)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("first"));
    analyzer.reset();
    analyzer.load(std::string("second"));
    BOOST_CHECK_EQUAL(analyzer.source(), "second");
    auto lines = analyzer.get_dedented_lines(0, 0);
    BOOST_REQUIRE(!lines.empty());
    BOOST_CHECK_EQUAL(lines[0], "second");
}

BOOST_AUTO_TEST_CASE(reset_without_load_is_safe)
{
    MockLangAnalyze analyzer;
    BOOST_CHECK_NO_THROW(analyzer.reset());
    BOOST_CHECK(analyzer.source().empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: LangAnalyzePatternSuite — locate_pattern()
// ============================================================================

BOOST_AUTO_TEST_SUITE(LangAnalyzePatternSuite)

BOOST_AUTO_TEST_CASE(json_pattern_basic_single_match)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("line0\nline1\nline2\nline3\nline4"));
    nlohmann::json result = analyzer.locate_pattern("line2");
    BOOST_REQUIRE(result.is_array());
    BOOST_REQUIRE(!result.empty());

    const auto& entry = result[0];
    BOOST_CHECK(entry.contains("meta"));
    BOOST_CHECK(entry.contains("text"));

    // Meta: File, Pattern, Matches, Total Length
    const auto& meta = entry["meta"];
    BOOST_CHECK_EQUAL(meta["field_name"].size(), 4u);
    BOOST_CHECK_EQUAL(meta["field_content"].size(), 4u);
    BOOST_CHECK_EQUAL(meta["field_name"][0], "File");
    BOOST_CHECK_EQUAL(meta["field_name"][1], "Pattern");
    BOOST_CHECK_EQUAL(meta["field_content"][1], "line2");

    // Text: line 2 is the match, with context_lines=2 → lines 0-4
    const auto& text = entry["text"];
    BOOST_CHECK(text["line_content"].is_array());
    BOOST_CHECK(!text["line_content"].empty());
    // line 2 should be marked "match"
    bool found_match = false;
    for (size_t i = 0; i < text["line_number"].size(); ++i) {
        if (text["line_number"][i] == 2u) {
            BOOST_CHECK_EQUAL(text["line_type"][i], "match");
            found_match = true;
        }
    }
    BOOST_CHECK(found_match);
}

BOOST_AUTO_TEST_CASE(json_pattern_multiple_matches)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("ab\ncd\nab\nef\nab"));
    nlohmann::json result = analyzer.locate_pattern("ab");
    BOOST_REQUIRE(!result.empty());
    const auto& entry = result[0];
    // Match count should be 3 (index 2 of 4-element array: [File, Pattern, Matches, Total Length])
    BOOST_CHECK_EQUAL(entry["meta"]["field_content"][2].get<size_t>(), 3u);
}

BOOST_AUTO_TEST_CASE(json_pattern_no_match_returns_empty_array)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("abc\ndef"));
    nlohmann::json result = analyzer.locate_pattern("xyz");
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(json_pattern_empty_source_returns_empty_array)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string(""));
    nlohmann::json result = analyzer.locate_pattern("a");
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(json_pattern_empty_pattern_returns_empty_array)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("abc"));
    nlohmann::json result = analyzer.locate_pattern("");
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(json_pattern_context_lines_included)
{
    MockLangAnalyze analyzer;
    // Match on line 1, default context_lines=2 → lines 0-3 (but only 3 lines exist)
    analyzer.load(std::string("line0\nMATCH\nline2"));
    nlohmann::json result = analyzer.locate_pattern("MATCH");
    BOOST_REQUIRE(!result.empty());
    const auto& text = result[0]["text"];
    // Should have all 3 lines (context clamped at boundaries)
    BOOST_CHECK_EQUAL(text["line_content"].size(), 3u);
    // line 1 is the match
    BOOST_CHECK_EQUAL(text["line_type"][1], "match");
    BOOST_CHECK_EQUAL(text["line_type"][0], "base");
    if (text["line_content"].size() > 2) {
        BOOST_CHECK_EQUAL(text["line_type"][2], "base");
    }
}

BOOST_AUTO_TEST_CASE(json_pattern_context_clamped_at_start)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("MATCH\na\nb\nc\nd"));
    nlohmann::json result = analyzer.locate_pattern("MATCH");
    BOOST_REQUIRE(!result.empty());
    const auto& lines = result[0]["text"]["line_number"];
    // Should not go below line 0
    BOOST_CHECK(lines[0].get<size_t>() == 0u);
}

BOOST_AUTO_TEST_CASE(json_pattern_context_clamped_at_end)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb\nc\nMATCH"));
    nlohmann::json result = analyzer.locate_pattern("MATCH");
    BOOST_REQUIRE(!result.empty());
    const auto& lines = result[0]["text"]["line_number"];
    // Last line should be within source (index 3)
    size_t last = lines[lines.size() - 1].get<size_t>();
    BOOST_CHECK(last <= 3u);
}

BOOST_AUTO_TEST_CASE(json_pattern_overlapping_intervals_merged)
{
    // Two matches on adjacent lines, context intervals overlap
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb\nMATCH1\nMATCH2\nc\nd"));
    nlohmann::json result = analyzer.locate_pattern("MATCH");
    BOOST_REQUIRE(!result.empty());
    const auto& lines = result[0]["text"]["line_number"];
    // Verify no duplicate line numbers (merged intervals)
    std::set<size_t> seen;
    for (const auto& ln : lines) {
        size_t n = ln.get<size_t>();
        BOOST_CHECK(seen.find(n) == seen.end());
        seen.insert(n);
    }
}

BOOST_AUTO_TEST_CASE(json_pattern_lines_in_ascending_order)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb\nMATCH\nc\nMATCH\nd"));
    nlohmann::json result = analyzer.locate_pattern("MATCH");
    BOOST_REQUIRE(!result.empty());
    const auto& lines = result[0]["text"]["line_number"];
    for (size_t i = 1; i < lines.size(); ++i) {
        BOOST_CHECK(lines[i].get<size_t>() > lines[i - 1].get<size_t>());
    }
}

BOOST_AUTO_TEST_CASE(json_pattern_line_type_match_vs_base)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("line0\nline1\nMATCH_LINE\nline3\nline4"));
    nlohmann::json result = analyzer.locate_pattern("MATCH_LINE");
    BOOST_REQUIRE(!result.empty());
    const auto& text = result[0]["text"];
    for (size_t i = 0; i < text["line_number"].size(); ++i) {
        size_t line_num = text["line_number"][i];
        std::string line_type = text["line_type"][i];
        if (line_num == 2u) {
            BOOST_CHECK_EQUAL(line_type, "match");
        } else {
            BOOST_CHECK_EQUAL(line_type, "base");
        }
    }
}

BOOST_AUTO_TEST_CASE(json_pattern_cross_line_match)
{
    // Pattern that spans across a line break
    MockLangAnalyze analyzer;
    analyzer.load(std::string("abc\ndef\nghi"));
    nlohmann::json result = analyzer.locate_pattern("c\nd");
    BOOST_REQUIRE(!result.empty());
    const auto& text = result[0]["text"];
    // Both line 0 and line 1 should be marked "match"
    bool line0_match = false, line1_match = false;
    for (size_t i = 0; i < text["line_number"].size(); ++i) {
        if (text["line_number"][i] == 0u && text["line_type"][i] == "match") line0_match = true;
        if (text["line_number"][i] == 1u && text["line_type"][i] == "match") line1_match = true;
    }
    BOOST_CHECK(line0_match);
    BOOST_CHECK(line1_match);
}

BOOST_AUTO_TEST_CASE(json_pattern_custom_context_via_setter)
{
    MockLangAnalyze analyzer;
    analyzer.set_context_lines(1);
    analyzer.load(std::string("line0\nline1\nMATCH\nline3\nline4"));
    nlohmann::json result = analyzer.locate_pattern("MATCH");
    BOOST_REQUIRE(!result.empty());
    const auto& text = result[0]["text"];
    // With context 1: lines 1, 2, 3 only (3 lines)
    BOOST_CHECK_EQUAL(text["line_content"].size(), 3u);
    // Meta: File, Pattern, Matches, Total Length
    BOOST_CHECK_EQUAL(result[0]["meta"]["field_name"].size(), 4u);
}

BOOST_AUTO_TEST_CASE(json_pattern_meta_file_path_from_open)
{
    TempFile tmp("hello world\nfoo bar");
    MockLangAnalyze analyzer;
    analyzer.open(tmp.path);
    nlohmann::json result = analyzer.locate_pattern("foo");
    BOOST_REQUIRE(!result.empty());
    BOOST_CHECK_EQUAL(result[0]["meta"]["field_content"][0], tmp.path.string());
}

BOOST_AUTO_TEST_CASE(json_pattern_non_overlapping_character_matches)
{
    // "aa" in "aaaa" should give 2 non-overlapping matches both on line 0,
    // merged into a single match line.
    MockLangAnalyze analyzer;
    analyzer.load(std::string("aaaa"));
    nlohmann::json result = analyzer.locate_pattern("aa");
    BOOST_REQUIRE(!result.empty());
    const auto& text = result[0]["text"];
    // Both matches are on line 0 → one "match" entry
    size_t match_count = 0;
    for (const auto& lt : text["line_type"]) {
        if (lt == "match") match_count++;
    }
    BOOST_CHECK(match_count >= 1u);
}

BOOST_AUTO_TEST_CASE(json_pattern_multiline_source_correct_lines)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("import os\nimport sys\n\ndef foo():\n    pass\n"));
    nlohmann::json result = analyzer.locate_pattern("import");
    BOOST_REQUIRE(!result.empty());
    const auto& text = result[0]["text"];
    // Lines 0 and 1 are matches (both have "import")
    bool line0 = false, line1 = false;
    for (size_t i = 0; i < text["line_number"].size(); ++i) {
        if (text["line_number"][i] == 0u && text["line_type"][i] == "match") line0 = true;
        if (text["line_number"][i] == 1u && text["line_type"][i] == "match") line1 = true;
    }
    BOOST_CHECK(line0);
    BOOST_CHECK(line1);
}

BOOST_AUTO_TEST_CASE(json_pattern_meta_includes_total_length)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb\nc\nd\ne"));
    nlohmann::json result = analyzer.locate_pattern("c");
    BOOST_REQUIRE(!result.empty());
    const auto& meta = result[0]["meta"];
    // field_name: [File, Pattern, Matches, Total Length]
    BOOST_CHECK_EQUAL(meta["field_name"].size(), 4u);
    BOOST_CHECK_EQUAL(meta["field_content"].size(), 4u);
    BOOST_CHECK_EQUAL(meta["field_name"][3], "Total Length");
    // Total length should be 5
    BOOST_CHECK_EQUAL(meta["field_content"][3].get<size_t>(), 5u);
}

BOOST_AUTO_TEST_CASE(json_pattern_total_length_empty_source)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string(""));
    nlohmann::json result = analyzer.locate_pattern("x");
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

// --- locate_pattern(use_regex) — regex pattern search ---

BOOST_AUTO_TEST_CASE(json_pattern_regex_matches_across_lines)
{
    MockLangAnalyze analyzer;
    // Two int declarations, one float declaration.
    analyzer.load(std::string("int x = 1;\nint y = 2;\nfloat z = 3;"));
    nlohmann::json result = analyzer.locate_pattern("int\\s+\\w+", /*use_regex=*/true);
    BOOST_REQUIRE(!result.empty());
    const auto& entry = result[0];
    // Matches count: the two "int <name>" lines.
    BOOST_CHECK_EQUAL(entry["meta"]["field_content"][2].get<size_t>(), 2u);

    // Lines 0 and 1 should be marked "match"; line 2 should not appear as match.
    const auto& text = entry["text"];
    bool line0_match = false, line1_match = false, line2_match = false;
    for (size_t i = 0; i < text["line_number"].size(); ++i) {
        size_t ln = text["line_number"][i].get<size_t>();
        if (ln == 0u && text["line_type"][i] == "match") line0_match = true;
        if (ln == 1u && text["line_type"][i] == "match") line1_match = true;
        if (ln == 2u && text["line_type"][i] == "match") line2_match = true;
    }
    BOOST_CHECK(line0_match);
    BOOST_CHECK(line1_match);
    BOOST_CHECK(!line2_match);
}

BOOST_AUTO_TEST_CASE(json_pattern_regex_invalid_returns_empty)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("some source"));
    // Invalid regex must yield an empty array, not terminate.
    nlohmann::json result = analyzer.locate_pattern("(unclosed", /*use_regex=*/true);
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(json_pattern_regex_non_overlapping)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("aaaa\nbbbb"));
    // "aa" non-overlapping matches twice on the first line.
    nlohmann::json result = analyzer.locate_pattern("aa", /*use_regex=*/true);
    BOOST_REQUIRE(!result.empty());
    BOOST_CHECK_EQUAL(result[0]["meta"]["field_content"][2].get<size_t>(), 2u);
}

BOOST_AUTO_TEST_CASE(json_pattern_regex_default_is_plain)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a.b\na.b"));
    // Plain mode: the literal "." is searched for; with context_lines=2 the
    // whole source is covered and both dots' lines are matches.
    nlohmann::json plain = analyzer.locate_pattern(".", /*use_regex=*/false);
    nlohmann::json regex = analyzer.locate_pattern(".", /*use_regex=*/true);
    BOOST_REQUIRE(!plain.empty());
    BOOST_REQUIRE(!regex.empty());
    // Plain "." matches the two literal dots (2); regex "." matches every
    // character, so many more matches.
    BOOST_CHECK_EQUAL(plain[0]["meta"]["field_content"][2].get<size_t>(), 2u);
    BOOST_CHECK_GT(regex[0]["meta"]["field_content"][2].get<size_t>(),
                   plain[0]["meta"]["field_content"][2].get<size_t>());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: LangAnalyzeDedentSuite — get_dedented_lines()
// ============================================================================

BOOST_AUTO_TEST_SUITE(LangAnalyzeDedentSuite)

BOOST_AUTO_TEST_CASE(dedented_lines_removes_common_indent)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("    a\n    b"));
    auto result = analyzer.get_dedented_lines(0, 1);
    BOOST_REQUIRE_EQUAL(result.size(), 2u);
    BOOST_CHECK_EQUAL(result[0], "a");
    BOOST_CHECK_EQUAL(result[1], "b");
}

BOOST_AUTO_TEST_CASE(dedented_lines_mixed_indent_uses_minimum)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("    a\n  b"));
    auto result = analyzer.get_dedented_lines(0, 1);
    BOOST_REQUIRE_EQUAL(result.size(), 2u);
    // Minimum indent is 2, so "    a" → "  a"
    BOOST_CHECK_EQUAL(result[0], "  a");
    BOOST_CHECK_EQUAL(result[1], "b");
}

BOOST_AUTO_TEST_CASE(dedented_lines_preserves_empty_lines)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("    a\n\n    b"));
    auto result = analyzer.get_dedented_lines(0, 2);
    BOOST_REQUIRE_EQUAL(result.size(), 3u);
    BOOST_CHECK_EQUAL(result[0], "a");
    BOOST_CHECK(result[1].empty());
    BOOST_CHECK_EQUAL(result[2], "b");
}

BOOST_AUTO_TEST_CASE(dedented_lines_all_blank_lines)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("  \n  \n"));
    auto result = analyzer.get_dedented_lines(0, 1);
    BOOST_REQUIRE_EQUAL(result.size(), 2u);
}

BOOST_AUTO_TEST_CASE(dedented_lines_empty_range)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb"));
    auto result = analyzer.get_dedented_lines(1, 0);
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(dedented_lines_start_beyond_lines)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb"));
    auto result = analyzer.get_dedented_lines(999, 999);
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(dedented_lines_no_indent)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("a\nb\nc"));
    auto result = analyzer.get_dedented_lines(0, 2);
    BOOST_REQUIRE_EQUAL(result.size(), 3u);
    BOOST_CHECK_EQUAL(result[0], "a");
    BOOST_CHECK_EQUAL(result[1], "b");
    BOOST_CHECK_EQUAL(result[2], "c");
}

BOOST_AUTO_TEST_CASE(dedented_lines_with_tab_indent)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("\ta\n\tb"));
    auto result = analyzer.get_dedented_lines(0, 1);
    BOOST_REQUIRE_EQUAL(result.size(), 2u);
    BOOST_CHECK_EQUAL(result[0], "a");
    BOOST_CHECK_EQUAL(result[1], "b");
}

// Regression test for Issue 1: OOB vector access when line_start > 0.
// Tests that get_dedented_lines works correctly for non-zero start lines
// (e.g., extracting content for nested entities like class methods).
BOOST_AUTO_TEST_CASE(dedented_lines_nested_offset_no_crash)
{
    MockLangAnalyze analyzer;
    // Simulate a Python class with an indented method:
    // line 0: "class Foo:"
    // line 1: "    def method(self):"
    // line 2: "        pass"
    analyzer.load(std::string("class Foo:\n    def method(self):\n        pass"));
    // Extract from line 1 (the method signature) to line 2 (start of body)
    auto result = analyzer.get_dedented_lines(1, 1);
    BOOST_REQUIRE_EQUAL(result.size(), 1u);
    BOOST_CHECK_EQUAL(result[0], "def method(self):");
}

BOOST_AUTO_TEST_CASE(dedented_lines_nested_deeply_offset)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string(
        "# line 0\n"
        "# line 1\n"
        "# line 2\n"
        "# line 3\n"
        "    def foo():\n"   // line 4
        "        pass\n"     // line 5
    ));
    // Extract from line 4 with line_start > 0
    auto result = analyzer.get_dedented_lines(4, 4);
    BOOST_REQUIRE_EQUAL(result.size(), 1u);
    BOOST_CHECK_EQUAL(result[0], "def foo():");
}

BOOST_AUTO_TEST_CASE(dedented_lines_partial_range)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("    a\n    b\n    c\n    d"));
    auto result = analyzer.get_dedented_lines(1, 2);
    BOOST_REQUIRE_EQUAL(result.size(), 2u);
    BOOST_CHECK_EQUAL(result[0], "b");
    BOOST_CHECK_EQUAL(result[1], "c");
}

BOOST_AUTO_TEST_CASE(dedented_lines_clamped_to_source_end)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("    a\n    b"));
    // line_end beyond actual lines — should be clamped
    auto result = analyzer.get_dedented_lines(0, 998);
    BOOST_REQUIRE_EQUAL(result.size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: LangAnalyzeVirtualDispatchSuite — verify virtual dispatch
// ============================================================================

BOOST_AUTO_TEST_SUITE(LangAnalyzeVirtualDispatchSuite)

BOOST_AUTO_TEST_CASE(analyze_virtual_dispatches_to_mock)
{
    MockLangAnalyze analyzer;
    analyzer.analyze();
    BOOST_CHECK(analyzer.analyze_called);
}

BOOST_AUTO_TEST_CASE(reset_chain_calls_base_through_mock)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("content"));
    analyzer.reset();
    BOOST_CHECK(analyzer.reset_called);
    BOOST_CHECK(analyzer.source().empty());
}

BOOST_AUTO_TEST_CASE(source_returns_loaded_content)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("test"));
    BOOST_CHECK_EQUAL(analyzer.source(), "test");
}

BOOST_AUTO_TEST_CASE(lines_reflects_loaded_content)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("one\ntwo"));
    auto lines = analyzer.get_dedented_lines(0, 1);
    BOOST_REQUIRE_EQUAL(lines.size(), 2u);
    BOOST_CHECK_EQUAL(lines[0], "one");
    BOOST_CHECK_EQUAL(lines[1], "two");
}

BOOST_AUTO_TEST_CASE(get_full_structure_returns_empty_array_for_mock)
{
    MockLangAnalyze analyzer;
    analyzer.load(std::string("test"));
    nlohmann::json result = analyzer.get_full_structure();
    BOOST_CHECK(result.is_array());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_SUITE_END()
