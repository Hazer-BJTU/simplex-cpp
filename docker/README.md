# docker — the containerized build context

Two images implement the "same execution context" strategy
(`docs/abi-context.md`) as something you can actually run: host
executables and dlopened plugin modules built by one compiler, one Boost,
one configure — inside one container.

| File | Image | Role |
| --- | --- | --- |
| `Dockerfile.build-base` | `…/build-base` | The toolchain base: everything the core tree needs to configure, build and test, **no sources**. Built rarely, published to ghcr, cached hard. |
| `Dockerfile.build-context` | `simplex-cpp-build` | `FROM` the base; `COPY`s the tree and runs one configure + build + full ctest. Rebuilt per tree change. |
| `Dockerfile.build-portable` | `…/build-portable` | Same role as `build-base`, different goal: artifacts that **run on other machines**. See [Which base to use](#which-base-to-use). |
| `portability_floor.cmake` | — | The ctest that keeps `build-portable` honest: asserts no artifact requires a newer glibc than the release targets. |

## Which base to use

`build-base` and `build-portable` are both toolchain bases and they are not
interchangeable:

| | `build-base` | `build-portable` |
| --- | --- | --- |
| Distro | Debian 13 (via `gcc:14.3.0`) | AlmaLinux 9 |
| glibc floor | **2.41** | **2.34** |
| Artifacts run on | Ubuntu 25.04+, Debian 13+, Fedora 42+ | RHEL/Alma/Rocky 9+, Ubuntu 22.04+, Debian 12+ |
| GCC | 14.3.0, from the upstream image | 14.3.0, bootstrapped from source |
| OpenSSL | Debian 13's | AlmaLinux 9's 3.5.x |
| Build time | minutes | ~1–1.5 h (the bootstrap) |
| Use for | CI, local dev, in-container testing | **binary releases** |

The single fact behind that table: **glibc is backward- but not
forward-compatible.** A binary that references `GLIBC_2.41` symbols fails at
load time on anything older, and the floor is decided by the glibc the image
was built against — not by anything in the source. So the reach of a release
is set by choosing a base image, and `build-base`, descending from
`gcc:14.3.0`, happens to sit on the newest Debian there is. Fine for CI;
unusable for distribution.

Both are kept because they are two different execution contexts, and a
toolchain-shaped regression that only shows up on one glibc/libstdc++
generation is then caught by whichever job sees it.

Why 2.34 and not lower: AlmaLinux **8** (glibc 2.28) reaches further and is
the manylinux_2_28 baseline, but only ships OpenSSL 1.1.1 — EOL since 2023,
and linking it stamps a `libssl.so.1.1` SONAME that no current distro
provides, so compatibility breaks at the other end. Reaching 2.28 means
building OpenSSL from source as well; 2.34 gets a supported system OpenSSL for
free. The `manylinux` images themselves were rejected for a different reason:
7× the size of an `almalinux` base, and the excess is CPython versions and
`auditwheel`, neither of which a C++ tree consumes.

### Two things this image deliberately does not do

**It does not use `gcc-toolset-14`.** AlmaLinux 9 packages GCC 14.2.1 as an
SCL, which would be far cheaper than a source bootstrap. It is not used
because (a) the plugin admission fingerprint hashes the *complete* compiler
version, so 14.2.1 would make this image a different execution context from
`build-base`'s `GNU-14.3.0` for no reason, and (b) `gcc-toolset` links new C++
symbols in statically from `libstdc++_nonshared.a`, leaving artifacts
dependent on the *host's* old `libstdc++.so.6`. Ordinary programs never
notice; this tree passes C++ types across `dlopen` boundaries and compares
typeinfo identity, which is exactly where duplicated library internals bite.
A source build yields a complete standalone `libstdc++.so.6` to ship instead.

**It does not statically link libstdc++.** `-static-libstdc++` is the usual
answer to "my binary needs a newer libstdc++ than the target has", and it is
wrong here for the same reason: host and plugin would each get a private copy
of the vtables and typeinfo, and cross-DSO RTTI and exception propagation stop
working. Ship the `.so`, do not embed it. The image collects what a release
needs to carry in **`/opt/simplex-runtime`** (`libstdc++.so.6`,
`libgcc_s.so.1`, `libboost_*.so*`), so the release step is a copy rather than
a hunt, and the dependency set is recorded in the image instead of in
someone's notes.

### The portability floor test

`portability_floor.cmake` is registered as the `portability_floor` ctest when
`-DSIMPLEX_GLIBC_FLOOR=<version>` is set (off by default — the local WSL build
and `build-base` both sit far above any release floor, and failing them over it
would be noise):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DSIMPLEX_THIRDPARTY_DIR=/opt/simplex-thirdparty \
    -DSIMPLEX_GLIBC_FLOOR=2.34 -DSIMPLEX_STRICT_NEEDED=ON
cmake --build build -j4 && ctest --test-dir build -R portability_floor
```

It scans every ELF in the build tree with `readelf` and fails if any references
a glibc symbol version above the floor, naming the offending symbols — the fix
depends on which call pulled the node in. `SIMPLEX_STRICT_NEEDED=ON` also
asserts every `DT_NEEDED` is universally present, shipped in
`/opt/simplex-runtime`, or a documented external requirement (OpenSSL 3).

It catches a real and otherwise-invisible class of regression, e.g. the local
Ubuntu 24.04 build requires `__isoc23_strtoll@GLIBC_2.38` — a *header-driven
redirect*, not a source-level choice: building against glibc ≥ 2.38 headers
silently rewrites `strtoll` to its C23-conformant alias. Nothing about the
build fails, and the artifact simply will not load on AlmaLinux 9. Only an
older base image fixes it, which is what this one is.

What it does **not** prove: that the release runs. Symbol versions are
necessary, not sufficient — kernel requirements and runtime behaviour are out
of scope, and actually executing the artifacts on a target distro is a
separate, complementary test. This is the cheap gate that runs on every commit.

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
| Boost | 1.91.0 **shared** | `/usr/local` | Built with exactly `filesystem` + `process` + `test` (= unit_test_framework): headers, `libboost_*.so.1.91.0`, and the CMake config package under `/usr/local/lib/cmake/Boost-1.91.0` — `find_package(Boost)` needs no `BOOST_ROOT`. |
| nlohmann/json | 3.12.0 (single header) | `/opt/simplex-thirdparty/include/nlohmann/json.hpp` | |
| yaml-cpp | 0.9.0, static **PIC** | `/opt/simplex-thirdparty/libs/libyaml-cpp.a` | PIC because it reaches MODULE plugin `.so`s through `simplex_thirdparty_iface`. |

`/opt/simplex-thirdparty` mirrors the tree's `third_party/{include,libs}`
layout, so the tree consumes it unchanged via
`-DSIMPLEX_THIRDPARTY_DIR=/opt/simplex-thirdparty`. Version pins follow
`third_party/versions/*.md` — that directory stays the single source of
truth; when a version moves, both this image and the records move with it.

Deliberately **absent**: any source code; tree-sitter and Boost
`program_options` (their only consumer was the legacy languages/indextools
domain; `process` is back — the new core `process` module's manager builds
on it).

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

### Publishing the portable base to ghcr (manual)

Same flow, one extra step: the CI job that consumes it pins the image by
digest, so the digest has to be read back after the push and pasted into
`.github/workflows/ci.yml`.

```bash
# ~1-1.5 h: the GCC bootstrap dominates. JOBS defaults to 4; raise it only if
# the host can spare the cores (see the WSL note at the end of this file).
docker build -f docker/Dockerfile.build-portable \
    --build-arg JOBS=4 \
    -t ghcr.io/hazer-bjtu/simplex-cpp/build-portable:glibc2.34-gcc14.3 .

echo <PAT> | docker login ghcr.io -u Hazer-BJTU --password-stdin   # write:packages
docker push ghcr.io/hazer-bjtu/simplex-cpp/build-portable:glibc2.34-gcc14.3

# then pin it in the workflow:
docker buildx imagetools inspect \
    ghcr.io/hazer-bjtu/simplex-cpp/build-portable:glibc2.34-gcc14.3
# -> paste the Digest into the portable-release job's `image:` as @sha256:…
```

The tag encodes both halves of what the image promises — the glibc floor and
the compiler — because those are the two things a consumer needs to know and
the two things a rebuild could silently change. `latest` is deliberately not
published for this image: "latest portable base" is not a meaningful thing to
depend on when the whole point is a specific floor.

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
  `extensions/plugin_magic.hpp` rejects exactly that mixing, by
  design. Same context, not portable.
- Build jobs are capped at `-j4` even inside the container: it shares the
  host's CPUs, and saturating them destabilizes the WSL services.
