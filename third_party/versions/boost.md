# Boost (system dependency, not vendored)

- Release: 1.91.0 (built locally from `boost_1_91_0.tar.gz` in
  `~/pkgcache/boost/`)
- Source: <https://www.boost.org/users/history/version_1_91_0.html>
- Required components (top-level `CMakeLists.txt`): `unit_test_framework`,
  `filesystem`, `process` (the core `process` module's manager, via the
  top-level `boost_process_iface`) + header-only Boost.DLL. Of the legacy
  domain's extras only `program_options` is still unwired; it stays with the
  extraction into a standalone module.
- Found via `find_package(Boost)` with `Boost_USE_STATIC_LIBS OFF` (shared
  libraries — one copy of every compiled runtime per process, per the
  plugin-boundary strategy); no files from Boost are vendored under
  `third_party/`.
- Container builds: `Dockerfile.build-base` compiles this release (shared,
  `filesystem` + `process` + `test`) into the toolchain base image published
  at ghcr.io.
- License: `../license/boost/LICENSE_1_0.txt`
