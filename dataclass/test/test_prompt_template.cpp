// Tests for PromptTemplate: markdown rendering, stability enforcement, and
// the byte-prefix guarantees the class exists to provide. Pure text checks:
// no network, no filesystem.
#define BOOST_TEST_MODULE prompt_template
#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "dataclass/model_io.hpp"
#include "dataclass/prompt_template.hpp"

using namespace model_io;

// Span of a named section, from a RenderedPrompt.
static const SectionSpan& span_of(const RenderedPrompt& r,
                                  std::string_view name) {
    for (const auto& s : r.spans)
        if (s.name == name) return s;
    BOOST_FAIL("no span named '" + std::string(name) + "'");
    return r.spans.front(); // unreachable
}

// A three-tier template: immutable identity, growing tools, volatile date.
static PromptTemplate three_tier_template() {
    PromptTemplate t;
    t.add_section("identity", "Identity", "You are a coding agent.");
    t.add_section("tools", "Tools", "### search", SectionStability::Growing);
    t.add_section("now", "", "Date: 2026-08-14", SectionStability::Volatile);
    return t;
}

// ---- rendering ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(empty_template_renders_empty_string) {
    PromptTemplate t;
    auto r = t.render();
    BOOST_CHECK_EQUAL(r.markdown, "");
    BOOST_CHECK(r.spans.empty());
}

BOOST_AUTO_TEST_CASE(section_renders_heading_body_and_trailing_newline) {
    PromptTemplate t;
    t.add_section("id", "Identity", "You are a coding agent.");
    BOOST_CHECK_EQUAL(t.render().markdown,
                      "## Identity\n\nYou are a coding agent.\n");
}

BOOST_AUTO_TEST_CASE(untitled_section_renders_body_only) {
    PromptTemplate t;
    t.add_section("id", "", "You are a coding agent.");
    BOOST_CHECK_EQUAL(t.render().markdown, "You are a coding agent.\n");
}

BOOST_AUTO_TEST_CASE(sections_are_joined_by_a_blank_line) {
    PromptTemplate t;
    t.add_section("a", "A", "first");
    t.add_section("b", "B", "second");
    BOOST_CHECK_EQUAL(t.render().markdown, "## A\n\nfirst\n\n## B\n\nsecond\n");
}

BOOST_AUTO_TEST_CASE(render_is_deterministic_and_leaves_state_untouched) {
    PromptTemplate t = three_tier_template();
    auto r1 = t.render();
    auto r2 = t.render();
    BOOST_CHECK_EQUAL(r1.markdown, r2.markdown);
    BOOST_CHECK_EQUAL(r1.spans.size(), r2.spans.size());
    BOOST_CHECK_EQUAL(t.size(), 3u);
}

BOOST_AUTO_TEST_CASE(heading_level_is_validated_at_render) {
    PromptTemplate t;
    t.add_section("id", "Identity", "body");
    t.heading_level = 0;
    BOOST_CHECK_THROW(t.render(), std::logic_error);
    t.heading_level = 7;
    BOOST_CHECK_THROW(t.render(), std::logic_error);
    t.heading_level = 1;
    BOOST_CHECK_EQUAL(t.render().markdown, "# Identity\n\nbody\n");
}

// ---- canonicalisation ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(text_is_canonicalised_on_entry) {
    PromptTemplate t;
    // Trailing spaces/tabs per line and leading/trailing blank lines go
    // away; interior blank lines stay.
    t.add_section("id", "T", "\n\nline one \t\n\nline two  \n\n");
    BOOST_CHECK_EQUAL(t.render().markdown, "## T\n\nline one\n\nline two\n");
}

BOOST_AUTO_TEST_CASE(append_extends_a_growing_section_with_a_blank_line) {
    PromptTemplate t;
    t.add_section("tools", "Tools", "", SectionStability::Growing);
    t.append("tools", "### search");
    t.append("tools", "### ls");
    BOOST_CHECK_EQUAL(t.render().markdown,
                      "## Tools\n\n### search\n\n### ls\n");
}

// ---- stability enforcement ------------------------------------------------------

BOOST_AUTO_TEST_CASE(append_rejects_non_growing_and_unknown_sections) {
    PromptTemplate t = three_tier_template();
    BOOST_CHECK_THROW(t.append("identity", "more"), std::logic_error);
    BOOST_CHECK_THROW(t.append("now", "more"), std::logic_error);
    BOOST_CHECK_THROW(t.append("nope", "more"), std::logic_error);
}

BOOST_AUTO_TEST_CASE(rewrite_and_erase_reject_non_volatile_and_unknown) {
    PromptTemplate t = three_tier_template();
    BOOST_CHECK_THROW(t.rewrite("identity", "x"), std::logic_error);
    BOOST_CHECK_THROW(t.rewrite("tools", "x"), std::logic_error);
    BOOST_CHECK_THROW(t.rewrite("nope", "x"), std::logic_error);
    BOOST_CHECK_THROW(t.erase("identity"), std::logic_error);
    BOOST_CHECK_THROW(t.erase("tools"), std::logic_error);
    BOOST_CHECK_THROW(t.erase("nope"), std::logic_error);

    t.rewrite("now", "Date: 2026-08-15");
    t.erase("now");
    BOOST_CHECK_EQUAL(t.size(), 2u);
    BOOST_CHECK(!t.contains("now"));
}

