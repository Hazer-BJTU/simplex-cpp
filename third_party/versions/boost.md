# Boost (system dependency, not vendored)

- Release: 1.91.0 (built locally from `boost_1_91_0.tar.gz` in
  `~/pkgcache/boost/`)
- Source: <https://www.boost.org/users/history/version_1_91_0.html>
- Required components (top-level `CMakeLists.txt`): `unit_test_framework`,
  `process`, `program_options`, `filesystem` (+ header-only Boost.DLL)
- Found via `find_package(Boost)` with `Boost_USE_STATIC_LIBS ON`; no files
  from Boost are vendored under `third_party/`.
- License: `../license/boost/LICENSE_1_0.txt`
