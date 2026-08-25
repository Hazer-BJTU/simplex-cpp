#pragma once

/**
 * @file yamlconfig/yaml_json.hpp
 * @brief Convert human-written YAML configuration into nlohmann JSON.
 *
 * The project's configuration contracts (model plugins' config JSON, the
 * upcoming host config) are nlohmann objects, but YAML is what humans
 * actually want to write — comments, anchors for shared blocks, merge keys.
 * This header is that one-way bridge: everything YAML can express that JSON
 * can represent, and NOTHING else.
 *
 * Supported (the deliberate subset):
 *   null / bool / integer / floating point / string scalars,
 *   mappings -> objects, sequences -> arrays,
 *   anchors & aliases (dereferenced and copied),
 *   `<<` merge keys (map or sequence-of-maps value; explicit keys win),
 *   YAML 1.1 boolean spellings (yes/no/on/off, per yaml-cpp).
 *
 * Rejected (YAML riches JSON cannot carry):
 *   non-scalar mapping keys   — a JSON object key must be a string. Scalar
 *                               keys are TEXTUALISED verbatim (`8080:` ->
 *                               "8080"; a quoted-vs-plain distinction that
 *                               matters to nobody reading config).
 *   .nan / .inf scalars       — JSON numbers are finite; fail loudly at the
 *                               config boundary instead of dumping `null`.
 *   multiple documents        — one config file, one object. `---` twice is
 *                               a structural mistake, so it is an error, and
 *                               an empty file parses to `null` (a deliberate
 *                               non-error: an empty optional config).
 *   cyclic aliases            — a dereferenced graph is a tree; the depth
 *                               guard (kMaxDepth) turns an alias cycle into
 *                               an error rather than a hang.
 *   custom tags               — ignored; scalars convert by their natural
 *                               (unquoted) type. Duplicate keys: last wins
 *                               (yaml-cpp behaviour, documented here).
 *
 * Errors are one type — YamlConfigError — carrying the in-document path and,
 * when yaml-cpp supplies one, the source mark: e.g.
 *   `at /server/ports/1 (line 3, column 5): non-finite number`
 * Parse-level failures (YAML::ParserException, BadFile) are wrapped with the
 * same treatment so callers need a single catch.
 *
 * Header-only, no state, no I/O beyond what the entry points do themselves.
 */

#include <cmath>       // std::isfinite
#include <cstdint>     // std::int64_t, std::uint64_t
#include <filesystem>
#include <limits>      // std::numeric_limits
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include <nlohmann/json.hpp>

