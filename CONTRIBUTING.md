# Contributing to Simplex

Use an issue to discuss a substantial behavior or interface change before
implementing it. Bug reports should include a minimal reproduction; feature
requests should explain the problem and intended scope. Report vulnerabilities
privately through the [security policy](SECURITY.md).

## 1. Prepare the toolchain

The reference native environment uses GCC 14.3.0, C++20, CMake 3.20+, Boost 1.91.0
(shared libraries), OpenSSL 3, nlohmann/json 3.12.0, and yaml-cpp 0.9.0 (static,
with PIC). Build the worker and native plugins with the same compiler and runtime.

Follow the [local build guide](docs/building/local.md) to prepare dependencies.
The [published Docker build images](docs/building/docker.md) provide an alternative
with the pinned CI execution context. Keep local and container build directories
separate; never reuse a CMake cache across compiler or dependency environments.

The Hub and documentation site require Node.js 22.18+ and npm. They are separate
from the native build.

## 2. Build

From a fresh checkout, after preparing the private dependencies as described in
the local guide:

```sh
export SIMPLEX_DEPS="$PWD/.local-deps"
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_PREFIX_PATH="$SIMPLEX_DEPS/boost" \
  -DSIMPLEX_THIRDPARTY_DIR="$SIMPLEX_DEPS/third-party"
cmake --build build -j2
```

Tests are enabled by default. Run native binaries and tests in the environment
that built them. For installation, runtime library paths and portable packaging,
use the build guides above rather than copying only the worker executable.

## 3. Validate the affected parts

Run the checks relevant to the change and include the commands and results in the
PR. State any checks you could not run and why.

**Worker and native plugins:**

```sh
ctest --test-dir build --output-on-failure --no-tests=error --timeout 120
```

**Hub and panel** (from `hub/`):

```sh
npm ci --ignore-scripts
npm run typecheck && npm run build && npm test
```

For panel changes, also run the [browser checks](hub/README.md). Real-worker E2E
requires a built worker and runtime resources; the Docker lifetime test is opt-in.
See the [CI selection and E2E boundaries](.github/ci/README.md).

**Documentation** (from the repository root):

```sh
npm ci --ignore-scripts --prefix docs
npm run build --prefix docs
```

The build runs `docs/scripts/check-site.mjs` to check generated pages, local links,
anchors and assets. Review changed prose and commands as well.

**Version alignment** (from the repository root):

```sh
node versioning/sync_version.mjs --check
```

`VERSION` is the shared worker/Hub version source. A normal feature or fix does
not need a version bump; do not edit generated version headers independently.

## 4. Submit a focused pull request

Use descriptive commit titles such as `docs: clarify deployment`,
`fix(core): preserve shutdown state`, or `feat(hub): add session controls`.
Describe the problem, resulting behavior and validation. Link the issue, include
screenshots for panel changes, and update user/protocol documentation when the
public behavior changes. Add or update tests for behavior changes where useful.

PRs trigger ordinary CI and the documentation workflow. CI selects native work
from the complete changed-input range; Node and browser jobs still run in full.
The **CI gate** verifies selected jobs and intentional skips. CI is additional
validation, not a substitute for reporting local checks honestly.

## 5. Develop plugins

Start with the [plugin development workflow](docs/plugins/development.md), then
read the domain guide for [tools](docs/plugins/tools.md),
[loop hooks](docs/plugins/loop-hooks.md), or
[model providers](docs/plugins/model-providers.md). Check
[configuration and loading](docs/plugins/configuration.md) and test the actual
shared-library lifecycle. Preserve the host/plugin ABI and ownership contracts;
rebuild affected components together when changing an interface.

## Release and recovery (owner only)

Publishing is an owner operation, not part of installing or using Simplex.
The implementation is in [release-worker.yml](.github/workflows/release-worker.yml),
[release preparation/finalization](docker/release-worker.mjs),
[recovery validation](docker/verify-release-recovery.mjs), and
[Hub publication](hub/scripts/publish-release.mjs).

### Start a release

