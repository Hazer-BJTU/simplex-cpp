# Vendored third-party releases

Headers and static libraries under `third_party/include/` and
`third_party/libs/` are **not** committed to git (see the root `.gitignore`);
only the licenses under `third_party/license/` are.

This directory pins the exact release of every vendored library so a fresh
checkout can be rebuilt locally: fetch the recorded release from its source
and copy the listed files into place.

| Library           | Release   | Record                              |
| ----------------- | --------- | ----------------------------------- |
| nlohmann/json     | 3.12.0    | [nlohmann_json.md](nlohmann_json.md) |
| tree-sitter       | <fill in> | [tree-sitter.md](tree-sitter.md)    |
| tree-sitter-python| <fill in> | [tree-sitter-python.md](tree-sitter-python.md) |
| yaml-cpp          | 0.9.0     | [yaml-cpp.md](yaml-cpp.md)          |
| Boost (system)    | 1.91.0    | [boost.md](boost.md)                |

Boost is a system dependency (built locally, not vendored under
`third_party/`), but its pinned release and required components are recorded
in `boost.md` as well. OpenSSL is resolved by CMake's `FindOpenSSL` with no
pinned version; its requirement lives in the top-level `CMakeLists.txt`.
