#include "tools/intrinsic/tool_declaration.hpp"

#include <cstddef>
#include <format>
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

/// The non-empty string under `key`, or a refusal naming the path it should
/// have been at. `what` completes both messages ("the tool's name").
std::string require_string(const json& document, const std::filesystem::path& file,
                           std::string_view key, std::string_view what)
{
    const std::string path = "/" + std::string(key);
    const auto found = document.find(key);
    if (found == document.end()) {
        fail(file, path, std::format("a declaration must carry {}", what));
    }
    if (!found->is_string()) {
        fail(file, path, std::format("{} must be a string, got {}", what,
                                     found->type_name()));
    }
    std::string value = trimmed(found->get<std::string>());
    if (value.empty()) {
        fail(file, path, std::format("{} must not be empty", what));
    }
    return value;
}

/// The `argument_schema` subtree, checked for the shape a tool cannot be
/// advertised without and handed back verbatim.
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
    const auto type = schema->find("type");
    if (type == schema->end() || !type->is_string()
        || type->get<std::string>() != "object") {
        // Every tool in this tree takes one object of named arguments, and a
        // schema that says otherwise is not something a caller here could hand
        // to a model as a tool definition.
        fail(file, "/argument_schema/type",
             "the argument schema must describe an object (type: object)");
    }

    const auto properties = schema->find("properties");
    if (properties != schema->end()) {
        if (!properties->is_object()) {
            fail(file, "/argument_schema/properties",
                 std::format("properties must be a mapping, got {}",
                             properties->type_name()));
        }
        for (const auto& entry : properties->items()) {
            if (!entry.value().is_object()) {
                fail(file, "/argument_schema/properties/" + entry.key(),
                     std::format("every property must be a mapping, got {}",
                                 entry.value().type_name()));
            }
        }
    }

    const auto required = schema->find("required");
    if (required != schema->end()) {
        if (!required->is_array()) {
            fail(file, "/argument_schema/required",
                 "required must be an array of property names");
        }
        for (std::size_t index = 0; index < required->size(); ++index) {
            const std::string path =
                std::format("/argument_schema/required/{}", index);
            const json& entry = (*required)[index];
            if (!entry.is_string()) {
                fail(file, path,
                     "required entries must be property names (strings)");
            }
            const std::string name = entry.get<std::string>();
            // A required property nobody declared is a typo that would
            // otherwise reach a model as a name it cannot fill in.
            if (properties == schema->end() || !properties->contains(name)) {
                fail(file, path,
                     std::format("\"{}\" is required but no such property is "
                                 "declared under properties", name));
            }
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
    declaration.name =
        require_string(document, file, "name", "the tool's name");
    declaration.description =
        require_string(document, file, "description", "the tool's description");
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
