# Changelog

All notable changes to Simplex are recorded here. This file summarizes the Git
history from the first release (`v0.1.0`) to `v1.1.0` and follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), with `VERSION` as the
shared worker/Hub version source. The code base's origin and the shape of each
release era are described in the
[project history](https://hazer-bjtu.github.io/simplex-cpp/architecture/history).

## [1.1.0] - 2026-10-20

### Added
- SQLite archive index based on FTS5 trigram chunks, plus bounded read-only
  query APIs for searching archived snapshots.
- `archive_memory` intrinsic toolset (`memory_search`, `memory_fetch`) so an
  agent can search and fetch its own archived memory.

### Fixed
- Archive queries wait for transient SQLite store locks instead of failing.

## [1.0.2] - 2026-10-20

### Added
- Compaction keeps the complete summary in persistence while displays render
  bounded previews.

### Fixed
- Stability follow-ups across persistence, core, manager, Hub and docs:
  fail-closed SQLite writes, conflicting backend/format detection, storage
  diagnostics, read-only session locks, and worker archive validation.
- Managed namespace mutations are serialized and busy retries are bounded.

## [1.0.1] - 2026-10-19

### Changed
- Managed launch templates are simpler and redundant deployment copies are
  removed.

## [1.0.0] - 2026-10-18

### Added
- Session storage abstracted behind a backend contract, with SQLite schema v1,
  explicitly confirmed store creation, checkpoints/recovery, and archived
  compact attempts.
- Managed session storage and a complete managed subagent lifecycle in the Hub,
  behind a launcher management transport boundary.
- `simplex_manager`, the local allocation manager for managed workers.

### Changed
- SQLite is the default persistence backend for new sessions.
- Complete session state is archived as JSON; Markdown exports are retired.

## [0.5.8] - 2026-10-16

### Fixed
- Hub: solid danger buttons, a tidier sessions sidebar, aligned force-kill
  client typing, and clearer installer diagnostics.
- LLM: declarative message patches are validated.
- Core: CLI console output is protected and process contracts are clarified.
- Release: provider checks are aligned and the staged Boost.Test build is
  verified.

## [0.5.7] - 2026-10-15

### Added
- Startup command bypass policies for approved commands.
- Declarative Chat Completions fallback provider.

## [0.5.6] - 2026-10-14

### Added
- Contract-only UI SDK, published and verified so independent panel clients can
  depend on the Hub API vocabulary.
- Hub downloads honor environment proxies.

### Changed
- UI contracts are isolated from the panel implementation and documented.

## [0.5.5] - 2026-10-13

### Added
- Hub requires an explicit worker platform selection and installs bundled
  release packages.

## [0.5.4] - 2026-10-13

### Fixed
- Logging survives closed console pipes after a Hub crash.

## [0.5.3] - 2026-10-13

### Fixed
- musl portable builds size Boost.Test signal stacks for the host kernel.

## [0.5.2] - 2026-10-13

### Added
- Relocatable musl worker archive with bundled runtime licenses, validated and
  published alongside the glibc archives.

## [0.5.1] - 2026-10-12

### Changed
- `run_command` and `subagent_send` schemas are simplified.
- Process I/O code is formatted and lifecycle contracts are clarified.

## [0.5.0] - 2026-10-11

### Added
- Process I/O backend with environment and anonymous pipes, a Linux POSIX PTY
  environment, and a shared per-process output budget.
- Tools expose `tty` selection and generic process I/O; terminal lifecycle is
  covered by worker tests.

### Changed
- Platform-dependent process operations are encapsulated, and partial startup
  rolls back cleanly.

## [0.4.6] - 2026-10-11

### Changed
- Panel: display budgets, approval projections, tool calls, and scroll anchors
  are indexed; reader intent is preserved.
- Hub: subagent projection writes are coalesced and subagent history is
  reconciled at run settlement.

### Fixed
- Oversized worker logs keep bounded previews.
- Documentation and protocol maintenance audits are closed.

## [0.4.5] - 2026-10-10

### Fixed
- Panel restores replay state after display eviction and correlates legacy
  tools without guessing result order.
- History reconciles input and response execution identities.

## [0.4.4] - 2026-10-10

### Fixed
- Hub lifecycle recovery converges and cleanup retries are bounded.
- Persisted Docker management environment is minimized.
- Subagent receive sources, rendering budgets, and diagnostics are aligned.
- Worker protocol tables and tool declaration diagnostics are in sync.

## [0.4.3] - 2026-10-09

### Fixed
- Approval argument previews stay useful.
- Complete compact summaries survive display boundaries.
- Worker survives event congestion and isolates query traffic.
- Compact budgets are preflighted and failed attempts pruned.

## [0.4.2] - 2026-10-08

### Added
- Worker version is exposed to clients and agent prompt guidance is aligned.

### Fixed
- Model answers are preserved across the bounded WebSocket display transport.

## [0.4.1] - 2026-10-07

### Fixed
- Panel cancel labels are contained and approvals simplified.
- Output and approval interactions are smoother.

## [0.4.0] - 2026-10-07

### Added
- Worker requests are compacted and resumed automatically.

### Changed
- Panel conversation layout is refined and diagnostics simplified; stopped
  subagents are hidden from session lists.

## [0.3.1] - 2026-10-06

### Fixed
- Release publication handles cold npm caches and transient verification
  failures.

## [0.3.0] - 2026-10-06

### Added
- Hub installs worker releases with safe replacement and `PATH` setup.
- Headless subagent delegation with lifecycle supervision.
- Headless subagent tools are exposed to workers through Hub remote calls.

## [0.2.0] - 2026-10-05

### Added
- Qwen text and vision provider.
- Load-time configuration for intrinsic tool schemas.
- General-purpose agent role and refined memory guidance prompts.

### Changed
- Provider configuration is consolidated and system prompts are documented.

## [0.1.3] - 2026-10-04

### Added
- English documentation site on GitHub Pages, including the Simplex Loop
  Worker Protocol.
- Security policy and contribution templates.

### Fixed
- Tested tag releases are recovered without moving tags.
- Panels are notified when the current worker disconnects; panel output is
  bounded and transcript recovery paced.
- Worker log decoding is isolated and optional disk writes bounded.
- Payload queue-overflow rejections are correlated with their requests.
- Malformed streamed tool-call identities are diagnosed.
- Complete history display pages are bounded.
- CI filters native builds and worker E2E runs by changed inputs.

## [0.1.2] - 2026-10-01

### Fixed
- Tag publication finds draft releases created earlier in the same release.

## [0.1.1] - 2026-10-01

### Fixed
- Hub assets are built before release tests run.

## [0.1.0] - 2026-10-01

First public release. The module set began as the C++ execution side of a Python
agent harness: a stdio JSON tool server for code retrieval and process
execution, waiting on an external host to drive it. The `dataclass` contracts
(conversation records, prompt templates, endpoint configuration) were carried
over from that harness, and during migration the goal changed from exposing a
native tool to an external agent to being a complete native C++ worker. The
legacy indexing tool and its language plugins were removed before this release.

### Added
- Native C++20 worker with the agent loop, model provider stack, tool and hook
  registries, cancellation, and session persistence.
- Extension ABI for native model, tool, and loop-hook plugins, with YAML-driven
  configuration and role-based prompt loading.
- Intrinsic toolsets for process management, file reading, and exact text
  editing, plus optional local image modality assistance.
- Context compaction and persistent memory archives.
- Node.js Hub with a browser panel, WebSocket worker protocol, persistent
  configuration editor, model controls, and transcript restoration.
- `simplex run` CLI with unified session persistence.
- Tag-driven release pipeline: glibc worker archives on GitHub Releases and the
  `@hazer-bjtu/simplex-hub` package on npm.

[1.1.0]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v1.0.2...v1.1.0
[1.0.2]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.8...v1.0.0
[0.5.8]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.7...v0.5.8
[0.5.7]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.6...v0.5.7
[0.5.6]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.5...v0.5.6
[0.5.5]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.4...v0.5.5
[0.5.4]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.3...v0.5.4
[0.5.3]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.2...v0.5.3
[0.5.2]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.4.6...v0.5.0
[0.4.6]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.4.5...v0.4.6
[0.4.5]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.4.4...v0.4.5
[0.4.4]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.4.3...v0.4.4
[0.4.3]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.4.2...v0.4.3
[0.4.2]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.4.1...v0.4.2
[0.4.1]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/Hazer-BJTU/simplex-cpp/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/Hazer-BJTU/simplex-cpp/releases/tag/v0.1.0
