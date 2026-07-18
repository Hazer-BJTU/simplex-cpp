# Language Support (Language Plugins)

Every language under this directory is a **self-contained plugin module**. The
host (`indextools`) loads these `.so` files at runtime from
`bin/plugins/` via Boost.DLL, rather than statically linking them into the
binary at compile time.

Design goal: **adding a new language only requires creating a new module
subdirectory here — no configuration above it needs to change** (the top-level
`CMakeLists.txt`, `languages/CMakeLists.txt`, and `test/` are all left
untouched).

---

## Directory layout

```
languages/
├── CMakeLists.txt        # auto-discovers subdirectories; no per-language config
├── README.md             # this file
├── python/               # one language = one module
│   ├── CMakeLists.txt     # all build config for this language (self-contained)
│   ├── include/
│   │   └── pythonlang.hpp
│   └── src/
│       ├── pythonlang.cpp          # analyzer implementation (LangAnalyze subclass)
│       └── pythonlang_plugin.cpp   # plugin export wrapper (LangPlugin subclass + export)
└── fallback/             # generic catch-all analyzer, same structure
    ├── CMakeLists.txt
    ├── include/fallbacklang.hpp
    └── src/{fallbacklang.cpp, fallbacklang_plugin.cpp}
```

`languages/CMakeLists.txt` walks this directory: any **subdirectory that
contains a `CMakeLists.txt`** is treated as a language module and
`add_subdirectory()`'d automatically. So dropping a module in gets it built.

---

## What a module consists of

Each module produces two build targets:

| Target | Type | Purpose |
|--------|------|---------|
| `<lang>_analyzer` | `STATIC` library | The analyzer implementation itself (no exported symbols). Linked by unit tests that include the analyzer header directly; it exposes its own `include/` directory as `PUBLIC`. Also reused by the plugin below (built PIC). |
| `lang<lang>`      | `MODULE` library | The runtime plugin `.so`, emitted into `bin/plugins/`. Made of the analyzer + the export wrapper. |

Once both targets are defined, the module registers itself into two **GLOBAL
properties**, which the layers above consume generically (nothing above ever
has to name a specific language):

- `INDEX_LANG_PLUGINS` — all plugin `.so` targets; the host adds them as build dependencies.
- `INDEX_LANG_ANALYZER_LIBS` — all analyzer static libraries; the tests link them uniformly.

---

## Plugin contract (ABI)

The contract header is `include/lang_plugin.hpp`, shared by the host and every
plugin:

- `LangPlugin` abstract interface: `abi_version()` / `name()` / `extensions()` / `create()`.
- `LANG_PLUGIN_ABI_VERSION`: the ABI version; the host refuses to load a plugin whose version does not match.
- A plugin must export a factory named `create_lang_plugin` (via `BOOST_DLL_ALIAS`).

**Lifetime note**: a `LangAnalyze` object created by a plugin has its vtable and
destructor code inside the `.so`. `LangPluginManager` binds the
`shared_library` that owns that `.so` into the deleter of the
`shared_ptr<LangAnalyze>` it returns, guaranteeing the `.so` outlives every
analyzer it created — callers just use `shared_ptr` RAII as usual and never
have to think about library unloading.

---

## How to add a new language (example: `rust`)

Suppose you want to add Rust support. All the work happens inside
`languages/rust/`.

### 1. Create the directory

```
languages/rust/
├── CMakeLists.txt
├── include/rustlang.h
└── src/
    ├── rustlang.cpp
    └── rustlang_plugin.cpp
```

### 2. Implement the analyzer `rustlang.h` / `rustlang.cpp`

Subclass `indextools::LangAnalyze` (see `include/lang.hpp`) and implement the
pure-virtual interface: `analyze()`, `reset()`, `result()`, `locate_entity()`,
`locate_identifier()`, `get_full_structure()`. The base class already provides
source loading, line indexing, `locate_pattern()`, `get_dedented_lines()`, and
other shared facilities. Use `python/` (structured tree-sitter analysis) or
`fallback/` (identifier tokenization only) as references.

### 3. Write the plugin export wrapper `rustlang_plugin.cpp`

Copy `python/src/pythonlang_plugin.cpp` and change three things: the class name,
`name()`, and the extension list in `extensions()` (**each language declares its
own**):

```cpp
#include "rustlang.h"
#include "lang_plugin.hpp"
#include <boost/dll/alias.hpp>

namespace indextools {
namespace {
class RustPlugin final : public LangPlugin {
public:
    std::uint32_t abi_version() const noexcept override { return LANG_PLUGIN_ABI_VERSION; }
    std::string_view name() const noexcept override { return "Rust"; }
    std::vector<std::string_view> extensions() const noexcept override { return {".rs"}; }
    std::unique_ptr<LangAnalyze> create() const override {
        return std::make_unique<RustLanguage>();
    }
};
} // namespace

std::shared_ptr<LangPlugin> create_lang_plugin() { return std::make_shared<RustPlugin>(); }
} // namespace indextools

BOOST_DLL_ALIAS(indextools::create_lang_plugin, create_lang_plugin)
```

> If an extension used to fall through to `fallback` (e.g. `.rs`), remove it
> from the list in `fallback/src/fallbacklang_plugin.cpp` so two plugins do not
> claim the same extension (on a clash `LangPluginManager` prints a warning and
> ignores the later claimant).

### 4. Write the module `CMakeLists.txt`

Copy `python/CMakeLists.txt` (tree-sitter based) or `fallback/CMakeLists.txt`
(no third-party parser) as a template, and change the three variables at the
top plus the source filenames:

```cmake
set(LANG_NAME     "rust")
set(LANG_ANALYZER "rust_analyzer")
set(LANG_PLUGIN   "langrust")        # -> liblangrust.so
```

If the language needs a third-party parser, `find_library` for it **right
inside** the module's `CMakeLists.txt` (as the python module does for
`tree-sitter`) — dependency declarations belong to the module, not to any
layer above.

### 5. Reconfigure and build

```bash
# from the project root (simplex-cpp/):
cmake -B build -S . && cmake --build build -j
```

`cmake` auto-discovers `languages/rust/` (a new subdirectory requires rerunning
configure), producing `bin/plugins/liblangrust.so`, which the host picks up on
its next startup.

### 6. (Optional) Add a unit test

Create `test/test_rustlang.cpp` with `#include "rustlang.h"`. The test harness
automatically compiles it into its own executable and links all language
analyzer static libraries (including `rust_analyzer`) — no edit to
`test/CMakeLists.txt` is needed.

---

## Summary

Adding a language only ever touches the `languages/<lang>/` directory. The
layers above — `languages/CMakeLists.txt`, the top-level `CMakeLists.txt`, and
`test/CMakeLists.txt` — are decoupled from the modules through "auto-discovery +
GLOBAL property registration," and none of them need to change.
