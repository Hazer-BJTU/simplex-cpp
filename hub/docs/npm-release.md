# Publishing the Hub to npm

The public npm package is `@hazer-bjtu/simplex-hub`. It includes the Hub server,
configuration templates, and built browser panel. It does not include the C++
worker. Install the worker separately and point `--worker-bin` or a saved launch
configuration at its executable.

The repository root `VERSION` controls both the worker and Hub versions. A
`v<VERSION>` tag on `main` runs `.github/workflows/release-worker.yml`. After
the worker build and cross-system tests pass, that workflow publishes the Hub
through npm trusted publishing, then publishes the GitHub Release containing
the worker archive. The Hub package is not uploaded as a second GitHub asset.

## First-time npm setup

The package must already exist before npm can accept a trusted publisher. For
the first release, the owner of the `@hazer-bjtu` npm scope should publish a
prerelease from an isolated checkout, leaving the real `0.1.0` for CI:

```sh
git clone https://github.com/Hazer-BJTU/simplex-cpp.git /tmp/simplex-npm-bootstrap
cd /tmp/simplex-npm-bootstrap/hub
npm ci --ignore-scripts --allow-remote=all
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
npm's authentication requirements. `--allow-remote=all` is needed when npm 12
sees registry tarball URLs recorded for a different configured registry in the
lockfile; the lockfile still pins their integrity hashes. Do not manually
publish `0.1.0`.

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
fixes need a new version and tag.

## Package checks

`npm run build:release` builds the browser panel, emits server JavaScript, and
stages the runtime templates beside that JavaScript. Node does not type-strip
`.ts` files inside installed `node_modules`, so the npm package runs compiled
JavaScript even though development uses TypeScript source directly. The package
uses a positive `files` list; `hub/data`, test results, test sources, and local
configuration never enter the npm archive. CI packs the actual archive,
validates its members, installs it, and checks the installed CLI on both the
minimum supported Node version and Node 24.
