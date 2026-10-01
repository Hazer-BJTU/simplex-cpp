# Build in a local environment

Use a consistent C++ execution context for the worker and its native plugins.
The supported reference toolchain is GCC 14.3.0 with C++20. A binary built against
a newer host glibc is not automatically portable to older machines; use the
[portable build image](docker.md#portable-release-build) for release artifacts.

## Dependencies

| Dependency | Reference version / requirement |
| --- | --- |
| CMake | 3.20 or newer |
| GCC | 14.3.0; default compiler command is `g++-14` |
| Boost | 1.91.0, shared `filesystem`, `process`, `program_options`, `unit_test_framework` |
| OpenSSL | Development headers/libraries; released binaries target OpenSSL 3 |
| nlohmann/json | 3.12.0 single header |
| yaml-cpp | 0.9.0 static library compiled with PIC |
| Test utilities | Python 3, Bash, binutils (`readelf`, `nm`), normal POSIX process tools |

Install compiler, CMake, OpenSSL development files, Python, and download/archive
tools with your OS package manager. Distro Boost may be older than the version
required by this project's Asio and Process code. The commands below build the
reference dependencies into private directories, without overwriting system
libraries.

## Prepare dependencies

Run from a repository checkout. The example assumes `g++-14` and `gcc-14` are the
matching compiler pair. Set `JOBS` to a suitable parallelism level.

```sh
export CC=gcc-14 CXX=g++-14
export JOBS=2
export SIMPLEX_DEPS="$PWD/.local-deps"
mkdir -p "$SIMPLEX_DEPS/src" "$SIMPLEX_DEPS/third-party/include/nlohmann"
curl -fL https://archives.boost.io/release/1.91.0/source/boost_1_91_0.tar.gz \
  -o "$SIMPLEX_DEPS/src/boost.tar.gz"
tar -xzf "$SIMPLEX_DEPS/src/boost.tar.gz" -C "$SIMPLEX_DEPS/src"
(
  cd "$SIMPLEX_DEPS/src/boost_1_91_0"
  ./bootstrap.sh --with-libraries=filesystem,process,program_options,test
  printf 'using gcc : simplex : %s ;\n' "$(command -v g++-14)" > simplex-user-config.jam
  ./b2 --user-config=simplex-user-config.jam toolset=gcc-simplex \
    -j"$JOBS" link=shared runtime-link=shared variant=release \
    --prefix="$SIMPLEX_DEPS/boost" --libdir="$SIMPLEX_DEPS/boost/lib" install
)
curl -fL https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp \
  -o "$SIMPLEX_DEPS/third-party/include/nlohmann/json.hpp"
curl -fL https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.9.0.tar.gz \
  -o "$SIMPLEX_DEPS/src/yaml-cpp.tar.gz"
tar -xzf "$SIMPLEX_DEPS/src/yaml-cpp.tar.gz" -C "$SIMPLEX_DEPS/src"
cmake -S "$SIMPLEX_DEPS/src/yaml-cpp-yaml-cpp-0.9.0" \
  -B "$SIMPLEX_DEPS/yaml-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 \
  -DYAML_BUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF \
  -DYAML_CPP_BUILD_CONTRIB=OFF -DCMAKE_INSTALL_LIBDIR=libs \
  -DCMAKE_INSTALL_PREFIX="$SIMPLEX_DEPS/third-party"
cmake --build "$SIMPLEX_DEPS/yaml-build" -j"$JOBS"
cmake --install "$SIMPLEX_DEPS/yaml-build"
```

Verify that `third-party/libs/libyaml-cpp.a` and the `yaml-cpp` include directory
exist. The project expects `include/` and `libs/` beneath
`SIMPLEX_THIRDPARTY_DIR`. Source releases and licenses are also recorded in
[third_party/versions](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/third_party/versions).

## Configure, build, test, install

```sh
cmake -S . -B build-local \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_PREFIX_PATH="$SIMPLEX_DEPS/boost" \
  -DSIMPLEX_THIRDPARTY_DIR="$SIMPLEX_DEPS/third-party" \
  -DCMAKE_INSTALL_PREFIX="$PWD/install-local"
cmake --build build-local -j"$JOBS"
ctest --test-dir build-local --output-on-failure --no-tests=error --timeout 120
cmake --install build-local --strip
export LD_LIBRARY_PATH="$SIMPLEX_DEPS/boost/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./install-local/bin/simplex run --help
```

This local install does not copy the private Boost shared libraries into
`install-local/lib`. The installed binaries use executable-relative library
paths, so the export above makes the explicitly selected Boost `lib` directory
available at runtime while preserving any existing `LD_LIBRARY_PATH`. Keep it
set when running the worker, or start the Hub from the same shell so launched
workers inherit it. Repeat the export in a new shell, using the absolute path
to your dependency directory.

For a distributable installation, use the [portable release build](docker.md#portable-release-build),
which bundles the required runtime libraries. Copying only the local executable
does not produce a self-contained worker.

Release defaults to `SIMPLEX_RELEASE_OPTIMIZATION=-Os`. Set it to `-O3` to trade
size for a different optimization policy. `SIMPLEX_ENABLE_LTO=ON` enables
whole-project Release LTO when supported; official release builds enable it.
Use a separate build directory when changing the compiler or dependency context.

The Hub is separate: in `hub/`, run `npm ci`, `npm run build`, `npm run typecheck`,
and `npm test`. `npm run build:release` also emits the installed server JavaScript.
