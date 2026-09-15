#pragma once

//
// skill_declaration.hpp — a toolset's skill, declared in YAML
// ===========================================================
//
// A toolset's skill is the prose about how its tools are used TOGETHER — the
// workflow, the principles, the mistakes worth not making — as opposed to what
// each tool does, which is a tool declaration's business
// (tools/intrinsic/tool_declaration.hpp). This header is where that document
// comes from: one file per set, named beside the set's tool declarations
// (tools/intrinsic/toolsets/process/schemas/skill.yaml), loaded once when the
// set is built.
//
// WHAT A SKILL FILE LOOKS LIKE
// ----------------------------
//
//   name: process                 # required — the skill's identity
//   title: Working with processes # optional — the prompt section's heading
//   description: >-               # optional — one line, for a catalogue
//     Run and drive child processes from the process tools.
//   keywords: [process, shell]    # optional — free-form selection hints
//   text: |                       # required — the guidance itself
//     Start with spawn_process ...
//
// `text` is markdown, carried VERBATIM into the system prompt: the section
// ToolSet::inject_skill() appends is this text, byte for byte, so reading the
// file is reading what the model was told. It is the one field here that leaves
// this process — and the one a reviewer should read whole.
//
// WHY THE OTHER FIELDS EXIST, since none of them reaches the model on its own:
// a host that lists skills (a `/skill` command, a log line, a UI) has nothing
// to show without a name, a title and a one-line description, and a host that
// SELECTS skills — this tree does not, yet — has nothing to select by without
// keywords. They are here because the file is the one place they can live:
// deriving them from the text would be guessing at prose.
//
// WHAT IS DELIBERATELY NOT LOADED. A toolset's skill cannot change what a call
// does, and nothing here pretends otherwise: this loader reads five keys and
// ignores every other key at the document's top level, the same policy the tool
// declaration loader applies (there, `type` and `security` are written for a
// human and read by nobody). A host that wants to record something else about a
// skill — an owner, a version, a date — writes it in the file and reads it
// itself; nothing here will refuse it, and nothing here will act on it.
//
// WHAT IS CHECKED, and why each one is worth refusing rather than tolerating:
//
//   name   required, non-empty: it is what skill_section_name() builds the
//          prompt section's name from, and a skill that cannot be named cannot
//          be found again in a prompt or reported in a log;
//   text   required, non-empty: the whole point of the document. An empty one
//          would inject a heading with nothing under it and call that
//          guidance;
//   title,
//   description
//          optional, but a key that IS there must carry a non-empty string —
//          a field that says nothing is a field whose author meant something;
//   keywords
//          optional, a sequence of non-empty strings, no duplicates: a word
//          listed twice selects exactly what it selected once.
//
// FAILURE
// -------
// Two entry points, one policy each, the same pair a tool declaration offers:
//
//   load_skill_declaration()      throws SkillDeclarationError — for a caller
//                                 that cannot continue without the skill;
//   try_load_skill_declaration()  reports the same failure through the log and
//                                 answers nullopt — for a toolset builder,
//                                 whose contract is to run WITHOUT guidance
//                                 rather than to fail the host.
//
// Nothing here parses YAML itself: yamlconfig/yaml_json.hpp is the project's
// one YAML boundary, and this header adds only what a *skill document* has to
// satisfy on top of it.
//

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include "tools/tool_skill.hpp"

namespace tools::intrinsic {

/**
 * A skill document that cannot be used: an unreadable file, a YAML syntax
 * error, or a document missing (or malformed in) one of the two things a skill
 * cannot be injected without.
 *
 * Its own type rather than yamlconfig::YamlConfigError, for the reason
 * ToolDeclarationError states: the message of the one exception a caller here
 * has to catch should not depend on which YAML library sits behind the loader.
 * It carries the file (and, for a shape problem, the in-document path) so a
 * packaging mistake names the file it is in.
 */
class SkillDeclarationError : public std::runtime_error {
public:
    explicit SkillDeclarationError(const std::string& what_arg)
        : std::runtime_error(what_arg) {}
};

/**
 * Load and validate one toolset's skill.
 *
 * @param file the skill file. Nothing about the path is interpreted here — a
 *        package decides where its files live and passes the whole path (the
 *        process toolset's schemas.hpp is that decision, made once, and its
 *        skill sits beside its tool declarations).
 * @return the skill, with every field trimmed of surrounding whitespace.
 * @throws SkillDeclarationError if the file cannot be read, is not YAML, is not
 *         a mapping, or does not carry a non-empty `name` and a non-empty
 *         `text`, or carries a `title`/`description` that is not a non-empty
 *         string or a `keywords` entry that is not a non-empty, unique string.
 *         Keys other than those five are not read at all (see the file header).
 */
[[nodiscard]] tools::ToolSetSkill load_skill_declaration(
    const std::filesystem::path& file);

/**
 * load_skill_declaration(), with the failure REPORTED instead of thrown: the
 * reason goes to the log at error level and the answer is nullopt.
 *
 * This is the form a toolset uses (IntrinsicToolSet::load_skill). A skill that
 * cannot be loaded is a packaging mistake that must be loud, but it is not a
 * reason to take the host down, and it is not a reason to withhold the tools
 * either: the set keeps every tool it registered and simply has no guidance to
 * inject, which is a state a model works in — it is told what each tool does,
 * and it can still call all of them.
 */
[[nodiscard]] std::optional<tools::ToolSetSkill> try_load_skill_declaration(
    const std::filesystem::path& file);

} // namespace tools::intrinsic
