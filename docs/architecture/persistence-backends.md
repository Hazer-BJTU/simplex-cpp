# Persistence backend

The worker saves its complete conversation state through a session-scoped store.
Both backends support restoration, checkpoints, compaction archives, and archive
retention. SQLite is the default for new sessions.

| Backend | Storage | Use |
| --- | --- | --- |
| `sqlite` | `state/state.sqlite3`, containing normalized state and archives | Default; transactional checkpoints and compaction with revision checks. |
| `json` | `state/state.json`, with complete JSON archives under `memory/` | Plain JSON snapshots; supported for existing sessions and explicit new configurations. |

JSON is a complete snapshot, **not JSONL**. The Hub's `events.jsonl` records
observed events separately; it cannot restore a worker's conversation.

## Configuration

```yaml
persistence:
  enabled: true
  directory: ./data/session
  backend: sqlite
  state: state
  memory: memory
  restore: if_present
  memory_retention:
    max_archives: 5
```

Use `backend: json` to select JSON for a new session. Paths are relative to
`persistence.directory`; the worker does not append a session ID. SQLite keeps
its archives in the database, while JSON uses the configured memory directory.

If `backend` is omitted, existing JSON-only or SQLite-only storage is detected;
an empty directory selects SQLite. If both exist, an explicit choice is required.
Changing the setting does not migrate data, and selecting the opposite backend
over a sole existing store is rejected. Saved Hub configurations are preserved.

Managed Hub sessions use `simplex manage read_state` for read-only inspection;
the manager returns decoded JSON for either backend. For backups, stop the worker
and finish manager reads before copying the whole session directory, including
any SQLite WAL/SHM files. Use a local filesystem compatible with SQLite WAL.

See [State and persistence](state-and-persistence.md) for recovery and lifecycle
behavior, and [Configuration](../getting-started/configuration.md) for setup.
