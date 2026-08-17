# extension_framework

A generic, **domain-agnostic** dynamic-library extension system. It discovers,
loads, verifies, routes, and creates objects from `.so`/`.dll`/`.dylib` plugin
modules — knowing nothing about *what* those plugins are (languages, commands,
greeters, …). A new domain plugs in by including one header and supplying two
callables.

It is **header-only**: the entire framework lives in
[`include/extension_framework/extensions.hpp`](include/extension_framework/extensions.hpp).
It depends on Boost.DLL (loading), nlohmann/json (`extras()`), and the project
logger (`verify_after_loaded` diagnostics). Consumers link the
`extension_framework_iface` INTERFACE target.

> This framework supersedes the old `plugin::` framework. The `indextools`
> language layer is a full real-world example of it — see
> [`indextools/include/indextools/lang_plugin.hpp`](../indextools/include/indextools/lang_plugin.hpp)
> and [`LangDispatcher`](../indextools/include/indextools/cache_system.hpp).

---

## Concepts

| Term | Meaning |
|------|---------|
| **ExtensionContext** | The abstract *descriptor* base every plugin implements: identity (`name()`, `abi_version()`), ordering (`priority()`), and the owning library handle once bound. Lives in the plugin `.so`. |
| **Product** | *(optional)* The per-use object a descriptor mints (e.g. an analyzer, a service). Also lives in the `.so`. A domain with no per-use objects skips this. |
| **Dispatcher** | `ExtensionDispatcher`: loads a directory of plugins, keeps a name→context index, and routes a request key to the best-matching context. Host-owned. |
| **ABI version** | A `std::uint32_t` the descriptor reports; the **domain** (not the framework) rejects mismatched plugins at load. Bump it on any binary-incompatible change. |

### The two-export contract

Every plugin `.so` exports factory aliases via `BOOST_DLL_ALIAS`:

1. **Descriptor factory** — `std::unique_ptr<extension::ExtensionContext> create_xxx()`.
   Mints the long-lived descriptor. Returned as the base `ExtensionContext` so the
   generic loader needs no domain-specific type.
2. **Product factory** *(optional)* — `std::unique_ptr<Product> create_xxx_product()`.
   Mints a fresh per-use object. Only needed if the domain has a `Product`.

### Lifetime (the load-bearing invariant)

