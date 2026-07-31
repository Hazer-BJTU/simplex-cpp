#pragma once

/**
 * @file plugin/dispatcher.hpp
 * @brief Policy-driven, priority-ordered plugin dispatch.
 *
 * `PluginDispatcher` is the *dispatch* half of the framework: given a request
 * key, it selects the plugin that should handle it. Unlike a hardcoded routing
 * table, the *matching strategy is injected*: a `Selector` policy decides
 * whether a given plugin (via its attached `Payload`) claims a key.
 *
 *   Selector  : `bool(const Payload&, const Key&)`   — does this entry match?
 *   Payload   : arbitrary per-plugin data the domain attaches at registration
 *               (e.g. a compiled std::regex for languages, a command-name
 *               string for a command registry).
 *
 * Selection walks entries in `(priority DESC, registration-order ASC)` order
 * and returns the first match — so a high-priority dedicated plugin always
 * wins over a low-priority catch-all, with deterministic tie-breaking. This is
 * a strict generalization of the old `LangPluginManager::_match_plugin` (which
 * was hardwired to a std::regex over a filename): the same dispatcher serves
 * regex-file routing, exact-name lookup, or any custom predicate.
 *
 * ## Ordering model
 *
 * On every `add()` the dispatcher marks itself dirty. The first query lazily
 * stable-sorts by `(priority DESC, order ASC)`; `flush()` forces the sort
 * eagerly. Callers that drive queries from a `noexcept` context (where a
 * throwing stable_sort would terminate) should `flush()` once after a batch of
 * `add()`s — the subsequent queries then never sort. `indextools` does exactly
 * this after loading a plugin directory.
 */

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "plugin/plugin.hpp"

namespace plugin {

/**
 * @brief Priority-ordered, policy-driven plugin router.
 *
 * @tparam Product  The product interface plugins produce.
 * @tparam Key      The request key dispatch selects on (e.g. a file path, a
 *                  command name).
 * @tparam Selector A default-constructible callable
 *                  `bool(const Payload&, const Key&)` returning true when the
 *                  entry claims the key.
 * @tparam Payload  Per-plugin data the Selector reads (default `std::monostate`
 *                  when the Selector needs no per-plugin data).
 */
template <PluginProduct Product, typename Key, typename Selector,
          typename Payload = std::monostate>
class PluginDispatcher {
public:
    /// Pointer-to-plugin stored per entry.
    using PluginPtr = std::shared_ptr<Plugin<Product>>;

    /**
     * @brief One routed plugin: its descriptor, the Selector's per-plugin data,
     *        its priority, and its registration order (for stable tie-breaking).
     *
     * Public so a domain that attached a payload (e.g. the owning library) can
     * read it back from the matched entry via `match()`.
     */
    struct Entry {
        PluginPtr plugin;
        Payload payload{};
        int priority = 0;
        std::size_t order = 0;
    };

    /**
     * @brief Register a plugin for dispatch.
     *
     * Priority is read from `plugin->priority()` and cached on the entry, so a
     * plugin's priority is fixed at the moment it is added (plugins are
     * stateless descriptors, so this is the expected lifetime).
     *
     * @param plugin  The plugin (null is rejected with a warning).
     * @param payload Per-plugin data for the Selector (default-constructed if
     *                omitted).
     * @return true if added; false if @p plugin was null.
     */
    bool add(PluginPtr plugin, Payload payload = {}) {
        if (!plugin) {
            return false;
        }
        Entry entry;
        entry.plugin = std::move(plugin);
        entry.payload = std::move(payload);
        entry.priority = entry.plugin->priority();
        entry.order = _next_order++;
        _entries.push_back(std::move(entry));
        _dirty = true;
        return true;
    }

    /// Remove all entries (clears routing state).
    void clear() noexcept {
        _entries.clear();
        _dirty = false;
    }

    /// Force the priority ordering to be computed now (no-op if already sorted).
    void flush() const {
        _ensure_sorted();
    }

    /// Number of registered entries.
    std::size_t size() const noexcept { return _entries.size(); }

    /// Whether any entries are registered.
    bool empty() const noexcept { return _entries.empty(); }

    /**
     * @brief The first matching entry (in dispatch order) for @p key, or nullptr.
     *
     * Exposes the full `Entry` so a domain can read back the payload it attached
     * (e.g. the owning library handle needed to build a lifetime-safe instance).
     */
    const Entry* match(const Key& key) const {
        _ensure_sorted();
        for (const auto& entry : _entries) {
            if (_selector(entry.payload, key)) {
                return &entry;
            }
        }
        return nullptr;
    }

    /// The first matching plugin for @p key, or nullptr (convenience over match()).
    PluginPtr select(const Key& key) const {
        const Entry* entry = match(key);
        return entry ? entry->plugin : nullptr;
    }

    /// Whether any registered entry claims @p key.
    bool any_match(const Key& key) const { return match(key) != nullptr; }

    /// Find a plugin by `name()` (linear; use PluginRegistry for O(1) by-name).
    PluginPtr find(std::string_view name) const {
        for (const auto& entry : _entries) {
            if (entry.plugin && entry.plugin->name() == name) {
                return entry.plugin;
            }
        }
        return nullptr;
    }

private:
    // Stable sort by priority DESC, then registration order ASC. Marked const so
    // the const query methods (select/match/any_match) can trigger the lazy sort.
    void _ensure_sorted() const {
        if (!_dirty) {
            return;
        }
        std::stable_sort(_entries.begin(), _entries.end(),
                         [](const Entry& a, const Entry& b) {
                             if (a.priority != b.priority) {
                                 return a.priority > b.priority;
                             }
                             return a.order < b.order;
                         });
        _dirty = false;
    }

    Selector _selector{};
    // Mutable so lazy reordering can happen inside the const query surface.
    mutable std::vector<Entry> _entries;
    mutable bool _dirty = false;
    std::size_t _next_order = 0;
};

} // namespace plugin
