# Authoritative session persistence

`persistence_lib` owns the worker's authoritative `AgentInputState` storage and
its compact-archive evidence. The contract is
`<persistence/store.hpp>`; the JSON adapter in `src/json_store.cpp` is the only
in-tree backend.

The module exists so that no execution code knows a storage format. `core` asks
for a checkpoint, a reserved archive, a compaction commit, or a retention pass;
it never names `state.json`, an archive directory, or a future database file.
Dataclass, loop, tool and provider code stay independent of the backend.

## Contract

- One store per session, one writer at a time, confined to the worker strand. The
  contract has no internal locking and no overlapping operations.
- Every operation is synchronous: it returns only after its durability outcome is
  known. A backend that needs a database writer still completes or fails the call
  before the worker continues, so a checkpoint can never be fire-and-forget.
- `checkpoint` and `commit_compaction` report `NotCommitted`, `Committed` or
  `Unknown`. `Unknown` (for JSON: the rename succeeded but the directory sync
  failed) stops further writes and is verified after reopening, never retried
  blindly.
- `reserve_archive` plans an identity during preflight; `begin_archive` freezes
  the original state before summarization; `commit_compaction` publishes the
  replacement; `finish_archive_attempt` records how the attempt settled. A settled
  archive is never rewritten.
- `prune_archives` applies the configured retention target. The current attempt
  and archives named by retained state are protected; unknown files, symlinks and
  incomplete directories are untouched. `archive_at` maps a historical on-disk
  archive path to a store record for that protection only.
- There is no memory or archive query surface: nothing here lists, pages, reads
  or searches archives for a client. Retention policy is unchanged by this
  abstraction.

## JSON adapter

The adapter preserves the pre-#187 behavior exactly, which is why the worker's
existing tests are the acceptance gate for the refactor:

- `state.json` is replaced atomically through `load::save_state`, including its
  publication and directory-sync semantics.
- Compact attempts archive a readable `state.md` into a uniquely named directory
  before the summary exchange. Ordinals are derived from existing entries and are
  never reused, including by failed attempts.
- Retention still uses `load::prune_memory_archives`, which recognizes only the
  ordinal/timestamp/UUID directory name with exactly one `state.md`.
- The readable `readable.md` export remains a separate, optional view written by
  the worker; it is not authoritative state.

Archiving the complete original state as JSON and retiring the Markdown export
belong to #187 PR-B, and the archive format change lands in `json_store.cpp`.

## Build and validation

```sh
cmake --build build --target test_persistence_store test_persistence_store_publication
ctest --test-dir build -R 'persistence' --output-on-failure
```

`test_persistence_store` covers the backend-neutral contract plus JSON layout,
naming, retention recognition and ownership rules. `test_persistence_store_publication`
interposes `fsync` in the test executable to prove that a published-but-unsynced
replacement is reported as `Unknown` and still decodes.
