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
an empty directory selects SQLite. A directory holding both stores is refused for
every setting, including an explicit backend: choosing one would abandon the other
history, and no migration is provided, so remove or migrate one store first.
Changing the setting does not migrate data, and selecting the opposite backend
over a sole existing store is rejected. Saved Hub configurations are preserved.

Managed Hub sessions use `simplex manage read_state` for read-only inspection;
the manager returns decoded JSON for either backend. For backups, stop the worker
and finish manager reads before copying the whole session directory, including
any SQLite WAL/SHM files. Use a local filesystem compatible with SQLite WAL.

See [State and persistence](state-and-persistence.md) for recovery and lifecycle
behavior, and [Configuration](../getting-started/configuration.md) for setup.

## Archived-text search

SQLite stores also maintain an FTS5 trigram index of retained archive text:
conversation bodies, tool calls, and textual tool results. Reasoning and binary
content are excluded from the index; complete snapshots remain unchanged.
Retention removes an archive's index entries with the archive, so the default
five-archive retention target also limits searchable history.

When persistence is enabled and the selected backend is SQLite, the worker
also loads `memory_search` and `memory_fetch`. The model can search for literal
text, then fetch a hit using its `archive_id` and `source_id`. Long sources use
byte pagination (`next_offset` / `has_more`); results have bounded previews and
invalid UTF-8 is replaced only for display. Prefer search phrases of at least
three characters; shorter queries are case-sensitive. Retention may remove a
source between search and fetch. Current unarchived turns are not searched.

The tools require no extra worker setting, including with automatic backend
selection. JSON and disabled persistence omit both tools and their skill.
Host-side query timeout and display budgets live in the installed
`bin/schemas/archive_memory/` declarations; see
[tool configuration](../plugins/configuration.md). No Hub search interface is
added. Existing SQLite schema-v1 stores remain readable and are upgraded with an index backfill
on their next normal writer operation. After upgrade, older workers that only
support schema v1 cannot open the store.
