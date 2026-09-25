#include "tools/intrinsic/tool_declaration.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>

#include "logging/logger.hpp"
#include "yamlconfig/yaml_json.hpp"

namespace tools::intrinsic {
namespace {

using nlohmann::json;

/// Refuse a document, in the spelling the YAML boundary itself uses
/// ("in <file>: at <path>: <reason>"): a reader then sees one format whether
/// the problem was YAML syntax, a missing key or a malformed one.
[[noreturn]] void fail(const std::filesystem::path& file, std::string_view path,
                       std::string_view reason)
{
    throw ToolDeclarationError(std::format(
        "in {}: at {}: {}", file.string(),
        path.empty() ? std::string_view("/") : path, reason));
}

/// A block scalar written with `|` keeps its final newline, and a description
/// is prose a model reads rather than a document, so trailing whitespace goes.
/// Interior newlines are left alone: whether a description is one line or
/// several is the author's call, and YAML already offers `>-` for the folded
/// form.
std::string trimmed(std::string text)
{
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    text.erase(last == std::string::npos ? 0 : last + 1);
    return text;
}

[[nodiscard]] bool is_one_of(std::string_view word,
                             std::initializer_list<std::string_view> words)
{
    for (const std::string_view candidate : words) {
        if (word == candidate) return true;
    }
    return false;
}

/// The non-empty string under `key` of `object`, or a refusal naming where it
/// should have been. `path` is the in-document path of `object` itself ("" at
/// the document root), and `what` completes both messages ("the tool's name").
std::string require_string_at(const json& object, std::string_view key,
                              const std::filesystem::path& file,
                              const std::string& path, std::string_view what)
{
    const std::string here = path + "/" + std::string(key);
    const auto found = object.find(key);
    if (found == object.end()) {
        fail(file, here, std::format("{} is missing", what));
    }
    if (!found->is_string()) {
        fail(file, here, std::format("{} must be a string, got {}", what,
                                     found->type_name()));
    }
    std::string value = trimmed(found->get<std::string>());
    if (value.empty()) {
        fail(file, here, std::format("{} must not be empty", what));
    }
    return value;
}

// ---- the argument-schema vocabulary -----------------------------------------
//
// WHY IT IS A CLOSED LIST. `argument_schema` is carried VERBATIM to a provider
// (llm/compat/chat_completions/src/interpreter.cpp puts it in the request's
// `parameters`), which makes it the one part of a declaration that leaves this
// process without anything downstream re-reading it. A keyword that reaches the
// wire unexamined is therefore a keyword nobody can promise anything about: a
// mistyped `minumum`, an `enum` contradicting its `default`, a `minimum` on a
// string would all be shown to a model as the contract for a call, while the
// code that really decides whether the call runs — the accessors in
// tool_base.hpp, called from ensure_arguments() — knows nothing about them.
//
// So every keyword is one this loader KNOWS and CHECKS, and the vocabulary is
// what this tree's tools can actually express:
//
//   type         string | boolean | integer | array — the kinds the argument
//                accessors read. Not `number` (nothing here reads a float) and
//                not `object` (no accessor reads a nested object).
//   description  the prose a model reads; required, because a property nobody
//                explained is a property a model has to guess at.
//   default      what the implementation settles when the property is absent.
//   enum         the values a caller may send, all of the declared kind.
//   minimum      a lower bound, on an integer.
//   maximum      an upper bound, on an integer.
//   minLength    a shortest length, on a string.
//   items        what an array's elements are; `{type: string}` is the only
//                array this tree reads (optional_string_list).
//
// and `anyOf` at the top of the schema, whose branches may require properties
// and narrow them with the same value clauses (see the file header). Anything
// else is refused by name, which is the point: extending the vocabulary is a
// deliberate act — the accessor or the implementation rule it describes comes
// first, and then the check for it below.

constexpr std::string_view kSchemaVocabulary =
    "type, properties, required, anyOf";
constexpr std::string_view kPropertyVocabulary =
    "type, description, default, enum, minimum, maximum, minLength, items";
constexpr std::string_view kNarrowingVocabulary = "enum, minimum, maximum, minLength";
constexpr std::string_view kTypeVocabulary = "string, boolean, integer, array";

/// Whether `value` is a JSON instance of `kind`. No coercion, and the same rule
/// the implementation's accessors apply to a call: a "5" is not an integer, and
/// neither is 5.0 — a schema that said otherwise would be describing a call
/// nobody can make.
[[nodiscard]] bool has_kind(const json& value, std::string_view kind)
{
    if (kind == "string") return value.is_string();
    if (kind == "boolean") return value.is_boolean();
    if (kind == "integer") return value.is_number_integer();
    if (kind == "array") return value.is_array();
    return false;
}

/// The kind a property declares: required, and one of the kinds above.
[[nodiscard]] std::string require_kind(const json& property,
                                       const std::filesystem::path& file,
                                       const std::string& path)
{
    const auto type = property.find("type");
    if (type == property.end()) {
        fail(file, path + "/type",
             std::format("a property must declare its type (type: one of {})",
                         kTypeVocabulary));
    }
    if (!type->is_string()) {
        fail(file, path + "/type",
             std::format("the declared type must be a string, got {}",
                         type->type_name()));
    }
    const std::string word = type->get<std::string>();
    if (!is_one_of(word, {"string", "boolean", "integer", "array"})) {
        // The list is the accessors' list, so this is also the answer to "why
        // may I not write `number`?": nothing in this tree reads one.
        fail(file, path + "/type",
             std::format("\"{}\" is not a kind of argument this project's tools "
                         "take; the vocabulary is {}", word, kTypeVocabulary));
    }
    return word;
}

/// The value clauses a property states — `enum`, `minimum`, `maximum`, `minLength` — each
/// checked against the kind it applies to, and handed back so a caller can hold
/// a `default` (or a sibling clause) against them.
struct ValueClauses {
    const json* allowed_values = nullptr; ///< `enum`, when stated
    const json* minimum = nullptr;        ///< `minimum`, when stated
    const json* maximum = nullptr;        ///< `maximum`, when stated
    const json* min_length = nullptr;     ///< `minLength`, when stated

