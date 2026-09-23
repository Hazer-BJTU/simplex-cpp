#pragma once

#include "loop/hook_interface.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace loop::intrinsic {

/**
 * Runtime declaration of one built-in hook.
 *
 * A file contains name, description and config. The common loader checks the
 * document's shape; the concrete hook must validate every option it consumes
 * before the host registers it with LoopHookRegistry. These values configure
 * an instance at construction, not while a callback is running.
 */
struct HookConfig {
    std::string name;
    std::string description;
    nlohmann::json config;
};

/** Invalid or unreadable hook YAML, with the file and field in what(). */
class HookConfigError : public std::runtime_error {
public:
    explicit HookConfigError(const std::string& message);
    ~HookConfigError() override;
};

/**
 * Load one hook's config.yaml. Unknown top-level fields are rejected.
 *
 * expected_name binds the document to its compiled hook package. It must
 * match the YAML name, so editing YAML cannot silently rename the registry
 * entry or make a package impersonate another hook. The config object remains
 * available for the concrete hook's own strict, typed validation.
 */
[[nodiscard]] HookConfig load_hook_config(
    const std::filesystem::path& file,
    std::string_view expected_name);

/** Log a rejected document and return nullopt, leaving the hook unregistered. */
[[nodiscard]] std::optional<HookConfig> try_load_hook_config(
    const std::filesystem::path& file,
    std::string_view expected_name);

/**
 * Load and construct one hook, logging and skipping either kind of failure.
 *
 * The factory performs its concrete option validation in the hook constructor.
 * The resulting instance is not subscribed; the host adds it to a registry
 * only when this function returns a non-null pointer. A null factory result or
 * mismatched hook identity is also rejected.
 */
using HookFactory = std::function<std::shared_ptr<LoopHookInterface>(HookConfig)>;

[[nodiscard]] std::shared_ptr<LoopHookInterface> try_create_hook(
    const std::filesystem::path& file,
    std::string_view expected_name,
    const HookFactory& factory);

/**
 * Locate a built-in hook's config.yaml without embedding its contents.
 *
 * Lookup order: nonempty SIMPLEX_LOOP_HOOK_SCHEMA_DIR/<hook>/config.yaml,
 * then <executable_dir>/schemas/loop/<hook>/config.yaml when that directory
 * exists, then <source_root>/hooks/<hook>/schemas/config.yaml for development.
 * An explicit environment override is used even if missing, so a deployment
 * typo is reported by the loader instead of silently falling back.
 *
 * hook_name must be one portable path segment (ASCII letters, digits, '_' or
 * '-'); anything else is rejected before constructing a filesystem path.
 */
[[nodiscard]] std::filesystem::path hook_config_file(
    std::string_view hook_name);

/**
 * Shared implementation of a built-in hook's YAML-backed identity.
 *
 * Derived constructors validate config().config before returning. Once built,
 * the host adds the shared instance to LoopHookRegistry, which calls its
 * subscribe() method. This order keeps malformed configuration off the bus.
 * Runtime config changes take effect when a new instance is constructed; no
 * hot reload or mutation of a live callback's state is implied.
 */
class IntrinsicLoopHook : public LoopHookInterface {
public:
    explicit IntrinsicLoopHook(HookConfig config);
    ~IntrinsicLoopHook() override;

    [[nodiscard]] std::string_view name() const noexcept final;
    [[nodiscard]] const HookConfig& config() const noexcept;

private:
    HookConfig config_;
};

} // namespace loop::intrinsic
