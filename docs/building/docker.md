# Build with the published Docker images

The project publishes three Linux x86_64 toolchain images to GHCR.

| Image | Purpose |
| --- | --- |
| `ghcr.io/hazer-bjtu/simplex-cpp/build-base:boost1.91-gcc14` | Development and CI in one compiler/dependency context |
| `ghcr.io/hazer-bjtu/simplex-cpp/build-portable:glibc2.34-gcc14.3` | Standard worker package (`linux-amd64-standard`) for hosts with glibc 2.34+ and OpenSSL 3 |
| `ghcr.io/hazer-bjtu/simplex-cpp/build-musl:musl1.2.6-gcc15.2` | Bundled worker package (`linux-amd64-bundled`) with the musl loader, runtime libraries and OpenSSL |

For reproducibility, use the corresponding digest pinned in
[CI](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/.github/workflows/ci.yml)
for development and standard builds, or in the
[musl workflow](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/.github/workflows/musl-portable.yml)
for the bundled package.
Use an x86_64 Docker host or configure emulation.

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

### Standard glibc package

Start with fresh build and staging directories.

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
    bash docker/check-worker-release.sh "worker-release/simplex-worker-v$(cat VERSION)-linux-x86_64-glibc2.34.tar.gz"
  '
```

`worker-release/` receives the archive and checksums. The staged tree includes
project libraries and the selected compiler/Boost runtime. Host glibc and OpenSSL
remain external. Symbol-floor checks constrain dependencies; running the archive
on target systems remains necessary. Official release CI also tests Ubuntu 22.04
and AlmaLinux 9.

### Bundled musl package

Use the release workflow's Dockerfile to build and stage the worker with the
musl launcher and bundled dependencies. The recipe configures Release with
whole-project LTO off.

```sh
image=ghcr.io/hazer-bjtu/simplex-cpp/build-musl:musl1.2.6-gcc15.2
docker build -f docker/Dockerfile.musl-test --build-arg BUILD_IMAGE="$image" \
  --build-arg JOBS=2 -t simplex-musl-build .
docker run --rm simplex-musl-build ctest --test-dir /build --output-on-failure --no-tests=error --timeout 120
mkdir -p musl-release
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/musl-release:/output" \
  simplex-musl-build bash /src/docker/package-musl-worker.sh /opt/simplex /output
```

The [musl build guide](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/docker/MUSL_EXPERIMENT.md#build-test-and-package-the-worker)
covers checksum verification, relocation and runtime checks.

For a container that **runs** the installed worker, follow
[Docker deployment](../deployment/docker-worker.md).
