# Musl portable worker build

This Linux x86_64 variant bundles musl's loader, GCC/Boost runtimes, OpenSSL and
fallback CA data. It can be unpacked into an arbitrary directory on a compatible
Linux system, including glibc distributions without a musl installation. Keep
the whole directory together. Bash and normal shell utilities (`dirname`,
`readlink`) are required; external tools used by tasks are not bundled.

The existing glibc release, public `simplex` router, downloader and CI workflows
are unchanged. The musl archive is a separate manually built artifact. Only
plugins compiled in the same musl build context are supported.

## Build the base image

From the repository root:

```bash
# Optional: source your own proxy setup first.
# source /path/to/setup_proxy.sh
export http_proxy="${HTTP_PROXY:-}" https_proxy="${HTTPS_PROXY:-}"
docker build --network host \
  --build-arg HTTP_PROXY --build-arg HTTPS_PROXY \
  --build-arg http_proxy --build-arg https_proxy \
  -f docker/Dockerfile.build-musl -t simplex-build-musl:experimental .
```

The base pins Alpine by digest and verifies downloaded Boost, JSON and yaml-cpp
sources with SHA-256. Alpine package repositories remain mutable: before
publishing a tested base, retain its digest and `runtime-packages.txt` and use
that digest for subsequent worker and third-party plugin builds. The initial
combination is GCC 15.2.0, musl 1.2.6, OpenSSL 3.5.9 and Boost 1.91.0. Review the
bundled notices when upgrading these packages. Do not mix glibc plugins with
musl or assume plugins from another GCC/Boost build are ABI compatible.

The image provides `/opt/simplex-thirdparty`, `/opt/simplex-runtime` and
`/opt/simplex-runtime-licenses`, plus `/opt/simplex-thirdparty-licenses`. Configure the project with
`-DCMAKE_CXX_COMPILER=g++` explicitly: the repository's unchanged default
compiler name is `g++-14`, while Alpine supplies `g++`.

## Build, test and package the worker

```bash
docker build --target build -f docker/Dockerfile.musl-test \
  -t simplex-musl-build-test:experimental .
docker run --rm simplex-musl-build-test:experimental \
  ctest --test-dir /build --output-on-failure --timeout 120
# Verify executable discovery when the kernel's executable is the loader:
docker run --rm simplex-musl-build-test:experimental \
  /opt/simplex/lib/ld-musl-x86_64.so.1 --library-path /opt/simplex/lib \
  /build/bin/test_executable_location /build/bin/test_executable_location
# Move the installation, exercise the real worker, and inspect launcher behavior:
docker run --rm simplex-musl-build-test:experimental \
  python3 /src/docker/test-musl-runtime.py /opt/simplex /src \
  /src/docker/musl-launch-probe
# Package, verify checksums, extract, relocate, and check missing-license rejection:
docker run --rm simplex-musl-build-test:experimental \
  bash /src/docker/test-musl-package.sh
mkdir -p musl-release
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD/musl-release:/output" simplex-musl-build-test:experimental \
  bash /src/docker/package-musl-worker.sh /opt/simplex /output
```

`Dockerfile.musl-test` accepts `BUILD_IMAGE` and `JOBS` build arguments (defaults:
`simplex-build-musl:experimental`, 2). Project-wide LTO is off in this test
recipe; target-local settings are unchanged. The build image retains test
executables and sources; the archive contains the installed worker tree only.
Packaging checks the compiled version and each ELF object's `DT_NEEDED` entries
against the bundled libraries, including modules loaded by plugins. It emits
`simplex-worker-v<VERSION>-linux-x86_64-musl.tar.gz` and a `.tar.gz.sha256` file.
The existing glibc packager and Hub download selection are not changed.

A custom build can use the same pipeline: install with
`-DSIMPLEX_BUNDLE_RUNTIME=/opt/simplex-runtime`, then run
`bash docker/prepare-musl-runtime.sh STAGING_DIR` inside the base image. Always
use a fresh staging directory. Preparation is intentionally rejected if it has
already been applied. Keep the repository's installed `$ORIGIN` library paths.

## Run from any directory

```bash
cd musl-release
sha256sum -c simplex-worker-v*.tar.gz.sha256
tar -xzf simplex-worker-v0.5.1-linux-x86_64-musl.tar.gz
mv simplex-worker-v0.5.1-linux-x86_64-musl "$HOME/my simplex worker"
"$HOME/my simplex worker/bin/simplex" --version
"$HOME/my simplex worker/bin/simplex" run --help
"$HOME/my simplex worker/bin/simplex" run \
  --config /absolute/path/config.yaml --session example
```

