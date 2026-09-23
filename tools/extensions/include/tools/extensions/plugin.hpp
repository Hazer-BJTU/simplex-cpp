#pragma once

#include "extensions/extensions.hpp"
#include "versioning/version.hpp"
#include "tools/toolsets.hpp"
#include "tools/intrinsic/tool_declaration.hpp"
#include "tools/intrinsic/skill_declaration.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tools::extensions {

inline constexpr std::uint32_t kAbiVersion = simplex::TOOLSET_PLUGIN_ABI_VERSION;
inline constexpr std::string_view kContextFactory = "create_toolset_plugin";
inline constexpr std::string_view kToolSetFactory = "create_toolset";

/** Configuration loaded before a plugin's toolset factory is called. */
struct ToolSetConfig {
    std::string name;
    std::string description;
    nlohmann::json config;
    std::filesystem::path schema_directory;
};

/** Domain descriptor exported through create_toolset_plugin(). */
class ToolSetExtensionContext : public extension::ExtensionContext {
public:
    ~ToolSetExtensionContext() override;
};

/** Resolve a plugin's YAML directory beside the executable or under an override. */
[[nodiscard]] std::filesystem::path schema_directory(std::string_view name);

/** Load strict config.yaml, checking the descriptor name and top-level shape. */
[[nodiscard]] ToolSetConfig load_config(const std::filesystem::path& directory,
                                        std::string_view expected_name);

/** Convenience paths and validated loaders for per-tool declarations and skill. */
[[nodiscard]] model_io::Invocable load_tool(const ToolSetConfig& config,
                                            std::string_view tool_name);
[[nodiscard]] std::optional<ToolSetSkill> load_skill(const ToolSetConfig& config);

/** A session-independent loader; registration in ToolRegistry remains explicit. */
class ToolSetExtensionLoader {
public:
    /**
     * Scan one directory, logging and skipping malformed modules.
     * Missing paths and non-directories add nothing; other filesystem status
     * errors and enumeration failures propagate to the caller.
     */
    std::size_t load(const std::filesystem::path& directory);
    /** Scan the executable-relative plugins directory for this domain. */
    std::size_t load_default();
    /**
     * Create a fresh instance; registry membership remains the caller's choice.
     * configuration selects an explicit schema directory.
     * An empty path uses executable-relative discovery or the environment override.
     * Unknown names, invalid configuration, and factory failures return nullptr.
     * Serialize load() with other operations on this loader.
     */
    [[nodiscard]] std::shared_ptr<ToolSet> create(
        std::string_view name,
        const std::filesystem::path& configuration = {}) const;
    [[nodiscard]] std::size_t size() const noexcept {
        return contexts_.size();
    }

private:
    std::vector<std::shared_ptr<ToolSetExtensionContext>> contexts_;
};

} // namespace tools::extensions