namespace yamlconfig {

/// Everything this header rejects, as one type: unsupported YAML richness,
/// non-finite numbers, multi-document input, parse failures, unreadable
/// files, cyclic aliases. The message carries the in-document path (and the
/// source mark when yaml-cpp supplies one).
class YamlConfigError : public std::runtime_error {
public:
    explicit YamlConfigError(const std::string& what_arg)
        : std::runtime_error(what_arg) {}
};

namespace detail {

/// Recursion ceiling: a dereferenced alias cycle would otherwise recurse
/// forever; real configs nest a handful of levels deep. Way below any stack
/// exhaustion threshold.
inline constexpr int kMaxDepth = 256;

/// The YAML spelling of a merge key (`<<: base`), YAML 1.1 style.
inline constexpr const char* kMergeKey = "<<";

/// Render a yaml-cpp mark as " (line L, column C)" for error messages.
std::string mark_suffix(const YAML::Mark& mark) {
    if (mark.is_null()) {
        return {};
    }
    return " (line " + std::to_string(mark.line) + ", column "
         + std::to_string(mark.column) + ")";
}

/// Throw YamlConfigError with the in-document path (and mark when present).
[[noreturn]] void fail(const std::string& path, const YAML::Mark& mark,
                       const std::string& reason) {
    throw YamlConfigError("at " + (path.empty() ? std::string("/") : path)
                          + mark_suffix(mark) + ": " + reason);
}

/// Convert one scalar node's text to a JSON value by its natural type:
/// bool (YAML 1.1 spellings per yaml-cpp), int64 (falling back to uint64
/// when above int64's range), finite double, else string.
nlohmann::json scalar_value(const YAML::Node& node, const std::string& path) {
    // A quoted scalar is tagged "!": the author meant characters, not a
    // number — skip type parsing entirely so `"42"` stays a string.
    const std::string& tag = node.Tag();
    if (tag == "!") {
        return node.Scalar();
    }
    const std::string& text = node.Scalar();

    // yaml-cpp's converter implements YAML 1.1 booleans (true/false,
    // yes/no, on/off, case variants). Reuse it rather than respelling the
    // table here.
    bool boolean = false;
    if (YAML::convert<bool>::decode(node, boolean)) {
        return boolean;
    }

    // Integers: int64 first, then uint64 for the above-int64 band (huge
    // id/nonce config values round-trip exactly in JSON).
    {
        std::int64_t i64 = 0;
        if (YAML::convert<std::int64_t>::decode(node, i64)) {
            return i64;
        }
        std::uint64_t u64 = 0;
        if (YAML::convert<std::uint64_t>::decode(node, u64)) {
            return u64;
        }
    }

    {
        double real = 0.0;
        if (YAML::convert<double>::decode(node, real)) {
            // .nan/.inf/.inf-ish spellings decode successfully but have no
            // JSON representation — a config error, surfaced as one.
            if (!std::isfinite(real)) {
                fail(path, node.Mark(), "non-finite number (.nan/.inf) is not representable in JSON");
            }
            return real;
        }
    }

    return text;
}

/// The merge mapping carried by a `<<` value, as a flat YAML::Node -> used
/// by convert_map to fold defaults under the explicit keys.
struct MergeSource {
    /// One map contributing default keys (several when the value was a
    /// sequence of maps; earlier maps win over later ones, per YAML merge
    /// semantics).
    std::vector<YAML::Node> maps;
};

/// Recognise and split a `<<` value into merge maps, or report why not.
/// Accepts a single map (`<<: *base`) or a sequence of maps
/// (`<<: [*base_a, *base_b]`); anything else is a config mistake.
MergeSource merge_source(const YAML::Node& value, const std::string& path) {
    MergeSource source;
    if (value.IsMap()) {
        source.maps.push_back(value);
    } else if (value.IsSequence()) {
        for (const YAML::Node& entry : value) {
            if (!entry.IsMap()) {
                fail(path + "/<<", entry.Mark(),
                     "merge-key sequence entries must all be mappings");
            }
            source.maps.push_back(entry);
        }
    } else {
        fail(path + "/<<", value.Mark(),
             "merge key (<<) value must be a mapping or a sequence of mappings");
    }
    return source;
}

nlohmann::json convert(const YAML::Node& node, const std::string& path, int depth);

/// Convert a mapping: scalar keys textualised, `<<` merged (explicit keys
/// win), non-scalar keys rejected.
nlohmann::json convert_map(const YAML::Node& node, const std::string& path, int depth) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& kv : node) {
        const YAML::Node& key = kv.first;
        const YAML::Node& value = kv.second;
        const std::string entry = path + "/" + key.Scalar();

        if (!key.IsScalar()) {
            // Sequences/mappings as keys are legal YAML and impossible in
            // JSON — there is no faithful spelling, so refuse.
            fail(entry, key.Mark(),
                 "mapping keys must be scalars (JSON object keys are strings)");
        }

        if (key.Scalar() == kMergeKey) {
            MergeSource source = merge_source(value, path);
            // Fold defaults in sequence order, first-map-wins on shared keys
            // (YAML 1.1 merge semantics): a map already claiming a key is
            // not overridden by a later one. Explicit keys (whichever side
            // of `<<` they sit on) win over every merge source, which the
            // contains() check preserves regardless of document order.
            for (const YAML::Node& map : source.maps) {
                nlohmann::json merged = convert(map, path + "/<<", depth + 1);
                for (auto& slot : merged.items()) {
                    if (!object.contains(slot.key())) {
                        object[slot.key()] = std::move(slot.value());
                    }
                }
            }
            continue;
        }

        object[key.Scalar()] = convert(value, entry, depth + 1);
    }
    return object;
}

