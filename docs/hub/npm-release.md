# Publishing the Hub to npm

The public npm package is `@hazer-bjtu/simplex-hub`. It includes the Hub server,
configuration templates, and built browser panel. It does not include the C++
worker. Install the worker separately and point `--worker-bin` or a saved launch
configuration at its executable.

The repository root `VERSION` controls both the worker and Hub versions. A
`v<VERSION>` tag on `main` runs `.github/workflows/release-worker.yml`. After
the worker build and cross-system tests pass, that workflow prepares a draft
GitHub Release with verified worker assets, publishes the Hub through npm
trusted publishing, then makes the draft public. The Hub package is not
uploaded as a second GitHub asset. Rerunning a partial release verifies exact
GitHub asset bytes and npm tarball integrity before continuing; any mismatch
fails rather than replacing a published artifact.

## First-time npm setup

The package must already exist before npm can accept a trusted publisher. For
the first release, the owner of the `@hazer-bjtu` npm scope should publish a
prerelease from an isolated checkout, reserving stable versions for CI. The
existing package has already been bootstrapped; do not repeat this step for
normal releases. For a new package, the initial procedure is:

```sh
git clone https://github.com/Hazer-BJTU/simplex-cpp.git /tmp/simplex-npm-bootstrap
cd /tmp/simplex-npm-bootstrap/hub
npm ci --ignore-scripts --registry=https://registry.npmjs.org
npm pkg set version=0.0.0-bootstrap.0
npm run build:release
archive=$(npm pack --pack-destination /tmp/simplex-npm-bootstrap --ignore-scripts --silent)
node scripts/check-release-package.mjs "/tmp/simplex-npm-bootstrap/$archive"
npm login --registry=https://registry.npmjs.org
npm publish "/tmp/simplex-npm-bootstrap/$archive" \
    --registry=https://registry.npmjs.org --access public --tag bootstrap
```

Before the manual publish, inspect the archive with `tar -tzf` and confirm
that it contains no `data/`, tests, or local configuration. The
publishing account needs permission to publish under `@hazer-bjtu` and must meet
npm's authentication requirements. The lockfile uses the official registry
host and pins each dependency's integrity hash. Stable versions are published
by the release workflow.

After the bootstrap appears on npm, configure its GitHub Actions Trusted
Publisher in the package's npm settings:

| Field | Value |
| --- | --- |
| GitHub owner | `Hazer-BJTU` |
| Repository | `simplex-cpp` |
| Workflow filename | `release-worker.yml` |
| Environment | Leave empty |
| Allowed action | Enable direct `npm publish` |

The publishing job uses `id-token: write` and requires no npm write token in
GitHub Secrets. Configure the trust relationship before pushing the release
tag. Once a version is published, npm does not permit overwriting it; release
fixes need a new version and tag. A failed job may be rerun for the same tag
only when the already-published npm tarball and GitHub assets exactly match
the tested artifacts. If the files differ, create a new version instead.

## Recovering a failed tag run

First retry failed jobs in the original tag workflow. This reuses the same
artifacts and can resume an existing draft or npm publication. If release
machinery itself needs a fix after the tag was pushed, merge that fix to
`main` without changing `VERSION`, then run the same workflow manually:

```sh
release_tag="v$(cat VERSION)"
source_run_id=123456789  # Replace with the original tag workflow run ID.
gh workflow run release-worker.yml --ref main \
    -f release_tag="$release_tag" -f source_run_id="$source_run_id"
```

The manual path uses the worker and Hub artifacts from the named tag run. It
requires that run's tag, source commit, worker build, Hub build, and both
cross-system tests to match and pass; only release machinery may differ on
`main`. It verifies draft assets byte-for-byte and npm's tarball integrity
before continuing. The source run's artifacts must still be retained (seven
days by default). Do not rebuild or repack the same version after publishing.

## Package checks

`npm run build:release` builds the browser panel, emits server JavaScript, and
stages the runtime templates beside that JavaScript. Public protocol and
configuration documents from root `docs/core` and `docs/hub` are copied into
`dist/docs`; historical panel redesign notes are excluded. Node does not type-strip
`.ts` files inside installed `node_modules`, so the npm package runs compiled
JavaScript even though development uses TypeScript source directly. The package
uses a positive `files` list; `hub/data`, test results, test sources, and local
configuration never enter the npm archive. CI packs the actual archive,
validates its members, installs it, and checks the installed CLI on both the
minimum supported Node version and Node 24.
