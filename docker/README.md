# docker — the containerized build context

Two images implement the "same execution context" strategy
(`docs/abi-context.md`) as something you can actually run: host
executables and dlopened plugin modules built by one compiler, one Boost,
one configure — inside one container.

| File | Image | Role |
| --- | --- | --- |
| `Dockerfile.build-base` | `…/build-base` | The toolchain base: everything the core tree needs to configure, build and test, **no sources**. Built rarely, published to ghcr, cached hard. |
| `Dockerfile.build-context` | `simplex-cpp-build` | `FROM` the base; `COPY`s the tree and runs one configure + build + full ctest. Rebuilt per tree change. |

The build context for both is the **repository root** (the root
`.dockerignore` governs `COPY . .`); invoke them as:

```bash
docker build -f docker/Dockerfile.build-base  -t <base-tag> .
docker build -f docker/Dockerfile.build-context -t <ctx-tag> .
```

## What the base image contains

| Piece | Version | Where | Notes |
| --- | --- | --- | --- |
| gcc (g++ included) | **14.3.0**, point-pinned + digest | `FROM gcc:14.3.0@sha256:…` | The admission fingerprint hashes the *complete* compiler version, so a moving tag (`gcc:14`) would drift the toolchain identity across image rebuilds; the digest guards against the point tag itself being re-pushed. The image also aliases `/usr/local/bin/g++-14 → g++` (see below). Bump the tag ⇒ update digest + `simplex.build.gcc` LABEL, retag, republish. |
| CMake | 3.31 (apt) | PATH | Tree requires ≥ 3.20. |
| OpenSSL dev | distro version | apt `libssl-dev` | For the asio SSL runtime; no pin by design (`third_party/versions/README.md`). |
| binutils (nm/readelf) | from the gcc base image | PATH | Needed by the `llm_plugin_boundary_hygiene` ctest. |
| Boost | 1.91.0 **shared** | `/usr/local` | Built with exactly `filesystem` + `test` (= unit_test_framework): headers, `libboost_*.so.1.91.0`, and the CMake config package under `/usr/local/lib/cmake/Boost-1.91.0` — `find_package(Boost)` needs no `BOOST_ROOT`. |
| nlohmann/json | 3.12.0 (single header) | `/opt/simplex-thirdparty/include/nlohmann/json.hpp` | |
| yaml-cpp | 0.9.0, static **PIC** | `/opt/simplex-thirdparty/libs/libyaml-cpp.a` | PIC because it reaches MODULE plugin `.so`s through `simplex_thirdparty_iface`. |

`/opt/simplex-thirdparty` mirrors the tree's `third_party/{include,libs}`
layout, so the tree consumes it unchanged via
`-DSIMPLEX_THIRDPARTY_DIR=/opt/simplex-thirdparty`. Version pins follow
`third_party/versions/*.md` — that directory stays the single source of
truth; when a version moves, both this image and the records move with it.

Deliberately **absent**: any source code; tree-sitter and Boost
`process`/`program_options` (only consumers were the legacy
languages/indextools domain, no longer wired into the top-level build).

Recorded versions are inspectable: `docker inspect <image>` → LABELs
(`simplex.build.*`).

## Using it

### Local debug (before the base is on ghcr)

```bash
# 1) build the base under a local tag
docker build -f docker/Dockerfile.build-base -t simplex-build-base:local .

# 2) build the tree image FROM that base (BASE_IMAGE defaults to the
#    published registry tag, so local debugging overrides it)
docker build -f docker/Dockerfile.build-context \
    --build-arg BASE_IMAGE=simplex-build-base:local \
    -t simplex-cpp-build:local .

# 3) the build layer already ran the full ctest; the image CMD re-runs it
#    against the baked-in build tree:
docker run --rm simplex-cpp-build:local
#    to poke at the artifacts instead:
docker run --rm -it --entrypoint bash simplex-cpp-build:local
#    or extract the whole build tree:
docker run --rm simplex-cpp-build:local tar C /src/build -cf - . | tar C build-image -xf -
```

### Publishing the base to ghcr (manual)

```bash
echo <PAT> | docker login ghcr.io -u Hazer-BJTU --password-stdin   # PAT: write:packages
docker build -f docker/Dockerfile.build-base \
    -t ghcr.io/hazer-bjtu/simplex-cpp/build-base:boost1.91-gcc14 \
    -t ghcr.io/hazer-bjtu/simplex-cpp/build-base:latest .
docker push ghcr.io/hazer-bjtu/simplex-cpp/build-base:boost1.91-gcc14
docker push ghcr.io/hazer-bjtu/simplex-cpp/build-base:latest
```

Once pushed, `Dockerfile.build-context` builds as-is (its `BASE_IMAGE`
default) and anyone cloning the tree gets a working same-context build
without provisioning Boost or the vendored headers locally.

### Behind a proxy

Two separate channels, neither requiring persistent config changes:

- **Registry pulls** go through the docker *daemon* — configure its proxy
  once (`docker info` shows it), or pre-`docker pull` the base layers.
- **`RUN` steps inside the build** (apt, wget, Boost/yaml-cpp sources) take
  BuildKit's predefined proxy build args, which vanish with the command:

```bash
docker build -f docker/Dockerfile.build-base \
    --build-arg HTTP_PROXY=http://<proxy>:<port> \
    --build-arg HTTPS_PROXY=http://<proxy>:<port> \
    --build-arg NO_PROXY=localhost,127.0.0.1 \
    -t simplex-build-base:local .
```

## The same-context contract

- One image ⇒ one toolchain identity (`GNU-14.3.0` here). Host and every
  plugin built in a single configure+build inside the image are
  self-consistent — that is the whole point.
- **One toolchain, literally.** The gcc image ships its GCC as
  `/usr/local/bin/g++` (14.3.0) while the Debian base *also* carries a
  distro `/usr/bin/g++-14` (14.2.0 here). The tree pins the compiler by
  the **name** `g++-14`, which PATH would resolve to the distro one —
  splitting the context into project@14.2.0 + Boost@14.3.0, a split the
  admission fingerprint cannot see (it hashes only the project-side
  compiler). The base image therefore aliases
  `/usr/local/bin/g++-14 → g++`, which is what keeps the tree's default
  pin on the same compiler that built Boost. Verified by the generated
  `version.hpp` inside a built image: `GNU-14.3.0`.
- Artifacts from the image will **not** load against hosts built in any
  other context (e.g. the local WSL g++-14.2.0): the admission gate in
  `extension_framework/plugin_magic.hpp` rejects exactly that mixing, by
  design. Same context, not portable.
- Build jobs are capped at `-j4` even inside the container: it shares the
  host's CPUs, and saturating them destabilizes the WSL services.
