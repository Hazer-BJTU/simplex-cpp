#pragma once

//
// tool_declaration.hpp — a tool's Invocable, declared in YAML
// ===========================================================
//
// A tool's model-facing surface is three things: its name, its description and
// the JSON Schema of its arguments. Written as C++ they are a screenful of
// escaped string literals and nested braces inside a constructor — the part of
// a tool that is least interesting to read and most expensive to get wrong, and
// the part a reviewer most wants to see whole. This header is the alternative:
// each of those three comes from a YAML file next to the toolset's sources,
// loaded once when the tool is built.
//
// WHAT A DECLARATION FILE LOOKS LIKE
// ----------------------------------
//
//   name: spawn_process
//   description: >-
//     Run a program. Waits a short while for it to finish, ...
//   type: serial_write          # documentation only — see below
//   security: require_confirm   # documentation only — see below
//   argument_schema:
//     type: object
//     required: [executable]
//     properties:
//       executable:
//         type: string
//         description: >-
//           Program to run: a name resolved through PATH ('grep') or a path
//           ('/usr/bin/grep')
//
// `argument_schema` is a JSON Schema and is carried VERBATIM: the converted
// subtree is what goes on the wire (llm/compat/chat_completions/src/interpreter.cpp
// puts it in the request's `parameters`), and what a host hands a model. That
// is the reason the file is YAML rather than a C++ builder chain — this is a
// document, and it reads like one.
//
// WHAT IS DELIBERATELY NOT LOADED
// -------------------------------
// `type` and `security` (the InvokeType/InvokeSecurity pair a call declares)
// may be written in the file for the reader, and this loader IGNORES them
// completely: they are behaviour, they belong to the tool's write_attributes(),
// and a declaration file that quietly overrode a security decision would be a
// way to change policy without touching code or its review. The same goes for
// every other key: what this header does not name, it does not read.
//
// That leaves a file able to state something the implementation does not do,
// which is why a test pins the two together: the process toolset's suite loads
// each file and checks what it claims against what the tool settles and
// declares (test_tools.cpp), so a declaration that rots fails the build's
// tests rather than quietly misinforming whoever reads it.
//
// FAILURE
// -------
// Two entry points, one policy each:
//
//   load_tool_declaration()      throws ToolDeclarationError — for a caller
//                                that cannot continue without the tool;
//   try_load_tool_declaration()  reports the same failure through the log and
//                                answers nullopt — for a toolset builder, whose
//                                contract is to run without a tool it cannot
//                                describe rather than to fail the host.
//
// Nothing here parses YAML itself: yamlconfig/yaml_json.hpp is the project's
// one YAML boundary (anchors, merge keys, comments, and one error type), and
// this header adds only what a *tool declaration* has to satisfy on top of it.
//

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace tools::intrinsic {

/**
 * One tool's model-facing declaration, as its YAML file states it.
 *
 * The whole of what a model is told about a tool, and nothing about how it
 * behaves: the type/security pair a call declares and the validation
 * ensure_arguments() performs are the implementation's, not this struct's.
 */
struct ToolDeclaration {
    /// The tool's name — the key ToolSet::dispatch() routes by.
    std::string name;

    /// The prose a model reads before calling it.
    std::string description;

    /// The JSON Schema of the call's `arguments`, carried verbatim.
    nlohmann::json argument_schema;
};

/**
 * A declaration that cannot be used: an unreadable file, a YAML syntax error,
 * or a document missing (or malformed in) one of the three things a tool
 * cannot be advertised without.
 *
 * Deliberately its own type rather than yamlconfig::YamlConfigError: the
 * message of the one exception a caller here has to catch should not depend on
 * which YAML library sits behind the loader. It carries the file (and, for a
 * shape problem, the in-document path) so a packaging mistake names the file it
 * is in.
 */
class ToolDeclarationError : public std::runtime_error {
public:
    explicit ToolDeclarationError(const std::string& what_arg)
        : std::runtime_error(what_arg) {}
};

/**
 * Load and validate one tool's declaration.
 *
 * @param file the declaration file. Nothing about the path is interpreted
 *        here — a package decides where its files live and passes the whole
 *        path (the process toolset's schemas.hpp is that decision, made once).
 * @throws ToolDeclarationError if the file cannot be read, is not YAML, is not
 *         a mapping, or does not carry a non-empty `name`, a non-empty
 *         `description` and an object-typed `argument_schema` whose `required`
 *         names only declared properties. `type`/`security` and any other key
 *         are not read at all (see the file header).
 */
[[nodiscard]] ToolDeclaration load_tool_declaration(
    const std::filesystem::path& file);

/**
 * load_tool_declaration(), with the failure REPORTED instead of thrown: the
 * reason goes to the log at error level and the answer is nullopt.
 *
 * This is the form a tool uses. A declaration that cannot be loaded is a
 * packaging mistake that must be loud, but it is not a reason to take the host
 * down: the caller leaves the tool unnamed, and IntrinsicToolSet::register_tools()
 * then skips it — so a model is never offered a tool whose description and
 * schema nobody could find, and the operator has one error line saying which
 * file failed and why.
 */
[[nodiscard]] std::optional<ToolDeclaration> try_load_tool_declaration(
    const std::filesystem::path& file);

} // namespace tools::intrinsic