1. Update `VERSION` in a version PR and run
   `node versioning/sync_version.mjs --write`, then `--check`. Review the generated
   Hub manifest/lockfile updates. Merge the PR and wait for main CI to pass.
2. Fetch main and tags, verify the intended commit and its version, then push a
   new `vX.Y.Z` tag at that commit. The tag must match `VERSION` and point to a
   commit reachable from `origin/main`. Never move an existing release tag.
3. Watch **Release worker**. The tag pipeline builds and tests the glibc worker,
   the musl worker (through the reusable workflow), and the exact Hub npm archive.
   Both worker variants must pass before combined assets are assembled.
4. The pipeline creates or verifies a **draft** GitHub release containing both
   worker archives and `SHA256SUMS`. It publishes or verifies the tested Hub
   archive on npm, then makes the GitHub release public. GitHub supplies source
   snapshots automatically. A failed npm step leaves the release as a draft.

npm publishing uses the package's configured GitHub trusted publisher and OIDC
(`id-token: write`), with Node 24 and the public registry. Keep the repository
and workflow identity in npm's trusted-publisher settings aligned with the
workflow; do not introduce a personal npm token as a routine workaround.

### Diagnose a partial publication

Inspect the failed job before retrying. A network error does not prove that an
upload or publication failed on the server.

- GitHub preparation compares existing asset bytes with the tested files. It
  uploads missing assets only to a draft; it does not overwrite conflicting
  assets or repair incomplete public releases silently. Finalization verifies
  assets before and after making the draft public.
- npm preflight probes the registry up to three times. A 404 permits publication;
  a transient registry failure does not. If the version already exists, its
  SHA-512 integrity must match the tested tarball.
- `npm publish` is attempted only once per script invocation. Even if it reports
  an error, the script probes afterward because the server may have accepted it.
  Post-publication verification makes up to ten probes, three seconds apart,
  with a ten-second timeout per probe.
- Network/body-read errors, HTTP 408/425/429 and 5xx are retryable probe failures.
  Post-publication 404 also waits for visibility. Authentication failures,
  incomplete/mismatched metadata and integrity conflicts fail immediately.
  Never republish different bytes under an existing npm version.

### Recover using the original tested artifacts

Use manual recovery for a publication-stage failure, not a failed build or test.
The original tag run may have failed overall, but it must be completed and have
successful `validate-tag`, `build`, `build-hub`, both `target-test` jobs and
`assemble-worker-assets`. Assembly also gates all musl validation jobs.
The run must belong to this repository's tag-push release workflow and exactly
match the requested tag and commit.

The `worker-release-assets` and `hub-release-package` artifacts must still be
available (currently retained for seven days). Recovery downloads these exact
artifacts; it does not rebuild the worker or repack the Hub.

If release machinery needs a fix, merge and validate it first. Recovery checks
that the tag is an ancestor of the selected workflow ref, its version still
matches `VERSION`, and every changed path since the tag is explicitly allowed
by `docker/verify-release-recovery.mjs`. This is a narrow list of release
workflow/scripts, their tests and `docs/hub/npm-release.md`, not a general
allowance for documentation or maintenance changes. Even unrelated docs changes
can therefore prevent recovery; inspect the actual diff and validator.

From the repository checkout, replace both placeholders and dispatch on main:

```sh
gh run list --workflow release-worker.yml --limit 10
gh run view SOURCE_RUN_ID --log-failed
gh workflow run release-worker.yml --ref main \
  -f release_tag=vX.Y.Z -f source_run_id=SOURCE_RUN_ID
```

Use the ID of the **original tag-push run**, not a manual recovery run. Inspect
the new run in Actions (or `gh run list`) and follow its `recover` job. Recovery
verifies provenance, prepares/verifies the draft, validates and publishes/verifies
the original npm archive, and finalizes GitHub in that order. Matching existing
npm publication and GitHub assets are accepted, so partial success is recoverable
without replacing them.

If artifacts expired, required build/test jobs failed, application source changed,
or published bytes conflict, do not weaken the checks or substitute a new build.
Fix the problem through review and release a new version/tag instead. After
success, check the public release's two archives and checksums and the matching
npm version. Keep the original failed run and tag for traceability.
