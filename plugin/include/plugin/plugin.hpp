#pragma once

/**
 * @file plugin/plugin.hpp
 * @brief Generic, domain-agnostic plugin base interface.
 *
 * This is the foundation of the project-wide plugin framework (see the sibling
 * registry.hpp / dispatcher.hpp / manager.hpp). It defines the one thing every
 * plugin domain shares: an abstract *descriptor + factory* that knows how to
 * mint instances of some product interface `Product`.
 *
 * ## Why a separate `Plugin<Product>` from `Product`?
 *
 * A `Product` (e.g. `LangAnalyze`, a future `Command`) is a *per-request* object
 * with lots of state. A plugin, by contrast, is a long-lived, stateless
 * descriptor: it knows its own identity (`name()`, `abi_version()`) and how to
 * build fresh `Product` instances (`create()`). Splitting them keeps the plugin
 * object tiny and lets a host query metadata without constructing anything.
 *
 * Concrete plugin domains specialize this template with their own product type
 * and add whatever domain-specific routing metadata they need on a subclass:
 *
 * @code
 *   class LangPlugin : public plugin::Plugin<LangAnalyze> {
 *       // inherited: abi_version() / name() / priority() / create()
 *       virtual std::string_view file_pattern() const noexcept = 0; // domain routing
 *   };
 * @endcode
 *
 * ## ABI versioning
 *
 * `abi_version()` is checked by `plugin::PluginManager` at load time against a
 * domain-supplied expected value, so a plugin built against an incompatible
 * header revision is rejected rather than crashing the host. Each domain owns
 * its own ABI constant (the language domain's lives in `versioning/`); the base
 * interface only mandates that a plugin *report* one.
 *
 * This header deliberately depends on the standard library only — no Boost — so
 * the base contract stays decoupled from the (Boost.DLL) loading mechanism that
 * lives in manager.hpp.
 */

#include <concepts>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

namespace plugin {

/**
 * @brief Concept satisfied by a type usable as a `Plugin` product.
 *
 * The product must be polymorphic AND have a virtual destructor. The virtual
 * destructor is load-bearing: `PluginManager::create_instance()` deletes the
 * product through a base pointer inside a custom deleter, which is undefined
 * behavior without one. Both requirements together give a clear compile-time
 * diagnostic if a domain mistakenly plugs in a non-polymorphic or
 * non-virtually-destructible type.
 */
template <typename T>
concept PluginProduct =
    std::is_polymorphic_v<std::remove_cvref_t<T>> &&
    std::has_virtual_destructor_v<std::remove_cvref_t<T>>;

/**
 * @brief Abstract descriptor + factory for one plugin of product type `Product`.
 *
 * Instances are produced by a plugin's exported factory and owned by a host's
 * `PluginManager`. A plugin is stateless with respect to individual requests;
 * it only knows its identity and how to build fresh `Product` instances.
 *
 * @tparam Product The interface the plugin mints via `create()`. Must satisfy
 *                 `PluginProduct` (polymorphic + virtual destructor).
 */
template <PluginProduct Product>
class Plugin {
public:
    virtual ~Plugin() = default;

    /**
     * @brief ABI version this plugin was built against.
     *
     * Must equal the domain's expected value for the host to accept the plugin.
     * Implementations should `return <DOMAIN_ABI_VERSION>;`.
     */
    virtual std::uint32_t abi_version() const noexcept = 0;

    /// Human-readable plugin name (e.g. "Python", "Fallback"). Used for logging,
    /// diagnostics, and by-name registry lookup; should be unique per domain.
    virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Match priority used to order plugins during dispatch.
     *
     * `PluginDispatcher` evaluates plugins highest-priority-first (ties broken
     * by registration order). A dedicated plugin should keep the default; a
     * broad catch-all should return a low value so it wins only when nothing
     * more specific matches. Overriding is optional.
     */
    virtual int priority() const noexcept { return 0; }

    /**
     * @brief Mint a fresh, empty product instance.
     *
     * The returned object is allocated inside the plugin module (when loaded
     * dynamically). The caller — `PluginManager::create_instance()` — is
     * responsible for binding the owning library's lifetime into the returned
     * `shared_ptr`'s deleter so the module stays mapped for the object's life.
     *
     * @return A newly constructed `Product`, never null on success.
     */
    virtual std::unique_ptr<Product> create() const = 0;
};

} // namespace plugin

/**
 * @def SIMPLEX_PLUGIN_ALIAS(fq_factory, alias_name)
 * @brief Standardized export of a plugin factory symbol.
 *
 * Thin wrapper over `BOOST_DLL_ALIAS` that every dynamically-loaded plugin uses
 * to publish its factory under the well-known name the host imports. Using this
 * macro (rather than `BOOST_DLL_ALIAS` directly) documents intent and keeps the
 * authoring convention uniform across domains.
 *
 * The translation unit using this macro MUST also include
 * `&lt;boost/dll/alias.hpp&gt;` (this header is intentionally Boost-free). It must
 * be placed at namespace scope, not inside a function or class:
 *
 * @code
 *   std::shared_ptr&lt;plugin::Plugin&lt;MyProduct&gt;&gt; create_my_plugin() {
 *       return std::make_shared&lt;MyPlugin&gt;();
 *   }
 *   SIMPLEX_PLUGIN_ALIAS(create_my_plugin, create_my_plugin)
 * @endcode
 */
#define SIMPLEX_PLUGIN_ALIAS(fq_factory, alias_name) \
    BOOST_DLL_ALIAS(fq_factory, alias_name)