    /// Nothing stated about the values, so any value of the declared kind is a
    /// value the declaration allows.
    [[nodiscard]] bool says_nothing() const noexcept
    {
        return allowed_values == nullptr && minimum == nullptr && maximum == nullptr
               && min_length == nullptr;
    }
};

/// Whether `value` — a clause's own value, or a `default` — satisfies the
/// clauses stated alongside it. `path` names the value being judged.
void check_value_against_clauses(const json& value, std::string_view kind,
                                 const ValueClauses& clauses,
                                 const std::filesystem::path& file,
                                 const std::string& path)
{
    if (!has_kind(value, kind)) {
        fail(file, path,
             std::format("must be a {}, got {}", kind, value.type_name()));
    }
    if (clauses.allowed_values != nullptr
        && std::find(clauses.allowed_values->begin(),
                     clauses.allowed_values->end(), value)
               == clauses.allowed_values->end()) {
        fail(file, path,
             std::format("{} is not one of the values this declaration lists "
                         "in its enum", value.dump()));
    }
    if (clauses.minimum != nullptr && value < *clauses.minimum) {
        fail(file, path,
             std::format("{} is below the minimum this declaration states ({})",
                         value.dump(), clauses.minimum->dump()));
    }
    if (clauses.maximum != nullptr && value > *clauses.maximum) {
        fail(file, path,
             std::format("{} is above the maximum this declaration states ({})",
                         value.dump(), clauses.maximum->dump()));
    }
    if (clauses.min_length != nullptr) {
        const auto shortest = static_cast<std::size_t>(
            clauses.min_length->get<std::int64_t>());
        if (value.get<std::string>().size() < shortest) {
            fail(file, path,
                 std::format("{} is shorter than the minLength this "
                             "declaration states ({})",
                             value.dump(), clauses.min_length->dump()));
        }
    }
}

/// Check the `enum`/`minimum`/`maximum`/`minLength` a schema states against `kind` and
/// answer them.
///
/// The clauses are checked against EACH OTHER as well as against the kind —
/// an enum member below the minimum, a minimum on a string — because a document
/// that disagrees with itself is one whose readers (a model, a provider, a
/// reviewer) would each take something different from it.
[[nodiscard]] ValueClauses check_clauses(const json& schema,
                                         std::string_view kind,
                                         const std::filesystem::path& file,
                                         const std::string& path)
{
    ValueClauses clauses;

    if (const auto values = schema.find("enum"); values != schema.end()) {
        if (!values->is_array() || values->empty()) {
            fail(file, path + "/enum",
                 std::format("enum must be a non-empty array of {} values, got "
                             "{}", kind, values->type_name()));
        }
        clauses.allowed_values = &*values;
    }
    if (const auto lower = schema.find("minimum"); lower != schema.end()) {
        if (kind != "integer") {
            fail(file, path + "/minimum",
                 std::format("minimum applies to an integer property, and this "
                             "one is a {}", kind));
        }
        if (!lower->is_number_integer()) {
            fail(file, path + "/minimum",
                 std::format("minimum must be an integer, got {}",
                             lower->type_name()));
        }
        clauses.minimum = &*lower;
    }
    if (const auto upper = schema.find("maximum"); upper != schema.end()) {
        if (kind != "integer") {
            fail(file, path + "/maximum",
                 std::format("maximum applies to an integer property, and this "
                             "one is a {}", kind));
        }
        if (!upper->is_number_integer()) {
            fail(file, path + "/maximum",
                 std::format("maximum must be an integer, got {}",
                             upper->type_name()));
        }
        clauses.maximum = &*upper;
    }
    if (clauses.minimum != nullptr && clauses.maximum != nullptr &&
        *clauses.minimum > *clauses.maximum) {
        fail(file, path + "/maximum",
             "maximum must not be below minimum");
    }
    if (const auto length = schema.find("minLength"); length != schema.end()) {
        if (kind != "string") {
            fail(file, path + "/minLength",
                 std::format("minLength applies to a string property, and this "
                             "one is a {}", kind));
        }
        if (!length->is_number_integer()) {
            fail(file, path + "/minLength",
                 std::format("minLength must be an integer, got {}",
                             length->type_name()));
        }
        if (length->get<std::int64_t>() < 0) {
            fail(file, path + "/minLength",
                 "minLength must not be negative: no string is shorter than "
                 "zero characters, so it would say nothing");
        }
        clauses.min_length = &*length;
    }

    if (clauses.allowed_values != nullptr) {
        // Each member against its sibling clauses: an enum that lists a
        // value outside its own bounds is a document two readers would take
        // two ways.
        for (std::size_t index = 0; index < clauses.allowed_values->size();
             ++index) {
            check_value_against_clauses(
                (*clauses.allowed_values)[index], kind, clauses, file,
                std::format("{}/enum/{}", path, index));
        }
    }
    return clauses;
}

/// The `items` of an array property: what an element is. This tree has one
/// array accessor and it reads strings (optional_string_list), so `{type:
/// string}` is the whole of what an element may be declared as — and saying so
/// is required, because an array with no element rule is one a model can only
/// guess about.
void check_items(const json& items, const std::filesystem::path& file,
                 const std::string& path)
{
    if (!items.is_object()) {
        fail(file, path,
             std::format("items must be a mapping describing the elements "
                         "({{type: string}}), got {}", items.type_name()));
    }
    for (const auto& entry : items.items()) {
        if (entry.key() != "type") {
            fail(file, path + "/" + entry.key(),
                 std::format("\"{}\" is not something this project declares "
                             "about an array's elements: `type: string` is "
                             "the whole of it", entry.key()));
        }
    }
    const std::string kind = require_kind(items, file, path);
    if (kind != "string") {
        fail(file, path + "/type",
             std::format("an array's elements must be strings, got {}: "
                         "optional_string_list() is the only array this tree "
                         "reads", kind));
    }
}

/// One property under `properties`, checked whole: its kind, the prose a model
/// reads, its value clauses, what it settles when absent, and — for an array —
/// what its elements are.
void check_property(const json& property, const std::filesystem::path& file,
                    const std::string& path)
{
    if (!property.is_object()) {
        fail(file, path, std::format("every property must be a mapping, got {}",
                                     property.type_name()));
    }
    for (const auto& entry : property.items()) {
        if (!is_one_of(entry.key(), {"type", "description", "default", "enum",
                                     "minimum", "maximum", "minLength", "items"})) {
            fail(file, path + "/" + entry.key(),
                 std::format("\"{}\" is not part of the argument-schema "
                             "vocabulary this project supports ({}); a keyword "
                             "the loader does not check is one nothing "
                             "downstream would notice was wrong",
                             entry.key(), kPropertyVocabulary));
        }
    }

    const std::string kind = require_kind(property, file, path);
    (void)require_string_at(property, "description", file, path,
                            "the property's description");

    if (const auto items = property.find("items"); items != property.end()) {
        if (kind != "array") {
            fail(file, path + "/items",
                 std::format("only an array property takes items, and this one "
                             "declares type: {}", kind));
        }
        check_items(*items, file, path + "/items");
    } else if (kind == "array") {
        fail(file, path + "/items",
             "an array property must declare what its elements are "
             "(items: {type: string})");
    }

    const ValueClauses clauses = check_clauses(property, kind, file, path);
    if (const auto fallback = property.find("default");
        fallback != property.end()) {
        // The default is what the implementation settles, so it has to be a
        // value the same declaration would let a caller send: of the kind, in
        // the enum, inside the numeric bounds. Anything else is a schema that describes
        // two different call sets depending on who is reading it.
        check_value_against_clauses(*fallback, kind, clauses, file,
                                    path + "/default");
        if (kind == "array") {
            for (std::size_t index = 0; index < fallback->size(); ++index) {
                if (!has_kind((*fallback)[index], "string")) {
                    fail(file, std::format("{}/default/{}", path, index),
                         std::format("every element of an array default must "
                                     "be a string, got {}",
                                     (*fallback)[index].type_name()));
                }
            }
        }
    }
}

/// An array of property names, each naming a property the schema declares.
/// Shared by the schema's own `required` and by each `anyOf` branch's, so the
/// two can never disagree about what a name has to be.
void check_required_list(const json& names, const json& properties,
                         const std::filesystem::path& file,
                         const std::string& path)
{
    if (!names.is_array()) {
        fail(file, path, "required must be an array of property names");
    }
    for (std::size_t index = 0; index < names.size(); ++index) {
        const std::string here = std::format("{}/{}", path, index);
        const json& entry = names[index];
        if (!entry.is_string()) {
            fail(file, here, "required entries must be property names (strings)");
        }
        const std::string name = entry.get<std::string>();
        // A required property nobody declared is a typo that would otherwise
        // reach a model as a name it cannot fill in.
        if (!properties.contains(name)) {
            fail(file, here,
                 std::format("\"{}\" is required but no such property is "
                             "declared under properties", name));
        }
    }
}

/// Whether `names` — an already-checked required list — holds `name`.
[[nodiscard]] bool names_contain(const json& names, std::string_view name)
{
    for (const json& entry : names) {
        if (entry.get<std::string>() == name) return true;
    }
    return false;
}

/// One alternative of a top-level `anyOf`: the properties it requires, and the
/// clauses it narrows them with.
///
/// A branch is checked as a NARROWING of the schema, never a schema of its own:
/// it may name properties the schema declares (it cannot introduce one), it
/// must require at least one property the schema does not already require (a
/// branch satisfied by every call the schema allows would make the `anyOf` say
/// nothing at all — and that is exactly the property the tests lean on when
/// they ask whether a call naming only the required properties is valid), and
/// its property entries may only tighten a value (`enum`, `minimum`,
/// `maximum`, `minLength`), because the type and the description come from the property
/// itself.
void check_branch(const json& branch, const json& properties,
                  const json& required, const std::filesystem::path& file,
                  std::size_t index)
{
    const std::string path = std::format("/argument_schema/anyOf/{}", index);
    if (!branch.is_object()) {
        fail(file, path, std::format("every alternative must be a mapping, got "
                                     "{}", branch.type_name()));
    }
    for (const auto& entry : branch.items()) {
        if (!is_one_of(entry.key(), {"required", "properties"})) {
            fail(file, path + "/" + entry.key(),
                 std::format("\"{}\" is not something an alternative may state "
                             "(required, properties); what an alternative does "
                             "is require properties of the call, and narrow "
                             "the values they may carry", entry.key()));
        }
    }

    const auto names = branch.find("required");
    if (names == branch.end()) {
        fail(file, path + "/required",
             "an alternative must require at least one property: one that "
             "requires nothing is satisfied by every call, so it adds nothing "
             "to the schema");
    }
    check_required_list(*names, properties, file, path + "/required");
    bool adds_a_requirement = false;
    for (const json& name : *names) {
        adds_a_requirement = adds_a_requirement
                             || !names_contain(required, name.get<std::string>());
    }
    if (!adds_a_requirement) {
        fail(file, path + "/required",
             "an alternative must require a property the schema does not "
             "already require, or it is satisfied by every call that satisfies "
             "the schema");
    }

    if (const auto narrowed = branch.find("properties");
        narrowed != branch.end()) {
        if (!narrowed->is_object()) {
            fail(file, path + "/properties",
                 std::format("an alternative's properties must be a mapping, "
                             "got {}", narrowed->type_name()));
        }
        for (const auto& entry : narrowed->items()) {
            const std::string here = std::format("{}/properties/{}", path,
                                                 entry.key());
            const auto property = properties.find(entry.key());
            if (property == properties.end()) {
                fail(file, here,
                     std::format("\"{}\" is not a property this schema "
                                 "declares: an alternative narrows a property, "
                                 "it does not introduce one", entry.key()));
            }
            if (!entry.value().is_object()) {
                fail(file, here,
                     std::format("a narrowing must be a mapping, got {}",
                                 entry.value().type_name()));
            }
            for (const auto& clause : entry.value().items()) {
                if (!is_one_of(clause.key(), {"enum", "minimum", "maximum", "minLength"})) {
                    fail(file, here + "/" + clause.key(),
                         std::format("\"{}\" is not something an alternative "
                                     "may narrow ({}): the type and the "
                                     "description come from the property "
                                     "itself", clause.key(),
                                     kNarrowingVocabulary));
                }
            }
            const std::string kind =
                require_kind(*property, file, "/argument_schema/properties/"
                                                 + entry.key());
            if (check_clauses(entry.value(), kind, file, here).says_nothing()) {
                fail(file, here,
                     "a narrowing must state one of enum, minimum, maximum or "
                     "minLength; leave the entry out to require the property "
                     "as it stands");
            }
        }
    }
}

/// The `argument_schema` subtree: everything above — the shape a tool cannot be
/// advertised without, the vocabulary it may use, and the alternatives a call
/// has to satisfy — handed back verbatim.
json require_argument_schema(const json& document, const std::filesystem::path& file)
{
    const auto schema = document.find("argument_schema");
    if (schema == document.end()) {
        fail(file, "/argument_schema",
             "a declaration must carry the tool's argument schema");
    }
    if (!schema->is_object()) {
        fail(file, "/argument_schema",
             std::format("the argument schema must be a mapping, got {}",
                         schema->type_name()));
    }
    const std::string base = "/argument_schema";
    for (const auto& entry : schema->items()) {
        if (!is_one_of(entry.key(),
                       {"type", "properties", "required", "anyOf"})) {
            fail(file, base + "/" + entry.key(),
                 std::format("\"{}\" is not part of the argument-schema "
                             "vocabulary this project supports ({}); the "
                             "subtree goes on the wire verbatim, so a keyword "
                             "the loader cannot check is one nothing "
                             "downstream would notice was wrong",
                             entry.key(), kSchemaVocabulary));
        }
    }

    const auto type = schema->find("type");
    if (type == schema->end() || !type->is_string()
        || type->get<std::string>() != "object") {
        // Every tool in this tree takes one object of named arguments, and a
        // schema that says otherwise is not something a caller here could hand
        // to a model as a tool definition.
        fail(file, base + "/type",
             "the argument schema must describe an object (type: object)");
    }

    const json no_properties = json::object();
    const json no_required = json::array();
    const auto declared = schema->find("properties");
    if (declared != schema->end() && !declared->is_object()) {
        fail(file, base + "/properties",
             std::format("properties must be a mapping, got {}",
                         declared->type_name()));
    }
    const json& properties =
        declared != schema->end() ? *declared : no_properties;
    for (const auto& entry : properties.items()) {
        check_property(entry.value(), file,
                       base + "/properties/" + entry.key());
    }

    const auto required = schema->find("required");
    if (required != schema->end()) {
        check_required_list(*required, properties, file, base + "/required");
    }
    const json& required_names = required != schema->end() ? *required
                                                           : no_required;

    if (const auto alternatives = schema->find("anyOf");
        alternatives != schema->end()) {
        if (!alternatives->is_array() || alternatives->empty()) {
            fail(file, base + "/anyOf",
                 std::format("anyOf must be a non-empty array of alternatives, "
                             "got {}", alternatives->type_name()));
        }
        for (std::size_t index = 0; index < alternatives->size(); ++index) {
            check_branch((*alternatives)[index], properties, required_names,
                         file, index);
        }
    }
    return *schema;
}

} // namespace

ToolDeclaration load_tool_declaration(const std::filesystem::path& file)
{
    json document;
    try {
        document = yamlconfig::load_file(file);
    } catch (const yamlconfig::YamlConfigError& failure) {
        // Already carries the file, the in-document path and, when yaml-cpp
        // supplies one, the source mark.
        throw ToolDeclarationError(failure.what());
    } catch (const std::exception& failure) {
        // Anything else the boundary did not wrap (a directory where a file was
        // expected, say). The contract here is one exception type, so a caller
        // has one thing to catch and the file is still named.
        throw ToolDeclarationError(
            std::format("in {}: cannot read the declaration ({})",
                        file.string(), failure.what()));
    }

    if (!document.is_object()) {
        // Including the empty document: an empty file declares nothing, which
        // is a declaration nobody can use rather than an empty one.
        fail(file, "", std::format("a tool declaration must be a YAML mapping, "
                                   "got {}", document.type_name()));
    }

    ToolDeclaration declaration;
    declaration.name = require_string_at(document, "name", file, "",
                                        "the tool's name");
    declaration.description = require_string_at(document, "description", file, "",
                                                "the tool's description");
    declaration.argument_schema = require_argument_schema(document, file);
    return declaration;
}

std::optional<ToolDeclaration> try_load_tool_declaration(
    const std::filesystem::path& file)
{
    try {
        return load_tool_declaration(file);
    } catch (const std::exception& failure) {
        // Report and carry on: the caller's answer to this is to leave the tool
        // unnamed, and IntrinsicToolSet::register_tools() then skips it — a
        // broken declaration must be loud, but it must not take the host down
        // or silently advertise a tool whose schema nobody could find.
        logging::Logger::error(std::format(
            "tool declaration rejected: {} — the tool it declares is not "
            "registered", failure.what()));
        return std::nullopt;
    }
}

} // namespace tools::intrinsic
