#include "tools/intrinsic/skill_declaration.hpp"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "logging/logger.hpp"
#include "yamlconfig/yaml_json.hpp"

namespace tools::intrinsic {
namespace {

using nlohmann::json;

/// Refuse a document, in the spelling the YAML boundary itself uses
/// ("in <file>: at <path>: <reason>"): a reader then sees one format whether
/// the problem was YAML syntax, a missing key or a malformed one. The same
/// shape tool_declaration.cpp uses, for the same reason.
[[noreturn]] void fail(const std::filesystem::path& file, std::string_view path,
                       std::string_view reason)
{
    throw SkillDeclarationError(std::format(
        "in {}: at {}: {}", file.string(),
        path.empty() ? std::string_view("/") : path, reason));
}

/// Strip the whitespace a block scalar brings with it. A role for the document
/// rather than for the loader: the fields here are trimmed on the way in, so
/// what a caller compares, renders or logs is the text and not the indentation
/// it was written under.
///
/// Interior newlines survive — `text` is markdown, and whether it is one
/// paragraph or a list of them is the author's call (the same rule, and the
/// same helper, as the tool declaration loader's).
std::string trimmed(std::string text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/// The non-empty string under `key`, or a refusal naming where it should have
/// been. `path` is the in-document path of `object` itself ("" at the document
/// root) and `what` completes both messages ("the skill's name").
[[nodiscard]] std::string require_string_at(const json& object,
                                            std::string_view key,
                                            const std::filesystem::path& file,
                                            std::string_view what)
{
    const std::string here = "/" + std::string(key);
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

/// The non-empty string under `key`, or "" when the key is absent — for the
/// fields a skill may leave out. A key that IS there is held to the same rule
/// as a required one: an empty title or description is a field whose author
/// meant to say something and did not.
[[nodiscard]] std::string optional_string_at(const json& object,
                                             std::string_view key,
                                             const std::filesystem::path& file,
                                             std::string_view what)
{
    if (!object.contains(key)) return {};
    return require_string_at(object, key, file, what);
}

/// The sequence of strings under `keywords`, or {} when the key is absent.
///
/// Every entry is checked where a caller would have to guess otherwise — a
/// non-string names its own index, and a word listed twice is refused because
/// it selects exactly what it selected once.
[[nodiscard]] std::vector<std::string> optional_keywords(
    const json& document, const std::filesystem::path& file)
{
    const auto found = document.find("keywords");
    if (found == document.end()) return {};

    if (!found->is_array()) {
        fail(file, "/keywords",
             std::format("keywords must be a list of words, got {}",
                         found->type_name()));
    }
    std::vector<std::string> keywords;
    keywords.reserve(found->size());
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0; index < found->size(); ++index) {
        const std::string here = std::format("/keywords/{}", index);
        const json& entry = (*found)[index];
        if (!entry.is_string()) {
            fail(file, here, std::format("every keyword must be a string, got "
                                         "{}", entry.type_name()));
        }
        std::string keyword = trimmed(entry.get<std::string>());
        if (keyword.empty()) {
            fail(file, here, "a keyword must not be empty");
        }
        if (!seen.insert(keyword).second) {
            fail(file, here,
                 std::format("\"{}\" is listed twice: a word listed twice "
                             "selects exactly what it selected once",
                             keyword));
        }
        keywords.push_back(std::move(keyword));
    }
    return keywords;
}

} // namespace

tools::ToolSetSkill load_skill_declaration(const std::filesystem::path& file)
{
    json document;
    try {
        document = yamlconfig::load_file(file);
    } catch (const yamlconfig::YamlConfigError& failure) {
        // Already carries the file, the in-document path and, when yaml-cpp
        // supplies one, the source mark.
        throw SkillDeclarationError(failure.what());
    } catch (const std::exception& failure) {
        // Anything else the boundary did not wrap (a directory where a file was
        // expected, say). The contract here is one exception type, so a caller
        // has one thing to catch and the file is still named.
        throw SkillDeclarationError(
            std::format("in {}: cannot read the skill declaration ({})",
                        file.string(), failure.what()));
    }

    if (!document.is_object()) {
        // Including the empty document: an empty file says nothing about using
        // the set's tools, which is a skill nobody can inject rather than an
        // empty one.
        fail(file, "", std::format("a skill declaration must be a YAML "
                                   "mapping, got {}", document.type_name()));
    }

    tools::ToolSetSkill skill;
    skill.name = require_string_at(document, "name", file, "the skill's name");
    skill.title = optional_string_at(document, "title", file,
                                     "the skill's title");
    skill.description = optional_string_at(document, "description", file,
                                           "the skill's description");
    skill.keywords = optional_keywords(document, file);
    // The one field that leaves this process: it is carried verbatim into the
    // prompt section, so it is required and its interior newlines survive
    // (trimmed()).
    skill.text = require_string_at(document, "text", file,
                                   "the skill's text");
    return skill;
}

std::optional<tools::ToolSetSkill> try_load_skill_declaration(
    const std::filesystem::path& file)
{
    try {
        return load_skill_declaration(file);
    } catch (const std::exception& failure) {
        // Report and carry on: the caller's answer to this is to carry no
        // skill, and the set's tools stay routable — a skill that cannot be
        // read costs the model its guidance and costs the operator one error
        // line, never the host.
        logging::Logger::error(std::format(
            "skill declaration rejected: {} — the toolset keeps its tools and "
            "carries no guidance for the model", failure.what()));
        return std::nullopt;
    }
}

} // namespace tools::intrinsic
