// PromptTemplate end-to-end usage walkthrough.
//
// Offline and deterministic. The demo builds a small but realistic three-tier
// system prompt — immutable identity/rules, growing tool + workspace regions,
// volatile per-turn state — and exercises every operation the class offers:
//
//   * building:   add_section() under the non-decreasing stability rule
//   * growing:    append(), including the tool-embedding pattern (Invocables
//                 rendered by caller code; the template itself never sees a
//                 tool, so the embedding style stays yours)
//   * volatile:   rewrite() and erase()
//   * reading:    range-for iteration, find()/contains()/size(), the fields
//                 of a located section, and render() spans
//   * rendering:  heading_level, and the byte-prefix cache check — how much
//                 of the previous prompt survived a mutation
//
// Every misuse is caught and its std::logic_error printed, showing the
// stability enforcement in action.
//
// Build target only; not registered with CTest (a walkthrough, not an
// assertion suite — see test/test_prompt_template.cpp for the assertions).
#include "dataclass/model_io.hpp"
#include "dataclass/prompt_template.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using model_io::Invocable;
using model_io::PromptTemplate;
using model_io::RenderedPrompt;
using model_io::SectionStability;

namespace {

void rule(const std::string& title) {
    std::cout << "\n==== " << title << " ====\n";
}

std::string_view stability_name(SectionStability s) {
    switch (s) {
    case SectionStability::Immutable: return "immutable";
    case SectionStability::Growing: return "growing";
    case SectionStability::Volatile: return "volatile";
    }
    return "?";
}

// A caller-defined Invocable -> markdown renderer. This function is where
// the tool-embedding style lives; PromptTemplate only ever sees the string.
std::string tool_markdown(const Invocable& inv) {
    std::string out = "### " + inv.name + "\n\n" + inv.description;
    const auto props
        = inv.argument_schema.value("properties", nlohmann::json::object());
    if (props.empty()) return out + "\n\nNo arguments.";

    const auto required = inv.argument_schema.value(
        "required", nlohmann::json::array());
    out += "\n\nArguments:";
    for (auto it = props.begin(); it != props.end(); ++it) {
        auto mods = it.value().value("type", std::string("any"));
        for (const auto& r : required)
            if (r == it.key()) mods += ", required";
        out += "\n- `" + it.key() + "` (" + mods + ")";
    }
    return out;
}

// Longest common leading byte count of two rendered prompts — exactly what a
// provider prefix cache keeps alive.
std::size_t common_prefix(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return i;
}

template <class Fn>
void show_error(const char* what, Fn&& fn) {
    try {
        fn();
        std::cout << what << ": NO ERROR (bug!)\n";
    } catch (const std::logic_error& e) {
        std::cout << what << "\n  -> " << e.what() << "\n";
    }
}

} // namespace

