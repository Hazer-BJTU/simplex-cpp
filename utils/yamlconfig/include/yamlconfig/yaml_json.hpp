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
 * THIS HEADER IS THE INTERFACE; THE CONVERSION LIVES IN A SHARED LIBRARY
 * (yamlconfig_lib / libyamlconfig.so). Two reasons, and both are this tree's
 * usual ones (docs/abi-context.md):
 *
 *   - YamlConfigError crosses module boundaries — a caller in a dlopened
 *     plugin catches what a host's config load threw — and that works only
 *     while ONE copy of the type's typeinfo is authoritative per process. The
 *     destructor is therefore declared, and defined out of line: the class
 *     gets a key function, so its vtable and typeinfo are emitted by this
 *     module rather than weakly by every translation unit that includes this
 *     header.
 *   - yaml-cpp is an implementation detail. Compiled into each consumer, as a
 *     header-only bridge does, its symbols are re-exported by every module
 *     that includes this header — two copies in one process the moment a
 *     plugin does the same, with symbol interposition deciding which one wins.
 *     Behind the library it is compiled exactly once, by the only module that
 *     includes yaml-cpp's headers at all (this one), and the library's link
 *     hides what the static archive contributes on top of that (see its
 *     CMakeLists.txt for what that option can and cannot do).
 *
 * Consumers link `yamlconfig_iface` and include this header. They need neither
 * yaml-cpp's headers nor its library, and nothing about the conversion is
 * template-visible: `parse()` and `load_file()` are ordinary functions behind
 * the shared object.
 */

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

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

    /// Out of line on purpose — the key function of the class, and what makes
    /// this library the one emitter of its vtable and typeinfo. See the file
    /// header.
    ~YamlConfigError() override;
};

/**
 * @brief Parse a YAML config string into JSON.
 *
 * @param yaml_text The document text (one document; empty -> json null).
 * @throws YamlConfigError on a syntax error, multiple documents, or YAML
 *         richness JSON cannot carry (see the file header).
 */
nlohmann::json parse(std::string_view yaml_text);

/**
 * @brief Load and convert a YAML config file.
 *
 * @param file Path to the config document (one document; empty -> null).
 * @throws YamlConfigError if the file cannot be read (wrapped BadFile), or
 *         for any reason parse() would reject.
 */
nlohmann::json load_file(const std::filesystem::path& file);

} // namespace yamlconfig
