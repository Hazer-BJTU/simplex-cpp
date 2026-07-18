#define BOOST_TEST_MODULE FallbackLanguageTests
#include <boost/test/unit_test.hpp>

#include "fallbacklang.hpp"

#include <fstream>
#include <filesystem>
#include <set>

using namespace indextools;

// ============================================================================
// Test helpers
// ============================================================================

namespace {

// Analyze a source string with FallbackLanguage and expose the identifier
// index for assertions.
struct AnalysisResult {
    FallbackLanguage lang;
    const FallbackLanguage::LineIndex& identifiers;

    explicit AnalysisResult(std::string source)
        : lang()
        , identifiers((lang.load(std::move(source))->analyze(), lang.get_identifier_line_map()))
    {}
};

// Collect the line numbers for a token into a sorted set for easy comparison.
std::set<size_t> lines_for(const FallbackLanguage::LineIndex& idx, const std::string& token) {
    std::string lower(token);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = idx.find(lower);
    if (it == idx.end()) {
        return {};
    }
    return {it->second.begin(), it->second.end()};
}

} // namespace

// ============================================================================
// Tokenization
// ============================================================================

BOOST_AUTO_TEST_CASE(tokens_are_split_on_punctuation_and_brackets) {
    AnalysisResult r(
        "int main(int argc, char** argv) {\n"
        "    return argc + 1;\n"
        "}\n"
    );

    // 'main' appears only on line 0.
    BOOST_CHECK(lines_for(r.identifiers, "main") == std::set<size_t>{0});

    // 'argc' appears on both lines (parameter + use).
    BOOST_CHECK(lines_for(r.identifiers, "argc") == (std::set<size_t>{0, 1}));

    // 'return' is a keyword but still a valid token — it appears on line 1.
    BOOST_CHECK(lines_for(r.identifiers, "return") == std::set<size_t>{1});

    // 'int' appears twice on line 0 but should collapse to a single entry.
    BOOST_CHECK(lines_for(r.identifiers, "int") == std::set<size_t>{0});
}

BOOST_AUTO_TEST_CASE(dollar_and_underscore_are_identifier_chars) {
    AnalysisResult r(
        "const $foo_bar = 1;\n"
        "let baz$_qux = 2;\n"
    );

    BOOST_CHECK(lines_for(r.identifiers, "$foo_bar") == std::set<size_t>{0});
    BOOST_CHECK(lines_for(r.identifiers, "baz$_qux") == std::set<size_t>{1});
}

BOOST_AUTO_TEST_CASE(tokens_are_lowercased_for_case_insensitive_lookup) {
    AnalysisResult r(
        "MyFunc and myfunc and MYFUNC\n"
    );

    // All three spellings collapse into one lowercased key on line 0.
    BOOST_CHECK(lines_for(r.identifiers, "myfunc") == std::set<size_t>{0});
    BOOST_CHECK(r.identifiers.contains("myfunc"));
    BOOST_CHECK(!r.identifiers.contains("MyFunc"));
}

BOOST_AUTO_TEST_CASE(numeric_tokens_are_indexed) {
    // Numbers are valid identifier-character runs, so they are indexed too.
    // This is acceptable for a fallback analyzer.
    AnalysisResult r("x = 42\ny = 4242\n");
    BOOST_CHECK(lines_for(r.identifiers, "42") == std::set<size_t>{0});
    BOOST_CHECK(lines_for(r.identifiers, "4242") == std::set<size_t>{1});
}

BOOST_AUTO_TEST_CASE(empty_and_whitespace_only_source_produces_no_tokens) {
    FallbackLanguage lang;
    lang.load("   \n  \t \n");
    lang.analyze();
    BOOST_CHECK(lang.get_identifier_line_map().empty());
}

// ============================================================================
// Interface contract
// ============================================================================

BOOST_AUTO_TEST_CASE(locate_entity_is_always_empty) {
    AnalysisResult r("def foo(): pass\n");
    BOOST_CHECK(r.lang.locate_entity("foo").empty());
    BOOST_CHECK(r.lang.locate_entity("anything").is_array());
}

BOOST_AUTO_TEST_CASE(get_full_structure_is_always_empty) {
    AnalysisResult r("def foo(): pass\n");
    BOOST_CHECK(r.lang.get_full_structure().empty());
}

BOOST_AUTO_TEST_CASE(result_is_always_empty) {
    AnalysisResult r("def foo(): pass\n");
    BOOST_CHECK(r.lang.result().empty());
}

BOOST_AUTO_TEST_CASE(locate_identifier_returns_context_lines) {
    // Line layout (0-based):
    //   0: preamble
    //   1: target_line with the token
    //   2: trailing
    FallbackLanguage lang;
    lang.load("preamble line\n"
              "target_line contains thetoken here\n"
              "trailing line\n");
    lang.analyze();
    lang.set_context_lines(1);

    auto j = lang.locate_identifier("thetoken");
    BOOST_REQUIRE(!j.empty());
    BOOST_REQUIRE(j.is_array());
    BOOST_REQUIRE(j.size() == 1);

    // meta must label the match as an Identifier.
    const auto& meta = j[0]["meta"];
    BOOST_CHECK(meta["field_name"][1] == "Identifier");
    BOOST_CHECK(meta["field_content"][1] == "thetoken");

    // With context_lines = 1 around line 1, the text window covers lines
    // 0..2. The matched line (1) is tagged "match"; the others "base".
    const auto& types = j[0]["text"]["line_type"];
    const auto& numbers = j[0]["text"]["line_number"];
    BOOST_REQUIRE(numbers.size() == 3);
    BOOST_CHECK(numbers[0] == 0 && numbers[1] == 1 && numbers[2] == 2);
    BOOST_CHECK(types[1] == "match");
    BOOST_CHECK(types[0] == "base");
    BOOST_CHECK(types[2] == "base");
}

BOOST_AUTO_TEST_CASE(locate_identifier_unknown_token_returns_empty) {
    AnalysisResult r("alpha beta\n");
    BOOST_CHECK(r.lang.locate_identifier("gamma").empty());
}

BOOST_AUTO_TEST_CASE(reset_clears_state) {
    FallbackLanguage lang;
    lang.load("foo bar\n");
    lang.analyze();
    BOOST_CHECK(!lang.get_identifier_line_map().empty());

    lang.reset();
    BOOST_CHECK(lang.get_identifier_line_map().empty());
    BOOST_CHECK(lang.source().empty());

    // Reusable after reset.
    lang.load("baz\n");
    lang.analyze();
    BOOST_CHECK(lang.get_identifier_line_map().contains("baz"));
}
