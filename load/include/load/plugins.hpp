#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm/models.hpp"
#include "loop/hook_interface.hpp"
#include "tools/toolsets.hpp"

namespace load {

/** Invalid startup plugin configuration or an unavailable requested extension. */
class PluginLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    ~PluginLoadError() override;
};

/**
 * Configured dynamic instances in their respective enable-list order.
 *
 * Instances retain their native modules independently of the temporary loaders.
 * Hooks are not subscribed and toolsets are not registered. The host transfers
 * them into session registries at a serialized initialization boundary. This is
 * a runtime ownership bundle, not a serializable configuration or session record.
 */
struct LoadedExtensions {
    std::vector<std::shared_ptr<tools::ToolSet>> tools;
    std::vector<std::shared_ptr<loop::LoopHookInterface>> loop_hooks;
};

/** Startup-owned descriptors and configured extensions; no live connections. */
struct LoadedPlugins {
    llm::LLMDispatcher providers;
    LoadedExtensions extensions;
};

/**
 * Discover every compatible provider in plugins.providers.directories.
 *
 * @param configuration Complete startup JSON mapping, usually converted from
 * YAML. Only the provider discovery section is consumed. In particular, the
 * top-level providers and driver_model fields do not filter plugin admission.
 * @param configuration_directory Base for explicit relative directory paths.
 * Must be absolute, making resolution independent of the current directory.
 *
 * Omitted/empty directories use plugins/llm beside the executable. Unknown
 * fields are ignored; malformed known fields throw PluginLoadError. Native
 * admission follows LLMDispatcher's existing checks and diagnostic policy.
 * No model is constructed and no credentials or network services are accessed.
 * Call during serialized startup, before sharing the returned dispatcher.
 */
[[nodiscard]] llm::LLMDispatcher load_providers(
    const nlohmann::json& configuration,
    const std::filesystem::path& configuration_directory);

/**
 * Construct only the dynamic toolsets and hooks named by their enable lists.
 *
 * @param configuration Complete startup JSON mapping. Only plugins.extensions
 * is consumed; intrinsic components are outside this function's responsibility.
 * @param configuration_directory Absolute base for directory and schema paths.
 *
 * Both lists are validated before discovery. Empty lists skip their directory
 * scans entirely. Nonempty lists discover descriptors using the existing domain
 * loaders, then read configuration and invoke factories only for selected names.
 * Discovery can open unselected native modules to obtain exported names; it is
 * not a guarantee against executing those modules' initialization code.
 *
 * Duplicate selections, invalid paths/names, or a selected instance that cannot
 * be created throw PluginLoadError. No partial result escapes. Already-created
 * objects are destroyed on failure; native initialization effects cannot be
 * rolled back. Filesystem enumeration failures propagate. No registry or event
 * bus is mutated. Returned instances pin their modules through destruction.
 */
[[nodiscard]] LoadedExtensions load_extensions(
    const nlohmann::json& configuration,
    const std::filesystem::path& configuration_directory);

/**
 * Read one startup YAML file, validate its plugin sections, and load plugins.
 *
 * @param configuration_file File to read. Relative file arguments are resolved
 * against the caller's working directory once; explicit paths inside the file
 * subsequently resolve against its absolute parent directory.
 *
 * All plugin sections are structurally validated before native discovery.
 * Unknown fields and unrelated model/client/storage sections are left alone.
 * YAML and plugin errors include the configuration filename in PluginLoadError.
 * The source file and the process environment are never modified. This function
 * does not initialize intrinsic components, registries, models, IO, or storage.
 */
[[nodiscard]] LoadedPlugins load_plugins(
    const std::filesystem::path& configuration_file);

} // namespace load
