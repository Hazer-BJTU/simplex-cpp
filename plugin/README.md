# Generic Plugin Framework

A reusable, domain-agnostic plugin framework shared across the project. It factors
the plugin machinery that previously lived only inside `indextools` (the language
plugin system) into four template components, each mapping to one concern:

| Component | Concern | Header |
|-----------|---------|--------|
| `plugin::Plugin<Product>`        | base interface (abi/name/priority/create) | `plugin/plugin.hpp` |
| `plugin::PluginRegistry<Product>`| **registration** — by-name register/find | `plugin/registry.hpp` |
| `plugin::PluginDispatcher<Product,Key,Selector,Payload>` | **dispatch** — policy-driven routing | `plugin/dispatcher.hpp` |
| `plugin::PluginManager<Product>` | **management** — load + ABI gate + lifetime | `plugin/manager.hpp` |

All are header-only templates in `namespace plugin`; link the `plugin_iface`
INTERFACE target to use them.

## Design pattern

Strategy + registry + policy-based dispatch, parameterized by a **product**
interface (`Product`) and, for dispatch, by a **key**, a **selector policy**, and
per-plugin **payload**:

```
plugin::Plugin<Product>                 // what every plugin is
plugin::PluginManager<Product>          // how a host loads .so plugins
plugin::PluginDispatcher<P, Key, Selector, Payload>   // how a key routes to a plugin
plugin::PluginRegistry<Product>         // how a host looks a plugin up by name
```

Dispatch is generic because the **matching strategy is injected**: `Selector` is a
callable `bool(const Payload&, const Key&)` and `Payload` is arbitrary per-plugin
data. The same dispatcher serves regex-over-filename routing (languages),
exact-name lookup (a future command registry), or any custom predicate. Selection
walks entries in `(priority DESC, registration-order ASC)` and returns the first
match — a high-priority dedicated plugin always beats a low-priority catch-all.

## Composing a domain (example: languages)

A domain specializes the base with its own product type and adds whatever routing
metadata it needs, then composes the three collaborators:

```cpp
// Domain base interface
class LangPlugin : public plugin::Plugin<LangAnalyze> {
    virtual std::string_view file_pattern() const noexcept = 0; // domain routing
};

// Host
class LangPluginManager {
    plugin::PluginManager<LangAnalyze> _loader;
    plugin::PluginDispatcher<LangAnalyze, std::filesystem::path,
                             LangFileSelector, LangFilePayload> _router;
    plugin::PluginRegistry<LangAnalyze> _registry;
};
```

## Authoring a plugin

Subclass the domain base, define a factory returning `shared_ptr<DomainPlugin>`,
and export it with `SIMPLEX_PLUGIN_ALIAS`:

```cpp
#include "plugin/plugin.hpp"
#include <boost/dll/alias.hpp>

std::shared_ptr<MyPlugin> create_my_plugin() { return std::make_shared<MyPlugin>(); }
SIMPLEX_PLUGIN_ALIAS(create_my_plugin, create_my_plugin)
```

## Lifetime safety

A `Product` produced by a loaded plugin has its code/vtable inside the plugin's
`.so`. `PluginManager::create_instance()` captures the owning library into the
returned `shared_ptr<Product>`'s deleter, guaranteeing the library outlives the
product. Callers use ordinary `shared_ptr` RAII and never manage unloading.
