# Persistence backend

<a id="archived-text-search"></a>

**Use SQLite for new worker sessions.** It saves checkpoints transactionally
and keeps conversation state and compaction archives in one database
(`state/state.sqlite3`). Its full-text index lets the
[`archive_memory` toolset](../plugins/configuration.md) search archived conversations.

The legacy `json` backend writes a complete snapshot to `state/state.json` and
stores compaction archives under `memory/`. It still supports existing sessions
and is planned for deprecation. The Hub's `events.jsonl` is a separate event log.

## Configuration

Set the backend in the worker YAML. A relative session directory resolves
against the configuration file.

```yaml
persistence:
  enabled: true
  directory: ./data/session
  backend: sqlite
```

Omit `backend` to reuse the existing store or select SQLite in an empty
directory. Keep existing JSON sessions on JSON. Switching the setting cannot
convert their history, and the worker refuses a directory containing both stores.

## Inspection and backups

For managed sessions, the Hub reads either backend through
[`simplex manage`](../reference/management.md). To back up a session, stop the
worker, let manager reads finish, and copy the entire session directory,
including any SQLite WAL and SHM files.