int main() {
    // ---------------------------------------------------------------- 1. build
    rule("1. build: immutable -> growing -> volatile");
    PromptTemplate tpl;
    tpl.add_section("identity", "Identity",
                    "You are simplex, a C++ coding agent.\n"
                    "Answer concisely; cite code as path:line.")
        .add_section("rules", "Ground Rules",
                     "- Read the header before guessing an API.\n"
                     "- Prefer minimal diffs.\n"
                     "- Ask before anything destructive.  \n") // trailing ws
        .add_section("tools", "Tools", "", SectionStability::Growing)
        .add_section("env", "Workspace", "", SectionStability::Growing)
        .add_section("clock", "", "Current date: 2026-08-15 (UTC).",
                     SectionStability::Volatile);

    // ------------------------------------------------------- 2. tool embedding
    rule("2. growing: register tools through YOUR renderer");
    std::vector<Invocable> tools;
    Invocable search;
    search.name = "search";
    search.description = "Full-text search over the code index";
    search.argument_schema = nlohmann::json{
        {"type", "object"},
        {"properties",
         {{"q", {{"type", "string"}, {"description", "query text"}}},
          {"limit", {{"type", "integer"}, {"description", "max hits"}}}}},
        {"required", nlohmann::json::array({"q"})},
    };
    tools.push_back(std::move(search));
    Invocable clock;
    clock.name = "clock";
    clock.description = "Current UTC time";
    tools.push_back(std::move(clock));

    for (const auto& inv : tools)
        tpl.append("tools", tool_markdown(inv));
    // The workspace region grows the same way. append() joins chunks with a
    // blank line (paragraph semantics), so batch tightly-packed lines into
    // one chunk when you want a compact list.
    tpl.append("env",
               "- repo root: /home/hazer/simplex-cpp\n"
               "- branch: feat/generic-plugin-framework");

    // ------------------------------------------------------------- 3. reading
    rule("3. read: iterate / find / contains / size");
    std::cout << "sections (" << tpl.size() << "):\n";
    for (const auto& s : tpl) // range-for over PromptSection
        std::cout << "  " << s.name << " [" << stability_name(s.stability)
                  << "] title=\"" << s.title << "\"\n";

    auto it = tpl.find("identity");
    if (it != tpl.end())
        std::cout << "find(\"identity\"): " << stability_name(it->stability)
                  << " tier, canonicalised text (note: the trailing "
                     "whitespace above was stripped on entry):\n  "
                  << it->text << "\n";
    std::cout << "contains(\"tools\"): " << std::boolalpha
              << tpl.contains("tools") << ", contains(\"nope\"): "
              << tpl.contains("nope") << "\n";

    // ------------------------------------------------------------ 4. rendering
    rule("4. render: heading_level shapes every section title");
    RenderedPrompt v2 = tpl.render(); // heading_level == 2 (the default)
    std::cout << "---- rendered with heading_level = 2 "
                 "(default) ----\n" << v2.markdown << "--------\n";

    tpl.heading_level = 1; // every title now one '#'
    RenderedPrompt v1 = tpl.render();
    std::cout << "first line with heading_level = 1: \""
              << v1.markdown.substr(0, v1.markdown.find('\n')) << "\"\n";
    tpl.heading_level = 2; // restore

    rule("4b. spans: one byte range per section");
    for (const auto& s : v1.spans)
        std::cout << "  [" << s.begin << ".." << s.end << ") " << s.name
                  << " (" << stability_name(s.stability) << ")\n";

    // --------------------------------------------------- 5. the cache guarantee
    rule("5. a new turn: plugin adds a tool, date moves");
    RenderedPrompt before = tpl.render();

    Invocable grep;
    grep.name = "grep";
    grep.description = "Regex search in tracked files";
    grep.argument_schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"pattern", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"pattern"})},
    };
    tpl.append("tools", tool_markdown(grep));   // growing: append-only
    tpl.append("env", "- open file: src/main.cpp"); // growing
    tpl.rewrite("clock", "Current date: 2026-08-16 (UTC)."); // volatile

    RenderedPrompt after = tpl.render();
    const auto kept = common_prefix(before.markdown, after.markdown);
    std::cout << "prefix bytes preserved: " << kept << " / "
              << before.markdown.size() << "\n";
    std::cout << "spans that did not move:\n";
    for (std::size_t i = 0; i < before.spans.size() && i < after.spans.size();
         ++i)
        if (before.spans[i].name == after.spans[i].name
            && before.spans[i].begin == after.spans[i].begin
            && before.spans[i].end == after.spans[i].end)
            std::cout << "  " << before.spans[i].name << "\n";

    // ------------------------------------------------------------- 6. misuse
    rule("6. misuse: every wrong move throws");
    show_error("append() to an immutable section", [&] {
        tpl.append("identity", "extra line");
    });
    show_error("rewrite() on a growing section", [&] {
        tpl.rewrite("tools", "replaced");
    });
    show_error("add_section() going backwards in stability", [&] {
        tpl.add_section("late", "Late", "x", SectionStability::Immutable);
    });
    show_error("add_section() with a duplicate name", [&] {
        tpl.add_section("tools", "Tools", "");
    });
    show_error("render() with heading_level out of range", [&] {
        PromptTemplate bad;
        bad.add_section("a", "A", "body");
        bad.heading_level = 9;
        bad.render();
    });

    // -------------------------------------------------------------- 7. erase
    rule("7. volatile: erase the clock section");
    tpl.erase("clock");
    RenderedPrompt done = tpl.render();
    std::cout << "final prompt (" << done.markdown.size() << " bytes, "
              << done.spans.size() << " spans):\n----\n"
              << done.markdown << "--------\n";
    return 0;
}
