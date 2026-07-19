# Dependency Map

This document records, per module, what each part of `simplex-cpp` depends on —
both the **system** dependencies (found via `find_package`) and the **vendored**
dependencies (under the top-level `third_party/` tree). It is the companion to
the top-level `CMakeLists.txt`, which owns the shared/system dependencies; each
module's own `CMakeLists.txt` owns its module-private dependencies.

## Conventions

- **Header management is interface-target-only.** There are no directory-scope
  `include_directories()`/`link_directories()` calls and no propagated include
  path variables. A target gets header paths by `target_link_libraries(...)`-ing
  an INTERFACE target (or a lib whose PUBLIC includes carry its own headers).
  Interface targets are the single source of truth for include paths — see the
  table below.
- **Exported headers are namespaced.** Every module's public headers live under
  `include/<module>/` and are included as `"<module>/<header>.hpp"` (e.g.
  `"indextools/lang.hpp"`, `"logging/logger.hpp"`, `"python/pythonlang.hpp"`).
  No flat header names — modules never collide. The generated versioning header
  follows the same rule (`"versioning/version.hpp"`).
- **System dependencies** (Boost, OpenSSL) are found once at the top level and
  shared. They are *not* vendored — install Boost on the host (the original
  project used the system Boost at `/usr/local/include`).
- **Vendored dependencies** live under `third_party/` at the **project root**
  (not inside any module). Their licenses are mirrored under
  `third_party/license/`. A module pulls a vendored header in via
  `simplex_thirdparty_iface` (linked transitively through `indextools_iface`);
  a module links a vendored static lib by `find_library(... PATHS
  "${SIMPLEX_THIRDPARTY_DIR}/libs")` in its own `CMakeLists.txt`.
- The toolchain is **g++-14**, C++20, pinned in the top-level `CMakeLists.txt`.

### Path variables (set by the build, not hardcoded)

Only the vendored-tree location remains as a variable (used by `find_library`
PATHS); include paths are not variables anymore.

| Variable | Meaning | Set by |
|---|---|---|
| `SIMPLEX_THIRDPARTY_DIR` | `<root>/third_party` | top-level `CMakeLists.txt` |

### Interface targets (header-path carriers)

| Target | Carries | Used by |
|---|---|---|
| `simplex_thirdparty_iface` | `third_party/include` + the `third_party/libs` search dir | `indextools_iface` (transitively all its consumers) |
| `simplex_versioning` | build-generated `versioning/version.hpp` dir | `indextools_iface` (transitively its consumers) |
| `indextools_iface` | `indextools/include` + (via links) third_party + versioning | the sibling `languages/` module (analyzers + plugins), and `indextools_lib` (PUBLIC) |
| `logging_lib` | `logging/include` (PUBLIC on the static lib itself) | `indextools_lib` (PUBLIC), `logging/test` |
| `indextools_lib` | everything above (PUBLIC) + compiled indextools logic | indextools tests, future sibling modules |

---

## Top-level (shared)

Owned by the root `CMakeLists.txt`. Every module inherits these.

| Dependency | Type | Used for |
|---|---|---|
| g++-14 | toolchain | compiler (pinned before `project()`) |
| CMake ≥ 3.20 | build | — |
| C++20 | standard | — |
| OpenSSL (`libssl` + `libcrypto`) | system lib | TLS/SSL connectivity, cryptographic hashing (via `openssl_iface` = `OpenSSL::SSL` + `OpenSSL::Crypto`) |
| Boost `process` | system lib | async subprocess management (`SubProcessManager`) |
| Boost `program_options` | system lib | host CLI parsing |
| Boost `filesystem` | system lib | backs Boost.DLL (`boost_dll_iface`) |
| Boost `unit_test_framework` | system lib | the unit-test layer |
| Boost `headers` | system, header-only | Boost.DLL, Boost.Asio, Boost.System |
| Boost.DLL | header-only | runtime plugin loading (`boost_dll_iface` = `Boost::headers` + `Boost::filesystem` + `libdl`, with `BOOST_DLL_USE_STD_FS`) |
| `third_party/include` | vendored headers | global include path (nlohmann, tree-sitter headers) |
| `third_party/libs` | vendored static libs | lib search path (tree-sitter `.a`) |

---

## `logging/` — global logger module (sibling of `indextools/`)

A standalone module split out of indextools: a thread-safe console logger
(`logging::Logger`) usable by any module without pulling in the indextools host.
Produces `logging_lib` (STATIC); headers under `include/logging/`, included as
`"logging/logger.hpp"`. Depends only on the standard library.

| Dependency | Source | Notes |
|---|---|---|
| (none) | — | standard library only (`<chrono>`, `<format>`, `<mutex>`, …) |

Consumers link `logging_lib`; `indextools_lib` links it PUBLIC because the
public `indextools/service_command/service_command.hpp` header uses
`logging::Logger`.

