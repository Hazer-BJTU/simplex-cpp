# Musl portable worker build

This Linux x86_64 variant bundles musl's loader, GCC/Boost runtimes, OpenSSL and
fallback CA data. It can be unpacked into an arbitrary directory on a compatible
Linux system, including glibc distributions without a musl installation. Keep
the whole directory together; installation paths must not contain `:` because
the loader treats it as a library search-path separator. Bash and normal shell utilities (`dirname`,
`readlink`) are required; external tools used by tasks are not bundled.

The public `simplex` router is shared by both package variants. The Hub installer
selects this package with `--platform linux-amd64-bundled`; use
`--platform linux-amd64-standard` for the glibc package. Older Hub versions
without platform selection require manual musl download.
The musl archive is a separate GitHub Release asset, also buildable locally or
through the manual validation workflow below. Only plugins compiled in the same musl build context
are supported.

## Manual CI validation

In GitHub Actions, select **Musl portable validation** → **Run workflow** and
choose the branch to validate. After this workflow is available on the default
branch, the equivalent CLI command is:

```bash
gh workflow run musl-portable.yml --ref main
gh run list --workflow musl-portable.yml
```

Daily development uses `workflow_dispatch` only: PRs and merges to `main` do
not start musl validation. Tag releases reuse it through `workflow_call` and
wait for all three stages before publishing. The release workflow combines the
glibc and musl runtime archives with one `SHA256SUMS`; validation-only files
are excluded. Neither validation path publishes images.

Its three stages use the same portable archive:

1. Build **Release** in the published musl base, pinned by digest. Run
   the complete CTest suite with per-test timeouts, then test non-root packaging,
   checksum validation, relocation and rejection of missing license notices.
2. Download that archive on **Ubuntu 22.04 and AlmaLinux 9**. Validate it as a
   non-root user without installing musl or a compiler. Exercise relocated CLI
   and worker lifecycle, then load installed schemas, prompts, providers, dynamic
   tools and loop hooks through separately delivered test executables. Build-time
   `/src` and `/build` paths are absent, preventing source fallback.
3. Run the Hub's offline E2E suite against the extracted worker on the GitHub
   runner, with missing worker prerequisites treated as failures.

Actions run on the glibc host; Docker isolates the musl toolchain. There is no
Debug build. The `musl-portable` Actions artifact is retained for seven days and
contains the worker `.tar.gz` and `.sha256`, plus `musl-test-support.tar.gz` and a
launcher probe used only by validation. Install only the worker archive; the
support binaries are not part of the distribution.

The workflow uses `docker/Dockerfile.musl-test`, with whole-project
`SIMPLEX_ENABLE_LTO=OFF` (individual targets retain their existing LTO settings).
An experiment with global LTO on this pinned Alpine/GCC toolchain failed when
linking `llm_deepseek_chat`: the fortify `vsnprintf` wrapper could not satisfy an
`always_inline` call. This workflow does not disable fortify or change compiler
policy to work around that incompatibility. Global LTO needs separate validation
before enabling it for musl.
Update the workflow's pinned `BUILD_IMAGE` digest when publishing a new tested
base. Cross-distribution containers share the runner kernel; this is not an
older-kernel compatibility guarantee. Offline E2E does not test live providers.

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

Replace `vX.Y.Z` below with the version of the archive you built or downloaded.

```bash
cd musl-release
sha256sum -c simplex-worker-v*.tar.gz.sha256
tar -xzf simplex-worker-vX.Y.Z-linux-x86_64-musl.tar.gz
mv simplex-worker-vX.Y.Z-linux-x86_64-musl "$HOME/my simplex worker"
"$HOME/my simplex worker/bin/simplex" --version
"$HOME/my simplex worker/bin/simplex" run --help
"$HOME/my simplex worker/bin/simplex" run \
  --config /absolute/path/config.yaml --session example
```

For GitHub Release downloads, verify the shared manifest with
`sha256sum --ignore-missing -c SHA256SUMS` instead of the local build's
`.tar.gz.sha256` file. Use the archive's actual version in these commands. Both `bin/simplex` and
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
- Ubuntu 22.04 and AlmaLinux 9 both passed the complete non-root archive,
  relocation, worker lifecycle and installed-resource/plugin checks.
- Offline Hub E2E passed 16 tests against the extracted archive; optional Docker
  ownership suites were not enabled. Four focused glibc regressions also passed.
- Explicit-loader executable discovery and the native glibc helper probe passed.

The dependency allowlist now recognizes the musl SONAME. Relocation tests use paths containing
spaces and Unicode, unrelated working directories and symlink/PATH entry points,
and exercise real provider loading, worker shutdown, pipe/TTY lifecycle,
argument/PID preservation and explicit/default trust selection.

Container tests share the host kernel and do not establish older-kernel support.
The workflow covers offline Hub E2E; real-provider inference and opt-in Docker
ownership tests remain separate. The Hub installer supports musl through the explicit bundled platform tag.

## Boost.Test signal stacks on CI hosts

The test image rebuilds only Boost.Test 1.91.0 from its checksum-pinned source
with a Linux signal-stack sizing patch. musl defines a fixed `SIGSTKSZ`, but
some host kernels require a larger signal frame. Registration then fails with
`ENOMEM` before tests run, even when plenty of memory is available. The patch
adds the kernel's `AT_MINSIGSTKSZ` to the handler's `SIGSTKSZ` allowance for
both allocation and registration. An absent auxv entry contributes zero.

A fault-injection regression simulates a large frame and checks that passing
tests start while assertions and fatal signals still fail. The same patched
test library is staged for cross-distribution test executables; production
worker behavior, entry points, and signal handling are unchanged. The published
base image need not be replaced for this correction.

The tag release reuses `musl-portable.yml`, including the patched test-image
build; it does not package directly from the unpatched base. The large-frame
regression runs against both the build library and the installed library via
the bundled loader. It asserts that the latter actually resolves Boost.Test
from the staging tree, preventing a build-host fallback from hiding a stale or
missing bundled copy.

Both archive formats and the Hub installer require at least one nonempty
`libllm_<name>.so` provider and reject any matching empty or non-file entry.
This is a presence check, not an ABI or runtime-loading guarantee. The musl
packager checks `DT_NEEDED` closure for shipped ELF files; arbitrary `dlopen`
requests are outside that static check. Relocated worker/plugin tests provide
separate runtime coverage.
