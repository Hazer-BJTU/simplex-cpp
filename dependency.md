# Dependency Map

This document records, per module, what each part of `simplex-cpp` depends on —
both the **system** dependencies (found via `find_package`) and the **vendored**
dependencies (under the top-level `third_party/` tree). It is the companion to
the top-level `CMakeLists.txt`, which owns the shared/system dependencies; each
module's own `CMakeLists.txt` owns its module-private dependencies.

## Conventions

- **System dependencies** (Boost) are found once at the top level and shared.
  They are *not* vendored — install Boost on the host (the original project used
  the system Boost at `/usr/local/include`).
- **Vendored dependencies** live under `third_party/` at the **project root**
  (not inside any module). Their licenses are mirrored under
  `third_party/license/`. A module pulls a vendored header in simply by
  `#include`-ing it (the `third_party/include` dir is on the global include
  path); a module links a vendored static lib by `find_library(... PATHS
  "${SIMPLEX_THIRDPARTY_DIR}/libs")` in its own `CMakeLists.txt`.
- The toolchain is **g++-14**, C++20, pinned in the top-level `CMakeLists.txt`.

### Path variables (set by the build, not hardcoded)

| Variable | Meaning | Set by |
|---|---|---|
| `SIMPLEX_THIRDPARTY_DIR` | `<root>/third_party` | top-level `CMakeLists.txt` |
| `INDEXTOOLS_SOURCE_DIR` | `<root>/indextools` | `indextools/CMakeLists.txt` |
| `INDEXTOOLS_INCLUDE_DIR` | `<root>/indextools/include` | `indextools/CMakeLists.txt` |

---

## Top-level (shared)

Owned by the root `CMakeLists.txt`. Every module inherits these.

| Dependency | Type | Used for |
|---|---|---|
| g++-14 | toolchain | compiler (pinned before `project()`) |
| CMake ≥ 3.20 | build | — |
| C++20 | standard | — |
| Boost `process` | system lib | async subprocess management (`SubProcessManager`) |
| Boost `program_options` | system lib | host CLI parsing |
| Boost `filesystem` | system lib | backs Boost.DLL (`boost_dll_iface`) |
| Boost `unit_test_framework` | system lib | the unit-test layer |
| Boost `headers` | system, header-only | Boost.DLL, Boost.Asio, Boost.System |
| Boost.DLL | header-only | runtime plugin loading (`boost_dll_iface` = `Boost::headers` + `Boost::filesystem` + `libdl`, with `BOOST_DLL_USE_STD_FS`) |
| `third_party/include` | vendored headers | global include path (nlohmann, tree-sitter headers) |
| `third_party/libs` | vendored static libs | lib search path (tree-sitter `.a`) |

---

## `indextools/` — the host module

Builds the `indextools` executable from `src/*.cpp` + `main.cpp`.

| Dependency | Source | Notes |
|---|---|---|
| Boost.process | system | `SubProcessManager` |
| Boost.program_options | system | CLI |
| Boost.DLL | system header-only | via `boost_dll_iface`; loads language plugins at runtime from `<executable_dir>/plugins` |
| nlohmann/json | vendored header | used by `editor`, `cache_system`, `viewer`, `service_command/*` |
| tree-sitter `api.h` | vendored header | pulled in transitively via `include/split.hpp` (header-only helpers) |

The host does **not** link the language analyzers directly — it discovers and
loads `bin/plugins/lib*.so` at runtime via `LangPluginManager`.

---

## `indextools/languages/python/` — Python analyzer plugin

A self-contained language module (auto-discovered by `languages/CMakeLists.txt`).
Produces `python_analyzer` (static, for tests) and `langpython` (MODULE `.so`,
for runtime).

| Dependency | Source | Notes |
|---|---|---|
| tree-sitter | vendored lib `libtree-sitter.a` | parser runtime |
| tree-sitter-python | vendored lib `libtree-sitter-python.a` | Python grammar |
| tree-sitter headers | vendored `tree_sitter/api.h`, `tree_sitter/tree-sitter-python.h` | |
| nlohmann/json | vendored header | via `pythonlang.hpp` |
| Boost.DLL | system header-only | plugin export wrapper (`BOOST_DLL_ALIAS`) |

This module owns its own tree-sitter `find_library` — the dependency travels
with the module, not with any layer above.

## `indextools/languages/fallback/` — generic tokenizer plugin

The catch-all analyzer for extensions without a dedicated language module.

| Dependency | Source | Notes |
|---|---|---|
| nlohmann/json | vendored header | via `fallbacklang.hpp` |
| Boost.DLL | system header-only | plugin export wrapper |

No tree-sitter dependency — this module tokenizes identifiers only.

---

## `indextools/test/` — unit tests

One executable per `test_*.cpp`, registered with CTest. Lives in the module
(tests stay with the code they exercise).

| Dependency | Source | Notes |
|---|---|---|
| Boost.unit_test_framework | system | every test binary |
| Boost.process | system | tests that drive `SubProcessManager` |
| Boost.DLL | system header-only | `test_plugin_manager`, `test_cache_system` (runtime plugin loader) |
| language analyzer libs | in-tree static | linked by header-including tests (e.g. `test_pythonlang`) via the `INDEX_LANG_ANALYZER_LIBS` global property |
| language plugins | in-tree `.so` | built as deps of the runtime-loader tests via `INDEX_LANG_PLUGINS` |

---

## Vendored `third_party/` inventory

| Path | Library | License | Used by |
|---|---|---|---|
| `include/nlohmann/json.hpp` | nlohmann_json | MIT (`license/nlohmann_json`) | host, python, fallback |
| `include/tree_sitter/api.h` | tree-sitter (headers) | MIT (`license/tree-sitter`) | host (transitively), python |
| `include/tree_sitter/tree-sitter-python.h` | tree-sitter-python (header) | MIT (`license/tree-sitter-python`) | python |
| `libs/libtree-sitter.a` | tree-sitter (static) | MIT | python |
| `libs/libtree-sitter-python.a` | tree-sitter-python (static) | MIT | python |
| `include/p-ranav-glob/glob.hpp` | p-ranav-glob | MIT (`license/p-ranav-glob`) | **currently unused** — globbing (`glob_match`/`glob_find`) is implemented natively in `src/utils.cpp`; vendored for reference/future use |

> Adding a vendored dependency: drop headers under `third_party/include/<name>/`
> and static libs under `third_party/libs/`, mirror the license under
> `third_party/license/<name>/`, and add a row here. The global include and lib
> search paths pick them up automatically; a module links a specific lib with a
> local `find_library` in its own `CMakeLists.txt`.

---

## Future modules (planned, not yet built)

Per the project roadmap, the following native-C++ modules will be added as
siblings of `indextools/` and wired in with `add_subdirectory()` from the
top-level `CMakeLists.txt`:

- **model adapter** — native C++ model-request tool adaptation. Expected to
  reuse the shared Boost (Asio/Process) and `third_party` tree.
- **agentloop** — the agent loop. Expected to depend on `indextools` (search /
  view / edit / subprocess facilities) and the model adapter.

Their dependencies will be documented here as they land.
