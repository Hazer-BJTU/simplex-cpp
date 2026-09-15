#define BOOST_TEST_MODULE SkillDeclarationTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/skill_declaration.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

// Tests for the skill loader in tools/intrinsic/skill_declaration.hpp: the YAML
// document that states a toolset's guidance for a model — what the tools are
// for when used TOGETHER, which is the one thing a per-tool declaration cannot
// say — and what the loader refuses.
//
// Two halves, and the first is the one with teeth: `text` is carried VERBATIM
// into the system prompt (ToolSet::inject_skill appends it as a section), so a
// loader that quietly reflowed, re-wrapped or trimmed the middle of it would
// change what a model reads while every check in this tree stayed green. The
// second half is the refusals, and they are all about a document that would
// inject SOMETHING rather than nothing: a skill with no name cannot be filed or
// found again, one with no text renders as a heading over an empty section, and
// metadata that is present but empty is a field whose author meant something.
//
// No toolset is involved here: the files are written by the test, which is the
// only way to reach the shapes a real skill file should never have. What a real
// one does once loaded — and whether it is still true about the tools its set
// registered — is the process suite's business
// (tools/intrinsic/toolsets/process/test/test_tools.cpp).

namespace fs = std::filesystem;
using tools::intrinsic::SkillDeclarationError;
using tools::intrinsic::load_skill_declaration;
using tools::intrinsic::try_load_skill_declaration;

namespace {

/// A scratch directory for one test process, with a writer that puts a document
/// where the loader will find it.
struct Scratch {
    fs::path directory;

    Scratch()
        : directory(fs::temp_directory_path()
                    / ("simplex_skill_declaration_" + std::to_string(::getpid())))
    {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        fs::create_directories(directory);
    }

    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator = (const Scratch&) = delete;

    /// Write `text` to `name` under the scratch directory and answer its path.
    [[nodiscard]] fs::path write(std::string_view name, std::string_view text)
    {
        const fs::path file = directory / std::string(name);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        BOOST_REQUIRE_MESSAGE(out.good(), "could not write " << file);
        return file;
    }
};

/// The message of the SkillDeclarationError `body` throws.
///
/// Most of what this loader owes a caller is WHICH file and WHAT is wrong with
/// it, so the cases below assert the path in the message rather than only the
/// exception's type — an error that does not say where it came from turns a
/// packaging mistake into a hunt.
template <class Body>
std::string refusal(Body&& body)
{
    try {
        body();
    } catch (const SkillDeclarationError& failure) {
        return failure.what();
    }
    BOOST_FAIL("expected a SkillDeclarationError");
    return {};
}

/// A skill that is wrong in one way: `body` is the whole document.
[[nodiscard]] std::string refusal_of(Scratch& scratch, std::string_view body)
{
    const fs::path file = scratch.write("broken.yaml", body);
    return refusal([&] { (void)load_skill_declaration(file); });
}

[[nodiscard]] bool mentions(const std::string& message, std::string_view needle)
{
    return message.find(needle) != std::string::npos;
}

/// The smallest document that loads: what every refusal case below is a
/// mutation of.
constexpr std::string_view kMinimal = R"(
name: probe
text: Call probe first, then probe again.
)";

} // namespace

// ---- what a good file gives the caller ---------------------------------------

BOOST_AUTO_TEST_CASE(a_complete_skill_loads)
{
    Scratch scratch;
    // Everything a real skill uses: comments, a folded one-liner, a keyword
    // list, a literal block for the guidance — which is markdown, and therefore
    // full of newlines that must survive — and a key the loader does not know.
    const fs::path file = scratch.write("skill.yaml", R"(
# The skill of a toolset that does not exist.
name: probe
title: Working with probes
description: >-
  How the probe tools fit together: one probe at a time, and how to read what
  it left behind.
keywords: [probe, probing, diagnostics]
owner: nobody-in-particular
text: |
  Start with `probe_once`, which both runs and reports.

  ## Principles

  - Prefer one `probe_once` to a loop of `probe_status`.
  - A probe that is still running keeps its slot until it is released.
)");

    const tools::ToolSetSkill skill = load_skill_declaration(file);

    BOOST_TEST(skill.name == "probe");
    BOOST_TEST(skill.title == "Working with probes");
    // The folded scalar arrives as one line, with the trailing newline gone.
    BOOST_TEST(skill.description
               == "How the probe tools fit together: one probe at a time, and "
                  "how to read what it left behind.");
    BOOST_TEST(skill.keywords.size() == 3);
    BOOST_TEST(skill.keywords[0] == "probe");
    BOOST_TEST(skill.keywords[1] == "probing");
    BOOST_TEST(skill.keywords[2] == "diagnostics");
    // The whole point of the document: the guidance arrives byte for byte,
    // apart from the blank line the literal block ends with. Interior blank
    // lines, indentation and bullet markers are the author's, not the
    // loader's — a model reads this text, and `owner` is none of its business.
    BOOST_TEST(skill.text
               == "Start with `probe_once`, which both runs and reports.\n"
                  "\n"
                  "## Principles\n"
                  "\n"
                  "- Prefer one `probe_once` to a loop of `probe_status`.\n"
                  "- A probe that is still running keeps its slot until it is "
                  "released.");
}

