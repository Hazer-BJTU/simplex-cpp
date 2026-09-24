#include "load/plugins.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "loop/extensions/plugin.hpp"
#include "tools/extensions/plugin.hpp"
#include "yamlconfig/yaml_json.hpp"

namespace load {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

/** Selected identity and optional explicit component configuration location. */
struct Selection {
    std::string name;
    fs::path configuration;
    std::string field;
};

/** Validated discovery inputs, private to one synchronous startup call. */
struct ExtensionPlan {
    std::vector<fs::path> directories;
    std::vector<Selection> selections;
};

/** Report field locations without echoing potentially sensitive input values. */
[[noreturn]] void invalid(std::string_view field, std::string_view message) {
    throw PluginLoadError(std::string(field) + ": " + std::string(message));
}

/** Omitted mappings default to empty; explicit null and other types are errors. */
const Json& mapping(const Json& parent, const char* key, const std::string& field) {
    static const Json empty = Json::object();
    const auto entry = parent.find(key);
    if (entry == parent.end()) {
        return empty;
    }
    if (!entry->is_object()) {
        invalid(field, "expected a mapping");
    }
    return *entry;
}

/** Validate the common input boundary before inspecting nested sections. */
const Json& plugins(const Json& configuration, const fs::path& directory) {
    if (!directory.is_absolute()) {
        invalid("configuration_directory", "expected an absolute directory path");
    }
    if (!configuration.is_object()) {
        invalid("/", "expected a startup configuration mapping");
    }
    return mapping(configuration, "plugins", "/plugins");
}

/** Require a nonempty string and reject embedded NUL before filesystem calls. */
std::string string_value(const Json& value, const std::string& field) {
    if (!value.is_string()) {
        invalid(field, "expected a nonempty string");
    }
    auto result = value.get<std::string>();
    if (result.empty() || result.find('\0') != std::string::npos) {
        invalid(field, "expected a nonempty string without NUL characters");
    }
    return result;
}

/** Resolve only explicit paths; an absent override remains an empty path. */
fs::path resolve(const Json& value, const fs::path& base, const std::string& field) {
    const fs::path path = string_value(value, field);
    return path.is_absolute() ? path : base / path;
}

/** Read directories in precedence order; empty means domain-specific defaults. */
std::vector<fs::path> directories(
    const Json& section, const fs::path& base, const std::string& field) {
    std::vector<fs::path> result;
    const auto entry = section.find("directories");
    if (entry == section.end()) {
        return result;
    }
    if (!entry->is_array()) {
        invalid(field + "/directories", "expected a sequence");
    }
    for (std::size_t index = 0; index < entry->size(); ++index) {
        auto path = resolve(
            (*entry)[index], base, field + "/directories/" + std::to_string(index));
        if (std::find(result.begin(), result.end(), path) == result.end()) {
            result.push_back(std::move(path));
        }
    }
    return result;
}

/** Match the portable descriptor names accepted by both extension domains. */
bool portable_name(std::string_view name) {
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '_' || character == '-';
    });
}

/** Validate one entire enable list before any module or factory is entered. */
ExtensionPlan extension_plan(
    const Json& section, const fs::path& base,
    const std::string& field, const char* configuration_key) {
    ExtensionPlan result;
    result.directories = directories(section, base, field);
    const auto entry = section.find("enable");
    if (entry == section.end()) {
        return result;
    }
    if (!entry->is_array()) {
        invalid(field + "/enable", "expected a sequence");
    }
    std::unordered_set<std::string> names;
    for (std::size_t index = 0; index < entry->size(); ++index) {
        const auto location = field + "/enable/" + std::to_string(index);
        const auto& item = (*entry)[index];
        if (!item.is_object()) {
            invalid(location, "expected a mapping");
        }
        const auto name = item.find("name");
        if (name == item.end()) {
            invalid(location + "/name", "required field is missing");
        }
        Selection selection;
        selection.name = string_value(*name, location + "/name");
        selection.field = location;
        if (!portable_name(selection.name)) {
            invalid(location + "/name", "expected ASCII letters, digits, '_' or '-'");
        }
        if (!names.insert(selection.name).second) {
            invalid(location + "/name", "duplicate selected plugin");
        }
        const auto path = item.find(configuration_key);
        if (path != item.end()) {
            selection.configuration = resolve(
                *path, base, location + "/" + configuration_key);
        }
        result.selections.push_back(std::move(selection));
    }
    return result;
}