An object produced by a dynamic library has its vtable and destructor *inside*
that library, so the library must stay mapped for the object's whole life —
including its destruction. The framework handles this two ways (see
`create_object_from_library`'s `bind_library_ref_deleter` flag):

- **Deleter-pinned (default for products)** — the `shared_ptr`'s deleter captures
  the library handle, releasing it only *after* `delete p` returns. Self-contained
  and safe; the object outlives its loader.
- **External ownership** — the object does not pin the library; the caller must
  keep a handle alive separately. Use only with a long-lived owner.

---

## Part 1 — Creating a plugin category (domain author)

A "category" is your domain's contract: a concrete `ExtensionContext` subclass,
the (optional) `Product` type, an ABI version, the factory-alias names, and the
host-side loading/routing. This is a header shared by the host and every plugin.

### 1.1 The contract header

```cpp
// mydomain/my_plugin.hpp — compiled into BOTH the host and every plugin .so
#include <cstdint>
#include <memory>
#include <string_view>
#include "extension_framework/extensions.hpp"

namespace mydomain {

/// Bump on any binary-incompatible change to MyPlugin / MyProduct.
inline constexpr std::uint32_t MY_ABI_VERSION = 1;

/// The well-known alias every descriptor factory is exported under.
inline constexpr const char* MY_PLUGIN_FACTORY  = "create_my_plugin";
/// The well-known alias every product factory is exported under (if you have one).
inline constexpr const char* MY_PRODUCT_FACTORY = "create_my_product";

/// Descriptor base for this domain. Adds whatever routing metadata you need.
class MyPlugin : public extension::ExtensionContext {
public:
    // inherited pure-virtuals the concrete plugin MUST implement:
    //   std::uint32_t abi_version() const noexcept;
    //   std::string_view name()       const noexcept;
    // inherited optional override:
    //   long priority() const noexcept;   // higher wins; default 0

    /// Domain-specific routing metadata — e.g. which keys this plugin claims.
    virtual std::string_view pattern() const noexcept = 0;
};

/// (Optional) the per-use object a plugin mints.
class MyProduct {
public:
    virtual ~MyProduct() = default;
    virtual std::string do_work() const = 0;
};

} // namespace mydomain
```

### 1.2 The host: load, ABI-gate, route, create

`ExtensionDispatcher` does the generic loading + indexing + dispatch. The domain
adds the two things that are domain-specific: **which key matches which plugin**
(a `Matcher`) and, if you load raw, an **ABI gate**. A small subclass is the
cleanest place for both:

```cpp
#include "mydomain/my_plugin.hpp"
#include "extension_framework/extensions.hpp"

class MyDispatcher : public extension::ExtensionDispatcher {
public:
    MyDispatcher() {
        // Route by whatever your key means. The matcher downcasts to reach the
        // domain-specific state — the framework stays generic.
        set_matcher([](const ContextPtr& ctx, std::string_view key) -> bool {
            auto p = std::dynamic_pointer_cast<mydomain::MyPlugin>(ctx);
            return p && /* does p claim `key`? */ ;
        });
    }

    /// Load + ABI-gate. Call once, single-threaded, at startup.
    std::size_t load(const std::filesystem::path& dir) {
        try {
            load_directory(dir, extension::is_likely_dynamic_library,
                           extension::same_tag_always{mydomain::MY_PLUGIN_FACTORY});
        } catch (const std::exception&) { return 0; } // missing dir → no plugins
        std::size_t good = 0;
        for (const auto& ctx : contexts()) {
            // The framework does NOT enforce ABI — the domain does, here.
            if (!ctx || ctx->abi_version() != mydomain::MY_ABI_VERSION) continue;
            if (std::dynamic_pointer_cast<mydomain::MyPlugin>(ctx)) ++good;
        }
        return good;
    }

    /// Route a key to a plugin and mint a fresh product.
    std::shared_ptr<mydomain::MyProduct> make(const std::string& key) const {
        auto ctx = dispatch(key);
        auto p = std::dynamic_pointer_cast<mydomain::MyPlugin>(ctx);
        return p ? extension::create_product<mydomain::MyProduct>(p, mydomain::MY_PRODUCT_FACTORY)
                 : nullptr;
    }
};
```

That is the whole host side. `dispatch(key)` returns `nullptr` when nothing
matches (a normal "unsupported" outcome, not an error); selection defaults to
highest-`priority()`-first, overridable with `set_selector(...)`.

### 1.3 CMake

The contract header needs only `extension_framework_iface` (which transitively
brings Boost.DLL, nlohmann/json, the logger):

```cmake
add_library(mydomain_iface INTERFACE)
target_include_directories(mydomain_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mydomain_iface INTERFACE extension_framework_iface)
```

Plugins link `mydomain_iface` (PRIVATE) + `boost_dll_iface`; the host links it too.

---

## Part 2 — Implementing a simple plugin

A plugin is one translation unit that subclasses the domain descriptor,
implements the factories, and exports them. Using the `mydomain` contract above:

```cpp
// my_fancy_plugin.cpp — builds into libmyfancyplugin.so
#include "mydomain/my_plugin.hpp"
#include <boost/dll/alias.hpp>
#include <memory>

namespace mydomain {
namespace {

class FancyPlugin final : public MyPlugin {
public:
    std::uint32_t abi_version() const noexcept override { return MY_ABI_VERSION; }
    std::string_view name()       const noexcept override { return "Fancy"; }
    std::string_view pattern()    const noexcept override { return "fancy"; }
    // priority() not overridden → default 0. A catch-all would return a low value.
};

class FancyProduct final : public MyProduct {
public:
    std::string do_work() const override { return "hello from fancy"; }
};

} // namespace

// 1) Descriptor factory — returns the base ExtensionContext.
std::unique_ptr<extension::ExtensionContext> create_my_plugin() {
    return std::make_unique<FancyPlugin>();
}

// 2) Product factory — mints a fresh per-use object.
std::unique_ptr<MyProduct> create_my_product() {
    return std::make_unique<FancyProduct>();
}

} // namespace mydomain

BOOST_DLL_ALIAS(mydomain::create_my_plugin,  create_my_plugin)
BOOST_DLL_ALIAS(mydomain::create_my_product, create_my_product)
```

> **Return types matter.** The descriptor factory must return exactly
> `std::unique_ptr<extension::ExtensionContext>` (a `unique_ptr<MyPlugin>` will
> *not* match the framework's import signature). The product factory returns
> `std::unique_ptr<Product>`.

### CMake — emit the `.so`

```cmake
add_library(myfancyplugin MODULE my_fancy_plugin.cpp)
target_link_libraries(myfancyplugin PRIVATE mydomain_iface boost_dll_iface)
set_target_properties(myfancyplugin PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
```

Drop the `.so` into the directory your dispatcher scans, and the host picks it up
on the next `load()`. The minimal end-to-end example in this module —
[`test/toy_extension_spec.hpp`](test/toy_extension_spec.hpp) +
[`test/toy_extension.cpp`](test/toy_extension.cpp) — is exactly this shape, and
[`test/test_extensions_dynamic.cpp`](test/test_extensions_dynamic.cpp) loads and
exercises it through the real pipeline.

---

## Part 3 — Advanced: caching the factory (hot paths)

`create_product<Product>(context, alias)` is the simple path: it resolves the
factory alias (`has()` + the symbol lookup Boost.DLL does internally) **on every
call**. For a one-shot that is fine. On a hot path that mints many products from
the same plugin — especially concurrently — that per-call symbol resolution is
wasted work.

`product_factory<Product>` pays the resolution **once**, then invokes a cached
function pointer per create:

```cpp
extension::product_factory<mydomain::MyProduct> factory{
    plugin->get_library_ref(), mydomain::MY_PRODUCT_FACTORY};

for (/* each request */) {
    auto product = factory.create();   // one indirect call, no symbol resolution
}
```

### Where to build it

Resolve it **once, at load time**, and store it on the descriptor — then every
`create()` is a pure read. The language domain does exactly this in
`LangPlugin::warm()`:

```cpp
class MyPlugin : public extension::ExtensionContext {
    std::shared_ptr<extension::product_factory<MyProduct>> _factory;
    bool _ready = false;
public:
    // Called once, single-threaded, right after the dispatcher loads + binds.
    bool warm() noexcept {
        try {
            _factory = std::make_shared<extension::product_factory<MyProduct>>(
                get_library_ref(), MY_PRODUCT_FACTORY);
        } catch (...) { return false; }
        return _ready = true;
    }
    std::shared_ptr<MyProduct> create_product() const noexcept {
        return (_ready && _factory) ? _factory->create() : nullptr;
    }
};
```

### Why it is safe and fast

- **No re-resolution.** `product_factory`'s constructor resolves the raw function
  pointer once (the same lookup `import_alias` does internally, minus Boost's
  wrapper); `create()` is a single indirect call.
- **Self-contained products.** The default mode (`bind_library_ref_deleter = true`)
  makes each product deleter-pinned: its `shared_ptr` carries the library handle,
  so the product is safely destructible **even after the dispatcher is gone**. No
  process-wide singleton is needed to keep libraries mapped.
- **Thread-safe by construction.** Resolve in one single-threaded load pass
  (`warm()`); afterwards the cached pointer + library ref are never mutated, so
  concurrent `create()` calls are lock-free reads. (`product_factory::create()`
  is `const` and has no shared mutable state.)

If you can guarantee the factory outlives every product it mints, the external-
ownership mode (`product_factory<Product, false>`) drops the per-product deleter
allocation entirely — products become plain `shared_ptr`s. Use the default
(`true`) unless you have measured the allocation and can uphold the lifetime
contract.

---

## API summary

All in `namespace extension`, in [`extensions.hpp`](include/extension_framework/extensions.hpp).

| Entity | Purpose |
|--------|---------|
| `ExtensionContext` | Abstract descriptor base (`abi_version`/`name`/`priority`/`extras`/`bind`). |
| `is_likely_dynamic_library(path)` | Filename predicate for directory scans. |
| `same_tag_always` | Tag policy: every file imports the same alias. |
| `get_library_ref(path)` | Open a `.so` into a shared handle. |
| `create_object_from_library<T,bind>(lib, alias)` | One-shot: import a factory alias, mint one object. |
| `load_modules_directory(...)` | Scan a dir, load each accepted module (per-file resilient). |
| `verify_after_loaded(loaded, errors)` | Drop failures, log diagnostics, stable-sort by priority. |
| `load_and_verify_directory(...)` | One-shot compose of the two above. |
| `create_product<T,bind>(context, alias)` | One-shot product from a context's bound library. |
| `product_factory<T,bind>` | **Cached** factory: resolve once (`ctor`), mint many (`create()`). |
| `ExtensionDispatcher` | Stateful registry + key router; `load_directory` / `find` / `dispatch` / `set_matcher` / `set_selector`, overridable `import_directory`. |

### Tests & examples

- [`test/test_extensions.cpp`](test/test_extensions.cpp) — pure-logic coverage
  (predicate, verify sort, dispatcher, error paths).
- [`test/test_extensions_dynamic.cpp`](test/test_extensions_dynamic.cpp) — drives
  the full pipeline over a real toy `.so`.
- The `indextools` language layer (`LangPlugin`, `LangDispatcher`, the
  `python`/`fallback` plugins) — the production instance of every pattern above,
  including ABI gating and `product_factory` caching.
