# Authoritative session persistence

`persistence_lib` owns the worker's authoritative `AgentInputState` storage and
its compact-archive evidence. The contract is `<persistence/store.hpp>`; the JSON
adapter is `src/json_store.cpp`, and `src/json_archive.cpp` owns that adapter's
archive layout, recognition and retention. It is the only in-tree backend.

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
  blindly. Codec and filesystem failures are translated into `StoreError`;
  programming errors such as a foreign reference stay standard exceptions.
- `reserve_archive` plans an identity during preflight; `begin_archive` freezes
  the original state before summarization; `commit_compaction` publishes the
  replacement and settles that attempt. `finish_archive_attempt` records only
  failure, cancellation or interruption, so a durable commit cannot be turned
  into a failure by later bookkeeping. A settled archive is never rewritten.
- `prune_archives` applies the configured retention target. The current attempt
  and archives named by retained state are protected; unknown files, symlinks and
  incomplete directories are untouched, and legacy Markdown archives are never
  candidates at all. `archive_at` maps a historical on-disk payload path to a
  store record for that protection only.
- There is no memory or archive query surface: nothing here lists, pages, reads
  or searches archives for a client. Retention policy is unchanged by this
  abstraction.

## JSON adapter

The authoritative snapshot keeps the worker's long-standing file layout: a
required checkpoint atomically replaces `<state>/state.json` through
`load::save_state`, including its publication and directory-sync semantics. The
one intentional interface difference from the pre-#187 worker is the error type:
a required write reports a recovery outcome instead of a bare publication flag,
with the same message.

Each compact attempt is frozen below `<memory>/` as a directory named
`<20-digit ordinal>-<UTC timestamp>-<run_id>` containing:

```text
state.json      complete original AgentInputState (the restorable payload)
manifest.json   attempt identity, format version and payload size
```

The payload is written first through the same JSON codec; the manifest is
published atomically afterwards as the attempt's publication marker. Recognition
requires both files, an intact manifest and a matching payload size. Ordinals are
derived from existing entries and are never reused, including by failed attempts:
a failed publication keeps its reserved directory, and a payload renamed before a
directory-sync failure stays as `Unknown` evidence. A directory without its
manifest is incomplete, so nothing treats it as restorable state.

Archives written by earlier workers hold a single `state.md`. They are recognized
as `legacy_readable`: non-restorable evidence that retention never deletes,
because no worker can reconstruct or replace the Markdown it contains. Unknown
files, symlinks, extra files and incomplete archives are likewise untouched.

`readable.md` is no longer written. `persistence.readable` and the explicit
`load::StateFormat::Readable` export remain accepted for compatibility; the
worker reports one startup diagnostic and never treats either as authoritative
state. See `json_archive.cpp` for the layout and recognition rules.

## Build and validation

```sh
cmake --build build --target \
    test_persistence_store test_persistence_store_archive test_persistence_store_publication
ctest --test-dir build -R 'persistence' --output-on-failure
```

`test_persistence_store` covers the backend-neutral contract, JSON layout, naming
and ownership rules. `test_persistence_store_archive` proves that an archive
round-trips the complete pre-compaction state (large text, binary content, loop
progress with an unprojected result batch and open extras), that recognition
rejects a foreign identity, a size mismatch or a missing manifest, that legacy
Markdown is preserved, and that retention keeps the current and referenced
archives newest first. `test_persistence_store_publication` interposes `fsync` in
the test executable to prove that a published-but-unsynced payload or manifest is
reported as `Unknown` and that an attempt without its marker is not recognized.
