# Ordinary CI job selection

The PR/main workflow stays unconditional. `changes` computes one category from a
complete local Git diff and derives all native producer/consumer decisions.
`hub-test` and `hub-panel` always run their full existing suites.

| Category | `build-test` Debug/Release | `portable-release` | `staged-runtime` Ubuntu/AlmaLinux | `hub-e2e` |
| --- | --- | --- | --- | --- |
| Native/runtime or unknown | Run | Run | Run | Run |
| Hub integration | Skip | Run | Skip | Run |
| Independent docs/panel | Skip | Skip | Skip | Skip |
| Content-verified version-only | Skip | Skip | Skip | Skip |

## Verified version-only bypass

`version-only.mjs` checks content before applying the ordinary path rules. The
bypass applies only when the complete PR/main comparison changes **exactly**
`VERSION`, `hub/package.json`, and `hub/package-lock.json`. All three must be
existing regular files modified in place with unchanged modes. Additions,
deletions, copies, renames, symlinks and mode/type changes cannot qualify.

Both revisions must contain synchronized, valid `MAJOR.MINOR.PATCH` versions
(no leading zeros), and the old and new versions must differ. As in the repository
version checker, surrounding whitespace in `VERSION` is ignored. Only these JSON
values may change:

- `hub/package.json.version`
- `hub/package-lock.json.version`
- `hub/package-lock.json.packages[""].version`

Every other JSON value must be unchanged, including scripts, engines, dependency
versions, lockfile format, integrity and resolution data. JSON whitespace and
object key order are ignored; strings compare by decoded value, while array order
and value types are preserved. Number tokens must keep the same spelling: for
example, `1` to `1.0` conservatively rejects the bypass. This also prevents
JavaScript integer rounding from concealing changes to large numeric values.
Duplicate decoded keys at any depth, malformed JSON, invalid UTF-8 and a JSON BOM
are rejected. The verifier reads Git blobs without executing their contents;
metadata reads are bounded to 16 MiB and JSON nesting to 128 levels.

False eligibility uses the existing path rules, which still classify `VERSION`
as native and Hub manifests as integration. Unreadable or ambiguous metadata
selects the full-validation fallback. Extra paths, including documentation, make
the bypass ineligible. A final version bump cannot hide earlier implementation
changes in the same event range. `versioning/**` and CI automation remain native.

For eligible changes, all four worker-dependent job groups are skipped together;
no consumer downloads a missing artifact or substitutes an older worker. The
unconditional `changes` job still runs classifier/gate/workflow regression tests
and `node versioning/sync_version.mjs --check`. Full Hub and panel suites still
run. The summary states **content-verified version-only bypass** and the version
transition. `version_from` and `version_to` outputs are mandatory for this category;
the gate rejects missing/invalid transitions, inconsistent decisions and failed
or cancelled retained jobs.

This exemption applies only to ordinary pull requests and pushes to `main`.
Tag-triggered release verification, LTO builds, packaging, npm publishing,
recovery and documentation workflows are unchanged. A release tag still validates
the actual tagged version through the complete release workflow.

## Rules and range handling

`selection.mjs` is the single rule source. All files under the native package
trees, `third_party/`, `cmake/`, and `docker/` trigger full native validation,
including README files, schemas, skills, prompts, config templates and scripts.
Root `CMakeLists.txt`, `VERSION`, `ci.yml`, and everything in `.github/ci/` do too.
No per-package target selection or extension-based exclusions are used.

`hub/web/**` and `hub/test/browser/**` are panel-only. Other `hub/**` paths select
Hub integration, including shared protocols, launch config, test helpers,
fixtures, package locks and build/test configuration. The broad boundary is
intentional; it can be refined after observing actual runs.

Root `docs/**`, `assets/**`, `README.md`, `CONTRIBUTING.md`, `SECURITY.md`,
`LICENSE`, `.gitignore`, `.github/ISSUE_TEMPLATE/**`,
`.github/PULL_REQUEST_TEMPLATE.md`, and the dedicated docs/release workflows do
not select native work. These community-file exceptions do not exclude other
`.github/**` files; unknown workflows and automation still select full validation.
Docs and release workflow behavior is unchanged.