---

## `indextools/` — the host module

Builds the `indextools_lib` static library from `src/*.cpp`. Public headers
live under `include/indextools/` (included as `"indextools/<name>.hpp"`).

| Dependency | Source | Notes |
|---|---|---|
| Boost.process | system | `SubProcessManager` |
| Boost.program_options | system | CLI |
| Boost.DLL | system header-only | via `boost_dll_iface`; loads language plugins at runtime from `<executable_dir>/plugins` |
| nlohmann/json | vendored header | via `indextools_iface`; used by `editor`, `cache_system`, `viewer`, `service_command/*` |
| tree-sitter `api.h` | vendored header | via `indextools_iface`; pulled in transitively through `indextools/split.hpp` (header-only helpers) |
| `logging_lib` | in-tree static | PUBLIC — `service_command.hpp` uses `logging::Logger` |

The host does **not** link the language analyzers directly — it discovers and
loads `bin/plugins/lib*.so` at runtime via `LangPluginManager`.

---

## `languages/` — language plugin module (sibling of `indextools/`)

Auto-discovers its language subdirectories (`python/`, `fallback/`, …) and
registers each plugin/analyzer target into the GLOBAL properties
`INDEX_LANG_PLUGINS` / `INDEX_LANG_ANALYZER_LIBS`, which `indextools/` and both
modules' `test/` subdirectories consume. Added before `indextools/` at the top
level so those properties are populated when `indextools/` reads them.

Languages compile against the indextools **header contract only**
(`indextools/lang.hpp`, `indextools/lang_plugin.hpp` are header-only) via the
`indextools_iface` INTERFACE target — plugin `.so`s do not link `indextools_lib`.
Each language's own headers live under `include/<lang>/` (included as
`"<lang>/<header>.hpp"`).

### `languages/python/` — Python analyzer plugin

A self-contained language module (auto-discovered by `languages/CMakeLists.txt`).
Produces `python_analyzer` (static, for tests) and `langpython` (MODULE `.so`,
for runtime).

| Dependency | Source | Notes |
|---|---|---|
| tree-sitter | vendored lib `libtree-sitter.a` | parser runtime |
| tree-sitter-python | vendored lib `libtree-sitter-python.a` | Python grammar |
| tree-sitter headers | vendored `tree_sitter/api.h`, `tree_sitter/tree-sitter-python.h` | |
| nlohmann/json | vendored header | via `python/pythonlang.hpp` |
| indextools contract | in-tree headers | via `indextools_iface` (`indextools/lang.hpp`, `indextools/lang_plugin.hpp`, versioning) |
| Boost.DLL | system header-only | plugin export wrapper (`BOOST_DLL_ALIAS`) |

This module owns its own tree-sitter `find_library` — the dependency travels
with the module, not with any layer above.

### `languages/fallback/` — generic tokenizer plugin

The catch-all analyzer for extensions without a dedicated language module.

| Dependency | Source | Notes |
|---|---|---|
| nlohmann/json | vendored header | via `fallback/fallbacklang.hpp` |
| indextools contract | in-tree headers | via `indextools_iface` |
| Boost.DLL | system header-only | plugin export wrapper |

No tree-sitter dependency — this module tokenizes identifiers only.

---

## `logging/test/` — logger unit tests

One executable per `test_*.cpp` (`test_logger`), registered with CTest.

| Dependency | Source | Notes |
|---|---|---|
| Boost.unit_test_framework | system | every test binary |
| `logging_lib` | in-tree static | brings `logging/logger.hpp` (PUBLIC) + compiled `Logger` |

---

## `indextools/test/` — host unit tests

One executable per `test_*.cpp`, registered with CTest. Tests stay with the code
they exercise: tests for the indextools machinery live here; tests for a
specific language analyzer live in `languages/test/`; tests for the logger live
in `logging/test/`.

| Dependency | Source | Notes |
|---|---|---|
| Boost.unit_test_framework | system | every test binary |
| Boost.process | system | tests that drive `SubProcessManager` |
| Boost.DLL | system header-only | `test_plugin_manager`, `test_cache_system` (runtime plugin loader) |
| language plugins | in-tree `.so` | built as deps of the runtime-loader tests via `INDEX_LANG_PLUGINS` |

---

## `languages/test/` — language analyzer unit tests

One executable per `test_*.cpp` (e.g. `test_pythonlang`, `test_fallbacklang`),
registered with CTest.

| Dependency | Source | Notes |
|---|---|---|
| Boost.unit_test_framework | system | every test binary |
| indextools contract | in-tree headers | via `indextools_iface` (`indextools/lang.hpp` base class) |
| language analyzer libs | in-tree static | linked via the `INDEX_LANG_ANALYZER_LIBS` global property |

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
