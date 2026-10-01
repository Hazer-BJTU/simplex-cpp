# Build with the published Docker images

The author publishes toolchain images to GHCR. They contain dependencies and
compilers, not a configured worker installation.

| Image | Purpose |
| --- | --- |
| `ghcr.io/hazer-bjtu/simplex-cpp/build-base:boost1.91-gcc14` | Development and CI in one compiler/dependency context |
| `ghcr.io/hazer-bjtu/simplex-cpp/build-portable:glibc2.34-gcc14.3` | Portable Linux x86_64 release builds with bundled runtime libraries |

For reproducibility, use the corresponding digest pinned in
[CI](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/.github/workflows/ci.yml)
or the [release workflow](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/.github/workflows/release-worker.yml).
Tags can move. The images target Linux x86_64; use a matching Docker host or
explicitly configured emulation.

## Development build

From the repository root:

```sh
image=ghcr.io/hazer-bjtu/simplex-cpp/build-base:boost1.91-gcc14
docker pull "$image"
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:/src" -w /src "$image" bash -lc '
    set -e
    cmake -S . -B build-container -DCMAKE_BUILD_TYPE=Debug \
      -DSIMPLEX_THIRDPARTY_DIR=/opt/simplex-thirdparty
    cmake --build build-container -j2
    ctest --test-dir build-container --output-on-failure --no-tests=error --timeout 120
  '
```

The source mount also receives build output. The user mapping prevents root-owned
output on a normal Linux host. Keep container and local build directories
separate. Development-image artifacts may depend on that image's newer glibc;
run them inside the same environment unless you have checked host compatibility.

## Portable release build

```sh
image=ghcr.io/hazer-bjtu/simplex-cpp/build-portable:glibc2.34-gcc14.3
docker pull "$image"
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:/src" -w /src "$image" bash -lc '
    set -e
    cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release \
      -DSIMPLEX_ENABLE_LTO=ON \
      -DSIMPLEX_THIRDPARTY_DIR=/opt/simplex-thirdparty \
      -DSIMPLEX_BUNDLE_RUNTIME=/opt/simplex-runtime \
      -DCMAKE_INSTALL_PREFIX=/src/stage-portable \
      -DSIMPLEX_GLIBC_FLOOR=2.34 -DSIMPLEX_OPENSSL_FLOOR=3.0.0 \
      -DSIMPLEX_STRICT_NEEDED=ON -DSIMPLEX_STAGING_DIR=/src/stage-portable
    cmake --build build-portable -j2
    cmake --install build-portable --strip
    ctest --test-dir build-portable --output-on-failure --no-tests=error --timeout 120
    bash docker/package-worker-release.sh stage-portable worker-release
    bash docker/check-worker-release.sh worker-release/*.tar.gz
  '
```

`worker-release/` receives the archive and checksums. The staged tree includes
project libraries and the selected compiler/Boost runtime. Host glibc and OpenSSL
remain external. Symbol-floor checks constrain dependencies; running the archive
on target systems remains necessary. Official release CI also tests Ubuntu 22.04
and AlmaLinux 9.

To build the toolchain images yourself, inspect
[Dockerfile.build-base](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/docker/Dockerfile.build-base)
and [Dockerfile.build-portable](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/docker/Dockerfile.build-portable).
The portable Dockerfile downloads a pinned existing GCC toolchain image rather
than compiling GCC. Building Boost and yaml-cpp still takes time.

For a container that **runs** the installed worker, follow
[Docker deployment](../deployment/docker-worker.md).
