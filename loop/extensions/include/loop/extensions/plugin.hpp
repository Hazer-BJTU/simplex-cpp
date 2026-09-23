#pragma once

#include "extensions/extensions.hpp"
#include "versioning/version.hpp"
#include "loop/hook_interface.hpp"
#include "loop/intrinsic/hook_config.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace loop::extensions {

inline constexpr std::uint32_t kAbiVersion = simplex::LOOP_HOOK_PLUGIN_ABI_VERSION;
inline constexpr std::string_view kContextFactory = "create_loop_hook_plugin";
inline constexpr std::string_view kHookFactory = "create_loop_hook";

/** Domain descriptor exported through create_loop_hook_plugin(). */
class LoopHookExtensionContext : public extension::ExtensionContext {
public:
    ~LoopHookExtensionContext() override;
};

/** Locate YAML independently of the intrinsic hook source-tree fallback. */
[[nodiscard]] std::filesystem::path config_file(std::string_view name);

/** Reuse the intrinsic document contract and require the descriptor's name. */
[[nodiscard]] inline intrinsic::HookConfig load_config(
    const std::filesystem::path& file, std::string_view expected_name) {
    return intrinsic::load_hook_config(file, expected_name);
}

/** Load descriptors, then construct DSO-pinned hooks for explicit registry use. */
class LoopHookExtensionLoader {
public:
    /** Scan one directory; malformed modules are logged and skipped. */
    std::size_t load(const std::filesystem::path& directory);
    /** Scan the executable-relative plugins directory for this domain. */
    std::size_t load_default();
    /**
     * Create a fresh instance; registry membership remains the caller's choice.
     * configuration selects an explicit config.yaml file.
     * An empty path uses executable-relative discovery or the environment override.
     * Unknown names, invalid configuration, and factory failures return nullptr.
     * Serialize load() with other operations on this loader.
     */
    [[nodiscard]] std::shared_ptr<LoopHookInterface> create(
        std::string_view name,
        const std::filesystem::path& configuration = {}) const;
    [[nodiscard]] std::size_t size() const noexcept {
        return contexts_.size();
    }

private:
    std::vector<std::shared_ptr<LoopHookExtensionContext>> contexts_;
};

} // namespace loop::extensions