`changes.mjs` separates range resolution from NUL-delimited name-status parsing.
PRs compare the target merge base to the checked-out PR revision, including all
PR commits. Main pushes compare event `before` to `after`, including squash,
rebase and multi-commit merges/pushes. Both rename paths and deleted inputs are
classified. Checkout uses full history. Missing/zero/unavailable SHAs, unsupported
events, malformed diff data or checkout mismatches fall back to full validation.
Invalid job-output combinations fail instead of being silently accepted.

Every run prints changed paths (JSON-escaped) and writes a selection summary.
Consumers only download current-run artifacts after a successful selected
`portable-release`. There is no cross-run worker reuse.

## E2E boundary

`hub-e2e` runs `npm run test:e2e`: Hub and real worker integration with the offline
provider. It has no browser dependency. `hub-panel` runs browser tests against the
stub Hub; `npm run check:panel` remains a manual real-browser acceptance check.

`SIMPLEX_E2E_REQUIRED=1` makes the ordinary E2E helper reject a missing or
non-executable worker, missing default/compact prompt, missing mock-provider or
process declarations, and a worker that cannot start because its shared runtime
cannot load. Selected CI fails rather than passing a skipped suite. Local tests
retain convenient skipping when the worker/default prompt is unavailable.

`test/e2e/docker-worker.test.js` remains independently opt-in with
`SIMPLEX_DOCKER_WORKER_TEST=1`; it requires a separately built Docker image and a
non-root host Hub. It is not made mandatory by the ordinary CI strict flag.

## Stable required check

The final **CI gate** runs with `if: always()` and checks classifier output plus
all actual job results. Selected jobs (including the always-selected Node jobs)
must succeed. Unselected jobs must be skipped. Failure, cancellation, invalid
outputs, unexpected success/skip, or a missing result fails the gate.

The `main` ruleset inspected on 2026-10-02 currently requires individual checks:
`build-test (Debug)`, `build-test (Release)`, `portable-release (Release)`,
`staged-runtime (almalinux:9)`, `staged-runtime (ubuntu:22.04)`, `hub-e2e`, and
`hub-panel`. This implementation does not modify GitHub repository settings.

After verifying the first full-validation PR, the repository administrator should
add **CI gate** as a required check (GitHub Actions integration), then remove the
conditionally scheduled individual checks. `hub-panel` can remain separately
required if desired; the gate already covers it and both `hub-test` matrix legs.
Do not remove the old requirements before the new gate has reported successfully.
A wholly cancelled workflow cannot report a successful gate and cannot satisfy
this new requirement.

## Validation and measurement

All workflows use `actions/checkout@v4` and `actions/setup-node@v4`, matching the
existing native CI and release baseline. A future upgrade should update every
workflow together after validating the runner and container requirements.
`workflows.test.mjs` checks that each action uses the same reference throughout
the workflow directory; it runs in the `changes` job before classification.

The Hub's `test/configurations.test.js` checks exact equality between the
canonical `load/schemas/config.example.yaml` and bundled `hub/schemas/worker.yaml`.
The always-selected `hub-test` job runs this check on both supported Node versions.
Update both files together when changing the worker configuration template.

Run CI selection, range, gate and workflow consistency checks without any package
dependencies:

```sh
node --test .github/ci/*.test.mjs
```

Run worker admission regression tests after installing Hub dependencies:

```sh
node --test hub/test/worker-preconditions.test.js
```

For real rollout verification, compare docs-only, panel-only, Hub-integration,
native, verified version-only and mixed PR/main changes. Use the selection summary
and the Actions job list to confirm the table, intentional consumer skips and
retained full native matrices. Record total runner minutes and wall time separately; local classifier
tests demonstrate scheduling decisions, not measured production cost savings.
