# versioning

Single source of truth for project version constants. A parallel sibling of
`indextools/` that owns **no compiled code** — it only renders a header template
(`version.hpp.in`) into a build-time, header-only
`versioning/version.hpp` via CMake's `configure_file()`.

## What lives here

| Constant | Source |
|---|---|
| `simplex::VERSION_MAJOR/MINOR/PATCH` | top-level `project(simplex_cpp VERSION …)` |
| `simplex::VERSION_STRING` | `PROJECT_VERSION` |
| `simplex::LANG_PLUGIN_ABI_VERSION` | `SIMPLEX_LANG_PLUGIN_ABI_VERSION` in this module's `CMakeLists.txt` |

The plugin ABI version was migrated here from
`indextools/include/lang_plugin.hpp`; that header now `#include`s the generated
header and keeps an `indextools::` namespace alias, so existing references are
unchanged.

## Consuming the generated header

The header lands at `build/generated/versioning/version.hpp`. Any module gets it
on its include path by linking the INTERFACE target:

```cmake
target_link_libraries(my_target PRIVATE simplex_versioning)
```

`indextools` instead uses the directory-scope variable (matching its existing
`include_directories(...)` style), so its host, plugins, and tests all see it:

```cmake
include_directories("${SIMPLEX_VERSIONING_INCLUDE_DIR}")
```

Then in code:

```cpp
#include "versioning/version.hpp"

std::cout << simplex::VERSION_STRING;             // "0.0.1"
if (plugin->abi_version() != simplex::LANG_PLUGIN_ABI_VERSION) { /* … */ }
```

## Adding a new version constant

1. Set (or derive) its value in `CMakeLists.txt`.
2. Reference it in `version.hpp.in` as an `@PLACEHOLDER@`.
3. Re-configure the build (`cmake …`).
