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
