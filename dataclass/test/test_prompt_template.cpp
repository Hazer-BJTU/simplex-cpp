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

// ---- JSON contract -------------------------------------------------------------

BOOST_AUTO_TEST_CASE(template_round_trips_through_json) {
    PromptTemplate t = three_tier_template();
    t.heading_level = 3;
    const std::string markdown_before = t.render().markdown;

    PromptTemplate copy = nlohmann::json(t).get<PromptTemplate>();

    BOOST_CHECK_EQUAL(copy.heading_level, 3);
    BOOST_REQUIRE_EQUAL(copy.size(), t.size());
    // Section-by-section identity, in stored order.
    for (auto a = t.begin(), b = copy.begin(); a != t.end(); ++a, ++b) {
        BOOST_CHECK_EQUAL(a->name, b->name);
        BOOST_CHECK_EQUAL(a->title, b->title);
        BOOST_CHECK(a->stability == b->stability);
        BOOST_CHECK_EQUAL(a->text, b->text);
    }
    // The structure is the durable artefact: the restored template renders
    // the identical bytes again.
    BOOST_CHECK_EQUAL(copy.render().markdown, markdown_before);
}

BOOST_AUTO_TEST_CASE(stored_layout_reenters_through_the_admission_rules) {
    // A stability regression in stored sections is rejected, not smuggled
    // in — from_json rebuilds through add_section().
    nlohmann::json backwards = nlohmann::json::parse(R"json({
      "heading_level": 2,
      "sections": [
        {"name": "a", "title": "", "stability": "volatile", "text": "x"},
        {"name": "b", "title": "", "stability": "immutable", "text": "y"}
      ]
    })json");
    PromptTemplate untouched;
    untouched.add_section("kept", "Kept", "original");
    BOOST_CHECK_THROW(backwards.get_to(untouched), std::logic_error);
    // Strong guarantee: the rejected document left the destination alone.
    BOOST_REQUIRE_EQUAL(untouched.size(), 1u);
    BOOST_CHECK_EQUAL(untouched.begin()->name, "kept");

    // Duplicate section names are rejected the same way.
    nlohmann::json duplicate = nlohmann::json::parse(R"json({
      "sections": [
        {"name": "a", "title": "", "stability": "immutable", "text": "x"},
        {"name": "a", "title": "", "stability": "immutable", "text": "y"}
      ]
    })json");
    BOOST_CHECK_THROW(duplicate.get<PromptTemplate>(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(from_json_keeps_defaults_and_canonicalises_bodies) {
    // No heading_level key: member default (2) survives.
    nlohmann::json j = nlohmann::json::parse(R"json({
      "sections": [
        {"name": "s", "title": "S", "stability": "growing",
         "text": "line  \n\nnext\n\n"}
      ]
    })json");
    PromptTemplate t = j.get<PromptTemplate>();

    BOOST_CHECK_EQUAL(t.heading_level, 2);
    BOOST_REQUIRE_EQUAL(t.size(), 1u);
    // Hand-edited bodies enter through the same canonicalisation as live
    // ones: per-line trailing whitespace stripped, edge blank lines dropped,
    // interior blank lines kept.
    BOOST_CHECK_EQUAL(t.begin()->text, "line\n\nnext");
    BOOST_CHECK(t.begin()->stability == SectionStability::Growing);

    // An unrecognised tier string fails closed to Immutable.
    nlohmann::json odd_tier = nlohmann::json::parse(R"json({
      "sections": [{"name": "s", "stability": "sometimes", "text": "x"}]
    })json");
    BOOST_CHECK(odd_tier.get<PromptTemplate>().begin()->stability
                == SectionStability::Immutable);
}

BOOST_AUTO_TEST_CASE(empty_template_round_trips_to_empty_sections) {
    PromptTemplate t;
    nlohmann::json j = t;
    BOOST_CHECK_EQUAL(j["heading_level"], 2);
    BOOST_CHECK(j["sections"].is_array());
    BOOST_CHECK(j["sections"].empty());
    BOOST_CHECK(j.get<PromptTemplate>().size() == 0u);
}
