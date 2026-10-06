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
// loaded once when the tool is constructed. A separate optional `config`
// mapping carries host-side construction settings, never model-facing data.
//
// WHAT A DECLARATION FILE LOOKS LIKE
// ----------------------------------
//
//   name: spawn_process
//   description: >-
//     Run a program. Waits a short while for it to finish, ...
//   type: serial_write          # documentation only — see below
//   security: require_confirm   # documentation only — see below
//   config: {}                  # optional host-side settings
//   argument_schema:
//     type: object
//     required: [executable]
//     properties:
//       executable:
//         type: string
//         description: >-
//           Program to run: a name resolved through PATH ('grep'), or a path
//           ('/usr/bin/grep')
//
// `argument_schema` is a JSON Schema and is carried VERBATIM: the converted
// subtree is what goes on the wire (llm/compat/chat_completions/src/interpreter.cpp
// puts it in the request's `parameters`), and what a host hands a model. That
// is the reason the file is YAML rather than a C++ builder chain — this is a
// document, and it reads like one.
//
// WHAT THE SCHEMA MAY SAY, AND WHY IT IS A CLOSED LIST
// ----------------------------------------------------
// Because that subtree leaves this process verbatim, every keyword in it is one
// this loader CHECKS: a keyword it let through unread would be one nothing
// downstream could notice was wrong, shown to a model as the contract for a
// call while the code that really decides whether the call runs — the accessors
// in tool_base.hpp, called from ensure_arguments() — knew nothing about it. So
// the vocabulary is exactly what this tree's tools can express:
//
//   argument_schema  type: object, properties, required,
//                    additionalProperties: false, anyOf
//   a property       type (string | boolean | integer | array | object),
//                    description, default, enum, minimum, maximum, minLength,
//                    maxLength, pattern, minItems, items; objects may also state
//                    properties, required and boolean additionalProperties
//   items            type: string, or a recursively checked object schema
//   an anyOf branch  required, optional not: {required: [property]} or
//                    not: {anyOf: [{required: [property]}, ...]}, and properties
//                    whose entries narrow with checked value clauses
//
// Object nesting is bounded to 32 levels. Opaque provider/extras objects may
// omit properties. Object and object-array defaults/enums are deliberately refused:
// no tool needs them yet, so their instance validation is not part of this API.
//
// and everything else is refused BY NAME, with a message that says what the
// vocabulary is. Adding a keyword is then a deliberate act: the accessor or the
// implementation rule it describes comes first, and the loader's check for it
// second.
//
// `anyOf` is how a declaration states a rule that spans properties, which no
// per-property clause can:
//
//   argument_schema:
//     type: object
//     required: [session_id]
//     properties:
//       session_id: {type: string, minLength: 1, description: ...}
//       input:      {type: string, default: "", description: ...}
//       close_input: {type: boolean, default: false, description: ...}
//     anyOf:                      # send something, or close the input
//       - required: [input]
//         properties: {input: {minLength: 1}}
//       - required: [close_input]
//         properties: {close_input: {enum: [true]}}
//
// Every branch adds a required property or a checked absence predicate. Without
// either, the branch adds no constraint and is refused. Existing process schemas
// use additional requirements; list/query tools can also describe empty argument
// objects through explicit exclusions. Concrete tools still validate arguments
// at invocation time; schema checks do not grant runtime authorization.
//
// WHAT IS DELIBERATELY NOT LOADED
// -------------------------------
// `type` and `security` (the InvokeType/InvokeSecurity pair a call declares)
// may be written in the file for the reader, and this loader IGNORES them
// completely: they are behaviour, they belong to the tool's write_attributes(),
// and a declaration file that quietly overrode a security decision would be a
// way to change policy without touching code or its review. The same goes for
// every other key at the document's top level except `config`: what this
// header does not name, it does not read. (Inside `argument_schema` the rule is the opposite — see
// above — because that is the part that goes on the wire.)
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
 * One tool's YAML declaration and separate host-side construction settings.
 *
 * Only name, description and argument_schema are advertised to the model.
 * The concrete tool validates config; type/security remain C++ policy.
 * Config is unrestricted implementation data, not an argument schema.
 */
struct ToolDeclaration {
    /// The tool's name — the key ToolSet::dispatch() routes by.
    std::string name;

    /// The prose a model reads before calling it.
    std::string description;

    /// The JSON Schema of the call's `arguments`, carried verbatim.
    nlohmann::json argument_schema;

    /// Owned host-side mapping; omission means {}. Never part of Invocable.
    nlohmann::json config = nlohmann::json::object();
};

/**
 * A declaration that cannot be used: an unreadable file, a YAML syntax error,
 * a missing/malformed model-facing field, or a non-mapping config.
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
 *         `description` and an `argument_schema` that is an object schema using
 *         this header's vocabulary correctly — a supported `type` on every
 *         property, a description on every property, a `default` (and an
 *         `enum`) that agrees with the type and the other clauses, `items` on
 *         every array, a `required` naming only declared properties, and
 *         `anyOf` branches that narrow rather than introduce. `type`/`security`
 *         are not read; optional config must be a mapping and is retained
 *         without applying the argument-schema vocabulary. Other unrecognized
 *         top-level keys are ignored (see the file header).
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
