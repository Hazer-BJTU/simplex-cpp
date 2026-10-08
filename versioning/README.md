# versioning

The repository root `VERSION` is the single value to edit for a project
release. It contains exactly `MAJOR.MINOR.PATCH`. CMake
reads it for the C++ project version and generates the header-only
`versioning/version.hpp` from `version.hpp.in`. The worker uses that version
in its prompt signature and exposes it through `simplex --version`,
`simplex run --version`, and `simplex_worker --version`.

Hub reads its npm package version for `simplex-hub --version` and panel
metadata. npm requires the version to be present in both `hub/package.json`
and `hub/package-lock.json`, so these fields are synchronized copies of
`VERSION`, not separate version decisions. After changing `VERSION`, run:

```bash
cd hub
npm run version:sync
npm run version:check
```

CI runs `version:check` on the supported Node versions. The synchronization
script uses only Node built-ins and does not download dependencies.

## What lives here

| Constant | Source |
|---|---|
| `simplex::VERSION_MAJOR/MINOR/PATCH` | root `VERSION`, read by top-level CMake |
| `simplex::VERSION_STRING` | root `VERSION`, via `PROJECT_VERSION` |
| `simplex::TOOLSET_PLUGIN_ABI_VERSION` | `SIMPLEX_TOOLSET_PLUGIN_ABI_VERSION` in this module's `CMakeLists.txt` |
| `simplex::LOOP_HOOK_PLUGIN_ABI_VERSION` | `SIMPLEX_LOOP_HOOK_PLUGIN_ABI_VERSION` in this module's `CMakeLists.txt` |
| `simplex::LLM_PLUGIN_ABI_VERSION` | `SIMPLEX_LLM_PLUGIN_ABI_VERSION` in this module's `CMakeLists.txt` |

Each plugin domain has an independent ABI constant defined here. Toolsets and
loop hooks expose their corresponding constant as `kAbiVersion` in their public
extension header. For LLM models,
`llm/include/llm/models.hpp` includes the generated header and aliases
`simplex::LLM_PLUGIN_ABI_VERSION` as `llm::LLM_PLUGIN_ABI_VERSION`.
These ABI numbers describe binary compatibility and must change only when the
corresponding contract changes; they do not follow the project release version.
The Hub panel protocol and persistence schema versions are likewise separate
compatibility markers.

## Consuming the generated header

The header lands at `build/generated/versioning/version.hpp`. Any module gets it
on its include path by linking the INTERFACE target:

```cmake
target_link_libraries(my_target PRIVATE simplex_versioning)
```

This is the single supported way: the project manages all header paths through
interface targets, not directory-scope `include_directories(...)` variables.

Then in code:

```cpp
#include "versioning/version.hpp"

std::cout << simplex::VERSION_STRING;
if (plugin->abi_version() != simplex::LLM_PLUGIN_ABI_VERSION) { /* … */ }
```

## Adding a new version constant

1. Set (or derive) its value in `CMakeLists.txt`.
2. Reference it in `version.hpp.in` as an `@PLACEHOLDER@`.
3. Re-configure the build (`cmake …`).
