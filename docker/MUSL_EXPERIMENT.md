# Local musl experiment

This is an experimental Linux x86_64 build, not a new release format. It does
not change the production launcher, downloader, application, or workflows.
The existing glibc build remains supported as before.

## Build

Run from the repository root. The proxy variables are Docker build arguments,
not persistent image environment variables. Both cases are needed by the
package manager and download utilities.

```bash
# Optional: source your proxy configuration before these commands.
# source /path/to/setup_proxy.sh
export http_proxy="$HTTP_PROXY" https_proxy="$HTTPS_PROXY"
docker build --network host \
  --build-arg HTTP_PROXY --build-arg HTTPS_PROXY \
  --build-arg http_proxy --build-arg https_proxy \
  -f docker/Dockerfile.build-musl -t simplex-build-musl:experimental .
docker build --target build -f docker/Dockerfile.musl-test \
  -t simplex-musl-build-test:experimental .
docker build -f docker/Dockerfile.musl-runtime \
  -t simplex-musl-test:experimental .
```

The base image pins Alpine by digest, but its package repositories remain
mutable in this first experiment. `/opt/simplex/runtime-packages.txt` records
installed package versions. Boost 1.91.0, JSON 3.12.0 and yaml-cpp 0.9.0 match
the repository. All C++ code and plugins use the same Alpine GCC toolchain.
The project-wide LTO option is off; target-local LTO settings are unchanged.

## Run

```bash
docker run --rm simplex-musl-test:experimental --version
docker run --rm simplex-musl-test:experimental run --help
# Mount a worker configuration and use its configured endpoints/storage:
docker run --rm -it --network host \
  -v /absolute/path/config.yaml:/config.yaml:ro \
  simplex-musl-test:experimental run --config /config.yaml --session musl-test
```

The runtime image is Ubuntu 22.04. It receives the staged worker plus musl,
libstdc++, libgcc, Boost, OpenSSL and a CA bundle; it does not install a host
musl package. The original Bash launcher remains in place.

For offline regression tests:

```bash
docker run --rm simplex-musl-build-test:experimental \
  ctest --test-dir /build --output-on-failure --timeout 120
```

## Scope and known limitations

- **Fixed prefix:** executable ELF interpreters are patched in the experiment
  to `/opt/simplex/lib/ld-musl-x86_64.so.1`. This permits the original launcher
  and executable-relative resource discovery to work. It does not establish
  arbitrary-directory archive portability. Direct invocation of the bundled
  loader must separately check executable path discovery before adoption.
- Shared libraries use installed `$ORIGIN` paths. Do not export a musl
  `LD_LIBRARY_PATH` into host commands spawned by the process tools.
- The runtime uses the packaged CA bundle via `SSL_CERT_FILE`. User overrides
  can be supplied with Docker `-e`. Host trust-store discovery, OpenSSL
  configuration/provider-module policy and certificate update policy are not
  implemented by this experiment. No FIPS or legacy-provider support is claimed.
- Only plugins compiled for this musl build context are supported. Existing
  glibc plugins are not interchangeable.
- The build/test image includes the test executables and sources; the runtime
  image contains only the installed worker tree and its bundled dependencies.
- Container tests share the host kernel; they do not establish compatibility
  with older kernels. Download selection, checksums/manifests, licenses and
  release CI need a separate production-packaging pass before publication.

## Loader-location probe

A minimal program calling the same `boost::dll::program_location()` used by
`utils/subprocess` reported `/tmp/location` on normal execution, but
`/lib/ld-musl-x86_64.so.1` when invoked explicitly through the loader. This
confirms that replacing the launcher's `exec` with an explicit loader command
alone would break executable-relative resource lookup. The fixed PT_INTERP
experiment avoids that change.

The tested source snapshot is `83654ed` (the model-tool-schema changes in PR #152);
this experiment does not modify that PR or publish images.

## Results (2026-10-12)

- GCC 15.2.0, musl 1.2.6, OpenSSL 3.5.9, Boost 1.91.0.
- Full Release build passed without application-source changes.
- Original CTest suite: **104/105 passed**, in about 61 seconds. The single
  failure is `llm_plugin_boundary_hygiene`: its glibc-only SONAME allowlist
  rejects `libc.musl-x86_64.so.1` for all three model plugins. In a disposable
  container, adding only that exact name made the structural check pass.
  The repository test and locally built experimental images retain the original
  check, so running the command above still reports that known failure.
- Additional local TLS SSE executable (`sse_request_e2e`) passed.
- Ubuntu 22.04: the unchanged launcher runs `--version` and `run --help`.
  Loader inspection resolves the worker and all five installed plugins using
  the bundled runtime, including libssl/libcrypto, without system musl.
- AlmaLinux 9: installed-worker CLI checks, original-launcher CLI checks,
  launcher relocation/symlink/exec tests, and the real-worker lifecycle suite
  passed. The lifecycle suite covers ownership, TTY recovery/non-aliasing,
  terminal shutdown and descendant-held pipe lifetime.
- Installed tree: approximately **32 MiB**, including approximately 11.5 MiB
  of selected third-party runtimes. No production archive-size claim is made.
- Restoring execute permission on the bundled loader is essential: CMake's
  library-directory install otherwise creates it with mode 0644. The test
  Dockerfile explicitly restores mode 0755 after installation.

Full Hub JavaScript E2E and real-provider inference were not run in this
experiment. CA fallback/enterprise trust, old-kernel coverage and movable
archive installation remain future work; successful container tests do not
establish those properties.