Use the archive's actual version in these commands. Both `bin/simplex` and
`bin/simplex_worker` can be placed on PATH or invoked through symbolic links.
Relative `--config` paths and task paths remain relative to the caller's working
directory. Configuration, prompt and plugin discovery still use the installed
application directory. Copy `bin/config.example.yaml` and fill in provider and
Hub endpoints before starting a real session.

For a glibc-based test container:

```bash
docker build -f docker/Dockerfile.musl-runtime \
  -t simplex-musl-test:experimental .
docker run --rm simplex-musl-test:experimental --version
docker run --rm -it --network host \
  -v /absolute/path/config.yaml:/config.yaml:ro \
  simplex-musl-test:experimental run --config /config.yaml --session musl-test
```

Container filesystem changes are ephemeral with `--rm`; mount persistence and
workspace directories explicitly when needed. Arbitrary installation paths do
not remove Linux kernel, architecture, filesystem executable-permission or
external-tool requirements. These artifacts are not macOS/Windows executables.

## How relocation works

`bin/simplex` stays unchanged. In the musl package only, `bin/simplex_worker`
is a Bash wrapper and `bin/simplex_worker.bin` is the actual ELF executable.
The wrapper resolves its own symlinks, locates `../lib/ld-musl-x86_64.so.1` and
executes it with an absolute worker path and `--library-path ../lib`. No wrapper
shell survives, so the worker keeps the PID and receives signals directly.
It preserves arguments and the caller's working directory. It does not export
`LD_LIBRARY_PATH` or `LD_PRELOAD` to commands launched by process tools.

The kernel reports the loader in `/proc/self/exe`. `utils/executable` therefore
uses the main executable's program-header entry on musl, with Boost.DLL as the
native-platform fallback. Both startup asset discovery and the model dispatcher
use this helper. No caller-controlled environment override is needed. Installing
musl globally or patching an absolute ELF interpreter after each move is not
required. Do not launch `simplex_worker.bin` directly on a host without musl.

## TLS and updates

The wrapper preserves explicitly provided `SSL_CERT_FILE`, `SSL_CERT_DIR` and
`OPENSSL_CONF`, including directory-only CA overrides. Otherwise it chooses a
readable common host CA bundle, falling back to `share/certs/ca-certificates.crt`.
It selects the package's minimal OpenSSL configuration to avoid implicitly
loading host-specific provider modules into the bundled library. Ordinary TLS
uses OpenSSL's built-in default provider. FIPS and legacy provider modules are
not bundled or claimed to be supported. TLS peer verification remains enabled.

The chosen CA/config environment is inherited by child processes. Users who
supply custom OpenSSL configurations must ensure any requested provider modules
match the bundled library. Library versions, source notices and fallback roots
travel with the installation; updating the host's OpenSSL does not update the
packaged copy. Rebuild and redistribute the worker package for runtime security
fixes and periodically refresh fallback roots.

## License notices

Every prepared installation, runtime image and binary archive retains the
project's root `LICENSE` (MIT), the repository's `third_party_licenses/` (Boost,
JSON and yaml-cpp), and `share/licenses/` (musl copyright, OpenSSL Apache 2.0,
GCC GPLv3 plus the Runtime Library Exception, and Mozilla CA data MPL 2.0).
The runtime notice README records upstream sources; `runtime-packages.txt`
records the build image's Alpine package versions. The package command refuses
to create an archive if any expected notice is missing or empty, and the
relocation test checks the extracted installation's notices too.

## Validation

Validated on 2026-10-13 with worker version 0.5.1:

- All 106 CTest tests passed in the final musl build environment.
- Non-root archive creation, checksum verification, extraction, relocation and
  missing-license rejection passed; the archive is approximately 9.5 MiB.
- Ubuntu 22.04 passed relocated CLI checks; AlmaLinux 9 passed the complete
  relocation and real-worker lifecycle suite as an unprivileged user.
- Explicit-loader executable discovery and the native glibc helper probe passed.

The dependency allowlist now recognizes the musl SONAME. Relocation tests use paths containing
spaces and Unicode, unrelated working directories and symlink/PATH entry points,
and exercise real provider loading, worker shutdown, pipe/TTY lifecycle,
argument/PID preservation and explicit/default trust selection.

Container tests share the host kernel and do not establish older-kernel support.
Full Hub JavaScript E2E and real-provider inference are separate from these
local packaging checks. No release workflow or automatic musl download choice
is introduced by this PR.