BOOST_AUTO_TEST_CASE(add_section_rejects_duplicates_and_backwards_stability) {
    PromptTemplate t;
    t.add_section("a", "A", "", SectionStability::Growing);
    BOOST_CHECK_THROW(t.add_section("a", "A2", ""), std::logic_error);
    BOOST_CHECK_THROW(t.add_section("imm", "I", "", SectionStability::Immutable),
                      std::logic_error);

    PromptTemplate t2;
    t2.add_section("v", "V", "", SectionStability::Volatile);
    BOOST_CHECK_THROW(
        t2.add_section("g", "G", "", SectionStability::Growing),
        std::logic_error);

    // Non-decreasing rank is fine: several volatile sections at the tail.
    PromptTemplate t3;
    t3.add_section("i", "I", "one", SectionStability::Immutable);
    t3.add_section("g", "G", "two", SectionStability::Growing);
    t3.add_section("v1", "", "three", SectionStability::Volatile);
    t3.add_section("v2", "", "four", SectionStability::Volatile);
    BOOST_CHECK_EQUAL(t3.render().markdown,
                      "## I\n\none\n\n## G\n\ntwo\n\nthree\n\nfour\n");
}

// ---- the cache guarantees --------------------------------------------------------

BOOST_AUTO_TEST_CASE(prefix_bytes_survive_a_growing_append) {
    PromptTemplate t = three_tier_template();
    auto before = t.render();
    t.append("tools", "### ls");
    auto after = t.render();

    // Bytes before the growing section are untouched...
    const auto tools_begin = span_of(before, "tools").begin;
    BOOST_CHECK_EQUAL(after.markdown.compare(0, tools_begin, before.markdown,
                                             0, tools_begin),
                      0);
    // ...and earlier spans do not move.
    for (const auto& name : {"identity"}) {
        const auto &b = span_of(before, name), &a = span_of(after, name);
        BOOST_CHECK_EQUAL(a.begin, b.begin);
        BOOST_CHECK_EQUAL(a.end, b.end);
    }
    // The appended text lands at the end of the tools section.
    BOOST_CHECK_NE(after.markdown.find("### ls"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(prefix_bytes_survive_a_volatile_rewrite) {
    PromptTemplate t = three_tier_template();
    auto before = t.render();
    t.rewrite("now", "Date: 2026-08-15");
    auto after = t.render();

    const auto now_begin = span_of(before, "now").begin;
    BOOST_CHECK_EQUAL(after.markdown.compare(0, now_begin, before.markdown,
                                             0, now_begin),
                      0);
    for (const auto& name : {"identity", "tools"}) {
        const auto &b = span_of(before, name), &a = span_of(after, name);
        BOOST_CHECK_EQUAL(a.begin, b.begin);
        BOOST_CHECK_EQUAL(a.end, b.end);
    }
    // The old volatile content is gone; the new one is in.
    BOOST_CHECK_EQUAL(after.markdown.find("2026-08-14"), std::string::npos);
    BOOST_CHECK_NE(after.markdown.find("2026-08-15"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(spans_cover_markdown_in_order) {
    PromptTemplate t = three_tier_template();
    auto r = t.render();
    BOOST_REQUIRE_EQUAL(r.spans.size(), 3u);

    std::size_t at = 0;
    for (const auto& s : r.spans) {
        BOOST_CHECK_EQUAL(s.begin, at);
        BOOST_CHECK_LT(s.begin, s.end);
        at = s.end + 2; // the blank-line separator
    }
    // Last span ends one newline short of the whole prompt.
    BOOST_CHECK_EQUAL(r.markdown.size(), r.spans.back().end + 1);
}

// ---- iteration -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(sections_iterate_in_order_and_find_locates_by_name) {
    PromptTemplate t = three_tier_template();

    std::vector<std::string> names;
    for (const auto& s : t) names.push_back(s.name);
    BOOST_CHECK(names == (std::vector<std::string>{"identity", "tools", "now"}));

    auto it = t.find("tools");
    BOOST_REQUIRE(it != t.end());
    BOOST_CHECK(it->stability == SectionStability::Growing);
    BOOST_CHECK(t.contains("tools"));
    BOOST_CHECK(!t.contains("nope"));
    BOOST_CHECK(t.find("nope") == t.end());
}

// ---- the tool-embedding pattern ---------------------------------------------------

// PromptTemplate never sees Invocable: the caller renders tools its own way
// and appends the text. This is that usage.
BOOST_AUTO_TEST_CASE(invocables_embed_through_a_custom_renderer) {
    std::vector<Invocable> tools;
    Invocable search;
    search.name = "search";
    search.description = "Full-text search over the index";
    search.argument_schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"q", {{"type", "string"}}}}},
    };
    tools.push_back(std::move(search));

    Invocable time;
    time.name = "time";
    time.description = "Current UTC time";
    tools.push_back(std::move(time));

    auto tool_markdown = [](const Invocable& inv) {
        return "### " + inv.name + "\n\n" + inv.description;
    };

    PromptTemplate t;
    t.add_section("identity", "Identity", "You are a coding agent.");
    t.add_section("tools", "Tools", "", SectionStability::Growing);
    for (const auto& inv : tools)
        t.append("tools", tool_markdown(inv));

    auto r = t.render();
    const auto search_at = r.markdown.find("### search");
    const auto time_at = r.markdown.find("### time");
    BOOST_CHECK_NE(search_at, std::string::npos);
    BOOST_CHECK_NE(time_at, std::string::npos);
    BOOST_CHECK_LT(search_at, time_at); // append order is render order
    BOOST_CHECK_NE(r.markdown.find("Full-text search over the index"),
                   std::string::npos);
}
