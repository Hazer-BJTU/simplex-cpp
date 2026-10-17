# Persistence backend contract

**Status:** accepted · #187 PR-A · supersedes no earlier decision.

This page records the storage boundary introduced before a normalized SQLite
backend exists. The planning issue is
[#187](https://github.com/Hazer-BJTU/simplex-cpp/issues/187); the contract itself
lives in `persistence/include/persistence/store.hpp`.

## Context

`model_io::AgentInputState` is the authoritative conversation. Until this change
the worker called file helpers directly: it named `state.json`, planned archive
directory names, wrote the readable archive, and applied retention inline with
run control. Compaction interleaved archiving, a network summary, a replacement
write and cleanup inside one coroutine.

Three constraints made that shape hard to extend:

- A second backend (normalized SQLite, #187 PR-C onward) would otherwise leak its
  format into the worker, the loop and the Hub integration.
- Recovery ordering is a correctness property, not an implementation detail. The
  checkpoint before tool dispatch and the one after results enter Projection must
  complete before the loop continues.
- A failed publication is not always a failed write. A rename can succeed and the
  following directory sync fail, leaving a visible but not crash-durable state.

The owner-confirmed scope of the first phase is unchanged: archive the complete
original state as-is, add no memory or archive query interface, keep the existing
retention policy, and defer new archive-reference handling.

## Decision

1. **One session-scoped store contract.** Operations are named after persistence
   semantics (`load_current`, `checkpoint`, `reserve_archive`, `begin_archive`,
   `commit_compaction`, `finish_archive_attempt`, `prune_archives`), not after
   files. `core` owns the store; dataclass, loop, tools and providers never see a
   backend type.
2. **The JSON adapter is the only backend in this phase** and preserves the
   historical layout exactly: atomic `state.json`, readable `state.md` archives,
   the same directory names, the same retention recognition and the same
   diagnostics. Existing worker tests are the acceptance gate.
3. **Checkpoints are synchronous and ordered.** A call returns only after its
   durability outcome is known. A backend that must run storage work on another
   executor (a database writer) must still complete or fail the call before the
   worker resumes; fire-and-forget writes and unobserved futures are excluded. The
   worker therefore cannot dispatch tools behind an outstanding checkpoint.
4. **Outcomes are three-state.** `NotCommitted` means the previous state is still
   authoritative; `Unknown` means publication may have happened and must be
   verified after reopening. Any required-write failure latches storage failure;
   an `Unknown` outcome is inspected after reopening instead of retried blindly.
   Codec and filesystem failures are translated into the store error so callers
   never have to classify a raw `std::filesystem_error`; only programming errors
   stay outside the contract.
5. **Revisions are optional.** A backend that reports a revision must enforce an
   expected revision and refuse a stale write. A backend that reports none (JSON)
   rejects a non-empty expectation as a programming error rather than pretending
   to validate it.
6. **Archive publication and settlement are ordered, and success is the commit.**
   Preflight reserves an identity without touching the filesystem, `begin_archive`
   freezes the original state before summarization, and the commit publishes the
   replacement. A committed replacement settles its own attempt;
   `finish_archive_attempt` records only failure, cancellation or interruption, so
   a backend cannot report a durable commit as failed through a later bookkeeping
   step. A failed publication keeps its reserved directory and ordinal, and a
   payload renamed before a directory-sync failure stays as `Unknown` evidence. A
   settled archive is never rewritten.
7. **Retention stays exactly as configured.** The current attempt and archives
   named by retained state are protected; other recognized archives are kept
   newest first; unknown files, symlinks and incomplete directories are untouched.
   `archive_at` maps a historical on-disk path to a store record for that
   protection only. It is not a query or reference-resolution interface.
8. **No memory or archive query surface.** Listing, paging, reading and search
   remain future work with their own design; the schema discussion in #187 is a
   prerequisite for PR-C. `ArchiveKind` is the single source of truth for
   restorability, so a reference cannot claim an inconsistent pair.

## Why not an awaitable contract in this phase

The JSON write path is synchronous and already completes before the checkpoint
returns. Making the contract awaitable now would change checkpoint ordering and
the loop integration for no observable benefit. PR-D introduces database
checkpoints and must then either add an awaitable boundary or isolate storage
work on a dedicated executor while the loop waits; either way, the ordering rule
above stays the acceptance criterion.

## Consequences

- Behavior, output and wire fields are unchanged: the same files are written, the
  `persisted` event keeps its `boundary` labels and reports `format` from the
  backend, and the compact result still names the archived file through
  `memory_file`.
- The required-save failure that stops the worker now surfaces as a store error
  with the same diagnostic text plus an explicit recovery outcome.
- `load` keeps the JSON codec and the retention helper; `persistence` depends on
  them until the archive format changes. Retention moves with the archive format
  in PR-B if it stops being described by filesystem paths.
- One more shared library (`simplex_persistence`) joins the release set, and
  `simplex_load_archives` is split out so the retention helper does not pull the
  plugin-loading stack into the store.
- Remaining work is unchanged: PR-B retires the Markdown archive, PR-C needs the
  owner-confirmed schema, and PR-D through PR-H add the database backend,
  migration tools, Hub integration and the new default.

## Validation

`test_persistence_store` covers the backend-neutral contract plus JSON layout,
archive naming, retention recognition and ownership rules.
`test_persistence_store_publication` interposes `fsync` to prove that a
published-but-unsynced replacement is reported as `Unknown` and still decodes.
The existing compact, auto-compact, congestion and application tests keep passing
unchanged apart from asserting the new error type; those tests drive real archives,
retention, cancellation, and a directory-sync failure through the worker.
