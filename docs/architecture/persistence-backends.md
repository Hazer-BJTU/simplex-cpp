# Persistence backend contract

**Status:** accepted · #187 PR-A–E and PR-G/H integration. SQLite is the default
for new empty sessions; existing JSON sessions remain supported. PR-F is deferred.

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
2. **Original PR-A decision (superseded by PR-G/H): JSON remains the default backend.** It keeps the
   authoritative `state.json` layout and the archive directory names, and since
   PR-B each attempt archives the complete state as
   `<memory>/<archive-id>/{state.json,manifest.json}`. The manifest is published
   atomically after the payload and is the attempt's identity and publication
   marker, so a payload without a matching manifest is incomplete evidence rather
   than a restorable archive. `readable.md` is no longer written. PR-C adds the
   owner-confirmed normalized SQLite schema v1 and its row codec
   (`persistence/src/sqlite_schema.cpp`, `persistence/src/sqlite_snapshot.cpp`)
   and PR-D connects its experimental `StateStore` for checkpoints and restoration;
   PR-E adds transactional database archives and retention. See
   [SQLite schema v1](sqlite-schema-v1.md).
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
   freezes the complete original state before summarization, and the commit
   publishes the replacement. A committed replacement settles its own attempt;
   `finish_archive_attempt` records only failure, cancellation or interruption, so
   a backend cannot report a durable commit as failed through a later bookkeeping
   step. A failed publication keeps its reserved directory and ordinal, and a
   payload renamed before a directory-sync failure stays as `Unknown` evidence. A
   settled archive is never rewritten, and no success marker is written after the
   commit.
7. **Retention stays exactly as configured.** The current attempt and archives
   named by retained state are protected; other recognized archives are kept
   newest first; unknown files, symlinks and incomplete directories are untouched.
   Legacy `state.md` archives are recognized as non-restorable `legacy_readable`
   evidence and are excluded from retention entirely: nothing can reconstruct
   their content, so a later prune never deletes them. `archive_at` maps a
   historical payload path (the form retained state records) or an archive
   directory to a store record for that protection only. It is not a query or
   reference-resolution interface.
8. **No memory or archive query surface.** Listing, paging, reading and search
   remain future work with their own design; the schema discussion in #187 is a
   prerequisite for PR-C. `ArchiveKind` is the single source of truth for
   restorability, so a reference cannot claim an inconsistent pair.

## Awaited storage boundaries

PR-D keeps the synchronous backend interface and moves current-state I/O onto
one dedicated storage thread. The worker awaits each operation. The loop exposes
an optional `Options::checkpoint` coroutine after the synchronous BeforeTools,
ResultsReady, StepFinished and RunFinished events; event hooks remain synchronous.
A barrier must settle before tools dispatch, pending results project, or the next
exchange starts. Terminal barrier failures propagate to the host, even when an
optional finish observer throws. The worker latches storage failure and refuses
later writes. Frozen state is borrowed only until the awaited operation completes;
control and read-only query handling can continue on the application executor.
Cancellation cannot detach a checkpoint.

SQLite stores the graph, revision and operation ID in one transaction. Read-only
loads retrieve all three consistently and never create or migrate a database. New
files use 0600 permissions. Existing invalid files fail rather than being adopted.
Commit errors are conservatively uncertain; reopen and compare the operation ID
before deciding what happened. No implicit retry or JSON fallback is allowed.
SQLite compact and auto compact use complete database archives. Since PR-G/H,
SQLite is the new-empty-session default; selecting another backend never migrates
an existing session. Legacy JSON selection and detection remain supported.

## Consequences

- `persisted` retains `format: json` as the dataclass exchange format and reports
  the actual backend separately. Compact results include structured `archive`
  evidence; `memory_file` is present only for a real JSON archive file. SQLite
  identifiers are not file paths or retrieval endpoints. Panel validation accepts
  both legacy file-only results and new structured results.

- The required-save failure that stops the worker now surfaces as a store error
  with the same diagnostic text plus an explicit recovery outcome.
- `load` keeps the JSON codec and the retention configuration type
  (`load::MemoryRetention`). PR-B moved archive layout, recognition and retention
  into the JSON backend, because the manifest is storage metadata that `load`
  should not own; the `simplex_load_archives` library therefore no longer exists.
- One shared library (`simplex_persistence`) stays in the release set.
- PR-F migration/export/rollback remains deferred. PR-G/H combines
  Hub configuration and current-state inspection, the new-session default switch,
  and deployment validation. Existing JSON sessions remain supported without
  automatic migration.

## Validation

`test_persistence_store` covers the backend-neutral contract plus JSON layout,
archive naming and ownership rules. `test_persistence_store_archive` proves a
complete-state round trip (large text, binary content, loop progress with pending
results and open extras), manifest validation, legacy Markdown preservation and
retention ordering. `test_persistence_store_publication` interposes `fsync` to
prove that a published-but-unsynced payload or manifest is reported as `Unknown`,
that an attempt without its marker is not recognized, and that the next attempt
keeps its ordinal. The compact, auto-compact, congestion and application tests
drive real archives, retention, cancellation and a directory-sync failure through
the worker.


`test_persistence_sqlite_archive` covers original-state equality, T1/T2 crash
boundaries, stale revision refusal, interruption evidence and retention targets
0/1/5. SQLite VFS fault injection covers preparation and replacement failures.
Worker tests exercise manual compact, live history during summarization, automatic
continuation, cancellation and provider errors with SQLite as well as JSON.

## PR-G/H integration

The final integration makes SQLite the default for new empty sessions, preserves
legacy JSON detection, and exposes current-state inspection through the manager.
See [selection and backup](state-and-persistence.md#selection-inspection-and-backup).
Offline migration/export (PR-F) remains deferred and is not an available command.