/// Convert one YAML node (any kind) to JSON.
nlohmann::json convert(const YAML::Node& node, const std::string& path, int depth) {
    if (depth > kMaxDepth) {
        fail(path, node.Mark(),
             "nesting exceeds the depth limit (cyclic alias?)");
    }

    switch (node.Type()) {
        case YAML::NodeType::Null:
            return nullptr;
        case YAML::NodeType::Scalar:
            return scalar_value(node, path);
        case YAML::NodeType::Sequence: {
            nlohmann::json array = nlohmann::json::array();
            for (std::size_t i = 0; i < node.size(); ++i) {
                array.push_back(
                    convert(node[i], path + "/" + std::to_string(i), depth + 1));
            }
            return array;
        }
        case YAML::NodeType::Map:
            return convert_map(node, path, depth);
        case YAML::NodeType::Undefined:
        default:
            fail(path, node.Mark(), "undefined YAML node");
    }
}

/// Shared discipline of the document-level entry points: exactly one
/// document (or `null` for none), errors wrapped with marks.
nlohmann::json single_document(std::vector<YAML::Node> documents,
                               const std::string& origin) {
    if (documents.size() > 1) {
        throw YamlConfigError(origin + ": multiple YAML documents found ("
                              + std::to_string(documents.size())
                              + "); a config file is exactly one document");
    }
    if (documents.empty()) {
        return nullptr;   // an empty optional config, deliberately not an error
    }
    try {
        return convert(documents.front(), "", 0);
    } catch (const YamlConfigError&) {
        throw;            // already carries path + mark
    } catch (const YAML::Exception& e) {
        throw YamlConfigError(origin + mark_suffix(e.mark) + ": " + e.what());
    }
}

std::string describe(const std::filesystem::path& file) {
    std::error_code ec;
    auto absolute = std::filesystem::weakly_canonical(file, ec);
    return (ec ? file : absolute).string();
}

} // namespace detail

/**
 * @brief Parse a YAML config string into JSON.
 *
 * @param yaml_text The document text (one document; empty -> json null).
 * @throws YamlConfigError on a syntax error, multiple documents, or YAML
 *         richness JSON cannot carry (see the file header).
 */
inline nlohmann::json parse(std::string_view yaml_text) {
    try {
        return detail::single_document(
            YAML::LoadAll(std::string(yaml_text)), "while parsing YAML text");
    } catch (const YamlConfigError&) {
        throw;
    } catch (const YAML::ParserException& e) {
        throw YamlConfigError(
            std::string("while parsing YAML text") + detail::mark_suffix(e.mark)
            + ": " + e.what());
    } catch (const YAML::Exception& e) {
        throw YamlConfigError(
            std::string("while parsing YAML text") + detail::mark_suffix(e.mark)
            + ": " + e.what());
    }
}

/**
 * @brief Load and convert a YAML config file.
 *
 * @param file Path to the config document (one document; empty -> null).
 * @throws YamlConfigError if the file cannot be read (wrapped BadFile), or
 *         for any reason parse() would reject.
 */
inline nlohmann::json load_file(const std::filesystem::path& file) {
    const std::string origin = "in " + detail::describe(file);
    std::vector<YAML::Node> documents;
    try {
        documents = YAML::LoadAllFromFile(file.string());
    } catch (const YAML::BadFile& e) {
        throw YamlConfigError(origin + ": cannot open file (" + e.what() + ")");
    }
    try {
        return detail::single_document(std::move(documents), origin);
    } catch (const YAML::ParserException& e) {
        throw YamlConfigError(origin + detail::mark_suffix(e.mark) + ": " + e.what());
    }
}

} // namespace yamlconfig
