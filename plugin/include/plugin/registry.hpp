#pragma once

/**
 * @file plugin/registry.hpp
 * @brief By-name registry of loaded plugins.
 *
 * `PluginRegistry` is the *registration* half of the framework: an O(1)
 * name → plugin map. It answers "do we have a plugin called X? give me it",
 * which is orthogonal to `PluginDispatcher`'s "given this request key, which
 * plugin handles it?" A domain typically populates both from the same load
 * (see how `indextools::LangPluginManager` feeds router and registry together).
 *
 * The registry does not own plugin lifetimes — it holds `shared_ptr`s, so a
 * plugin stays alive as long as the registry (and any handed-out references)
 * hold it. Names come from `Plugin::name()`; registering a duplicate name is
 * rejected (returns false, logs a warning) so a domain never silently gets two
 * claimants under one name.
 *
 * This is one of three collaborators a host composes:
 *   - `PluginManager<Product>`   — load + lifetime (manager.hpp)
 *   - `PluginDispatcher<...>`    — request-keyed dispatch (dispatcher.hpp)
 *   - `PluginRegistry<Product>`  — by-name lookup (this file)
 */

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plugin/plugin.hpp"

namespace plugin {

/**
 * @brief A map from plugin name to plugin descriptor.
 *
 * @tparam Product The product interface (same as the `Plugin<Product>` it stores).
 */
template <PluginProduct Product>
class PluginRegistry {
public:
    /// Pointer-to-plugin stored and handed out by the registry.
    using PluginPtr = std::shared_ptr<Plugin<Product>>;

    /**
     * @brief Register a plugin under its `name()`.
     *
     * @return true if registered; false (with a warning to std::cerr) if a plugin
     *         with the same name is already registered or @p plugin is null.
     */
    bool register_plugin(PluginPtr plugin) {
        if (!plugin) {
            std::cerr << "[plugin] registry: ignoring null plugin\n";
            return false;
        }
        const std::string key(plugin->name());
        if (_by_name.find(key) != _by_name.end()) {
            std::cerr << "[plugin] registry: duplicate name '" << key
                      << "' — ignoring later registration\n";
            return false;
        }
        _by_name.emplace(std::move(key), std::move(plugin));
        return true;
    }

    /// Look up a plugin by name, or nullptr if none is registered under @p name.
    PluginPtr find(std::string_view name) const {
        auto it = _by_name.find(std::string(name));
        return it == _by_name.end() ? nullptr : it->second;
    }

    /// Whether a plugin is registered under @p name.
    bool contains(std::string_view name) const {
        return _by_name.find(std::string(name)) != _by_name.end();
    }

    /// Number of registered plugins.
    std::size_t size() const noexcept { return _by_name.size(); }

    /// Whether the registry is empty.
    bool empty() const noexcept { return _by_name.empty(); }

    /// Snapshot of all registered plugins (unordered).
    std::vector<PluginPtr> all() const {
        std::vector<PluginPtr> out;
        out.reserve(_by_name.size());
        for (const auto& [_, plugin] : _by_name) {
            out.push_back(plugin);
        }
        return out;
    }

private:
    std::unordered_map<std::string, PluginPtr> _by_name;
};

} // namespace plugin
