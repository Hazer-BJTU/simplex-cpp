#pragma once

//
// tool_skill.hpp — what a toolset tells a model about USING it
// ============================================================
//
// A tool's declaration answers "what does this call do": a name, a description
// and the schema of its arguments. What no per-tool document can answer is the
// question a model has when the tools are a family: which one comes first, what
// the ordinary path through them looks like, what the wrong path costs, and
// which of two overlapping calls is the one to prefer. Six process tools are
// the worked example: `spawn_process` says what it runs, and nothing in it says
// that polling a live child in a loop is the mistake and waiting on it once is
// the answer.
//
// That prose is a toolset's SKILL, and this header is its type. A skill is:
//
//   name         what the skill is called — its identity in a catalogue, and
//                the name its section takes in a prompt (skill_section_name());
//   title        the heading it renders under, when it has one;
//   description  one line saying what the skill is for, for a host that lists
//                skills without showing their text;
//   keywords     the words a host may select a skill by — never sent to a
//                model on their own, and deliberately free-form: this tree has
//                no vocabulary of its own here, because nothing validates
//                against them;
//   text         the guidance itself, as markdown.
//
// WHERE ONE COMES FROM, AND HOW IT REACHES A MODEL. A toolset ships its skill
// as a YAML document beside its tool declarations (the process set's is
// tools/intrinsic/toolsets/process/schemas/skill.yaml), and the loader that
// reads it is tools::intrinsic::load_skill_declaration() (tools/intrinsic/
// skill_declaration.hpp). An intrinsic set takes one on with
// IntrinsicToolSet::load_skill() and answers it from ToolSet::skill().
//
// Getting it in front of the model is ToolSet::inject_skill(): the skill
// becomes ONE section of the host's system prompt, appended at the end, so a
// set's guidance sits where a reader would look for it — after the identity and
// the persona it belongs to, before whatever the host rewrites per turn.
// ToolRegistry::inject_skills() is the same call over every registered set, in
// registration order, for the host that wants the whole catalogue's guidance in
// one line.
//
// A SKILL IS DATA, AND TYPED LIKE DATA. Nothing here runs, and nothing here
// changes a tool's behaviour: a model that ignores its skill can still call
// every tool, which is why the tool declarations remain the contract and this
// is advice. Nothing validates a call against it either — that is
// ensure_arguments()' business — so the only thing a skill can be is wrong
// about the tools it describes, and a test is what keeps it honest
// (toolsets/process/test/test_tools.cpp loads the set's skill and holds it
// against the tools the set actually registered).
//
// A SET MAY CARRY NONE. ToolSet::skill() answers nullopt by default, and
// inject_skill() then contributes nothing — a set with nothing to say about
// using its tools together is an ordinary set, not a broken one.
//

#include <string>
#include <string_view>
#include <vector>

namespace tools {

/**
 * One toolset's guidance for the model: the prose that says how the set's tools
 * fit together, and the metadata a host needs to file it.
 *
 * See the file header for where one is written, who loads it, and how it
 * reaches a system prompt. Plain data: no methods, no invariants beyond the
 * loader's (a skill whose name or text is empty is one nothing can file or
 * show, and load_tool_skill() refuses it).
 */
struct ToolSetSkill {
    /// The skill's name — its identity, and what skill_section_name() builds
    /// the prompt section's name from. The convention is the toolset's own
    /// name (the process set's skill is "process"), so that a reader can tell
    /// which set a section came from without reading the text.
    std::string name;

    /// The heading the skill renders under in the prompt, or empty for no
    /// heading (a skill whose text opens with its own heading, say).
    std::string title;

    /// One line saying what the skill is for — for a catalogue, a log line or
    /// a `/help`, never a substitute for `text`.
    std::string description;

    /// The words a host may select this skill by. Free-form on purpose: no
    /// part of this tree matches against them, so a vocabulary here would be a
    /// rule nothing enforces.
    std::vector<std::string> keywords;

    /// The guidance itself, as markdown. This is the part a model reads.
    std::string text;
};

/**
 * The name a skill's section takes in a prompt: "skill." + the skill's name.
 *
 * Stated once, here, because three parties have to agree on it: the injection
 * that adds the section (ToolSet::inject_skill), a host that replaces or
 * inspects it afterwards, and PromptTemplate's own duplicate rule — a second
 * section of the same name is refused (std::logic_error), and that refusal is
 * the answer to "was this already injected?".
 */
[[nodiscard]] inline std::string skill_section_name(std::string_view skill_name)
{
    return "skill." + std::string(skill_name);
}

} // namespace tools