/** Discover all providers without consulting model configurations or credentials. */
llm::LLMDispatcher discover_providers(const std::vector<fs::path>& paths) {
    llm::LLMDispatcher result;
    if (paths.empty()) {
        result.load_default_models();
    } else {
        for (const auto& path : paths) {
            result.load_models(path);
        }
    }
    return result;
}

/** Share selection mechanics while keeping admission/factory checks domain-owned. */
template<typename Loader, typename Products>
void construct_selected(const ExtensionPlan& plan, Products& products) {
    if (plan.selections.empty()) {
        return;
    }
    Loader loader;
    if (plan.directories.empty()) {
        loader.load_default();
    } else {
        for (const auto& directory : plan.directories) {
            loader.load(directory);
        }
    }
    products.reserve(plan.selections.size());
    for (const auto& selection : plan.selections) {
        auto instance = loader.create(selection.name, selection.configuration);
        if (!instance) {
            invalid(selection.field, "cannot create selected plugin '" + selection.name + "'");
        }
        products.push_back(std::move(instance));
    }
}

/** Parse both extension domains before their discovery or configuration IO. */
std::pair<ExtensionPlan, ExtensionPlan> extension_plans(
    const Json& plugin_config, const fs::path& directory) {
    const auto& section = mapping(plugin_config, "extensions", "/plugins/extensions");
    const auto& tools = mapping(section, "tools", "/plugins/extensions/tools");
    const auto& hooks = mapping(section, "loop_hooks", "/plugins/extensions/loop_hooks");
    return {
        extension_plan(tools, directory, "/plugins/extensions/tools", "schema_directory"),
        extension_plan(hooks, directory, "/plugins/extensions/loop_hooks", "config_file")
    };
}

/** Keep partial construction local so failures cannot publish a partial bundle. */
LoadedExtensions construct_extensions(
    const ExtensionPlan& tools, const ExtensionPlan& hooks) {
    LoadedExtensions result;
    construct_selected<tools::extensions::ToolSetExtensionLoader>(tools, result.tools);
    construct_selected<loop::extensions::LoopHookExtensionLoader>(hooks, result.loop_hooks);
    return result;
}

} // namespace

PluginLoadError::~PluginLoadError() = default;

llm::LLMDispatcher load_providers(
    const Json& configuration, const fs::path& configuration_directory) {
    const auto& root = plugins(configuration, configuration_directory);
    const auto& providers = mapping(root, "providers", "/plugins/providers");
    return discover_providers(
        directories(providers, configuration_directory, "/plugins/providers"));
}

LoadedExtensions load_extensions(
    const Json& configuration, const fs::path& configuration_directory) {
    const auto& root = plugins(configuration, configuration_directory);
    const auto [tools, hooks] = extension_plans(root, configuration_directory);
    return construct_extensions(tools, hooks);
}

LoadedPlugins load_plugins(const fs::path& configuration_file) {
    const auto file = fs::absolute(configuration_file);
    try {
        const auto configuration = yamlconfig::load_file(file);
        const auto directory = file.parent_path();
        const auto& root = plugins(configuration, directory);
        const auto& providers = mapping(root, "providers", "/plugins/providers");
        const auto paths = directories(providers, directory, "/plugins/providers");
        const auto [tools, hooks] = extension_plans(root, directory);

        LoadedPlugins result;
        result.providers = discover_providers(paths);
        result.extensions = construct_extensions(tools, hooks);
        return result;
    } catch (const std::exception& error) {
        throw PluginLoadError(file.string() + ": " + error.what());
    }
}

} // namespace load