BOOST_AUTO_TEST_CASE(a_skill_may_state_only_the_two_required_fields)
{
    Scratch scratch;
    // A literal block written with `|` keeps its final newline, so the text is
    // trimmed on the way in — otherwise every injection would end in a blank
    // line nobody wrote.
    const fs::path file = scratch.write("minimal.yaml", kMinimal);

    const tools::ToolSetSkill skill = load_skill_declaration(file);

    BOOST_TEST(skill.name == "probe");
    BOOST_TEST(skill.text == "Call probe first, then probe again.");
    // Absent, not empty-and-present: a host can tell "no title" from
    // "a title that says nothing", because the loader refuses the second.
    BOOST_TEST(skill.title.empty());
    BOOST_TEST(skill.description.empty());
    BOOST_TEST(skill.keywords.empty());
}

// ---- what it refuses ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(the_two_required_fields_are_required)
{
    Scratch scratch;

    BOOST_TEST(mentions(refusal_of(scratch, R"(
text: guidance with nothing to file it under
)"), "name"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
)"), "text"));
    // Present but empty is the same failure as absent, and says so by name.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: ""
text: guidance
)"), "name"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
text: "   "
)"), "text"));
    // A name that is not a string is not a name.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: [probe]
text: guidance
)"), "name"));
}

BOOST_AUTO_TEST_CASE(optional_fields_that_are_present_must_say_something)
{
    Scratch scratch;

    // An empty title injects a heading with nothing in it; an empty description
    // is a catalogue line that answers nothing. Both are omitted, not blanked.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
title: ""
text: guidance
)"), "title"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: "  "
text: guidance
)"), "description"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
description: [a, b]
text: guidance
)"), "description"));
}

BOOST_AUTO_TEST_CASE(a_keyword_list_is_checked_element_by_element)
{
    Scratch scratch;

    // Not a list at all: the single word a reader would accept is exactly what
    // a host iterating keywords cannot use.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
keywords: probe
text: guidance
)"), "keywords"));
    // A non-string entry names its own index, so the fix is a one-line edit.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
keywords: [probe, 7]
text: guidance
)"), "/keywords/1"));
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
keywords: [probe, ""]
text: guidance
)"), "/keywords/1"));
    // A word listed twice selects exactly what it selected once.
    BOOST_TEST(mentions(refusal_of(scratch, R"(
name: probe
keywords: [probe, probe]
text: guidance
)"), "listed twice"));
}

BOOST_AUTO_TEST_CASE(a_document_that_is_not_a_mapping_is_refused)
{
    Scratch scratch;

    // Including the empty file: a skill that says nothing is not an empty
    // skill, it is one that cannot be injected.
    BOOST_TEST(mentions(refusal_of(scratch, ""), "mapping"));
    BOOST_TEST(mentions(refusal_of(scratch, "- probe\n- probe again\n"),
                        "mapping"));
}

BOOST_AUTO_TEST_CASE(an_unreadable_file_is_refused_and_names_itself)
{
    Scratch scratch;
    const fs::path missing = scratch.directory / "no-such-skill.yaml";

    const std::string message =
        refusal([&] { (void)load_skill_declaration(missing); });
    BOOST_TEST(mentions(message, "no-such-skill.yaml"));

    // A directory where a file was expected reaches the same single exception
    // type, so a caller has one thing to catch.
    const fs::path directory = scratch.directory / "a-directory.yaml";
    std::error_code ignored;
    fs::create_directories(directory, ignored);
    BOOST_TEST(mentions(refusal([&] { (void)load_skill_declaration(directory); }),
                        "a-directory.yaml"));
}

// ---- the form a toolset uses --------------------------------------------------

BOOST_AUTO_TEST_CASE(the_reporting_form_answers_nullopt_instead_of_throwing)
{
    Scratch scratch;

    // What IntrinsicToolSet::load_skill() calls: a broken file is reported and
    // the answer is "no skill" — the set keeps every tool it registered.
    BOOST_TEST(!try_load_skill_declaration(scratch.write("broken.yaml", R"(
name: probe
)"))
                    .has_value());

    const std::optional<tools::ToolSetSkill> loaded =
        try_load_skill_declaration(scratch.write("good.yaml", kMinimal));
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->name == "probe");
    BOOST_TEST(loaded->text == "Call probe first, then probe again.");
}
