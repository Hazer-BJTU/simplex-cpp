# Worker allocation manager

`manager_lib` implements synchronous, target-local storage operations. The
`simplex_manager` executable exposes them through `simplex manage`. This is
phase A of [#142](https://github.com/Hazer-BJTU/simplex-cpp/issues/142): existing
Hub launchers still use their original local layout. The Hub now has an internal
local/SSH/Docker management transport adapter; session lifecycle switching and
clean-fork integration follow in separate PRs.

See the [management protocol](../docs/reference/management.md) for requests,
results, ownership rules, and examples.

## Implementation boundaries

- `src/main.cpp` decodes one Base64 argv request and writes one JSON response.
  It does not initialize providers, networking, tools, or the agent loop.
- `src/service.cpp` validates target paths and allocation identities, acquires
  stable sibling locks, and publishes the allocation control document atomically.
- `resolve_configuration()` is used by `load::read_configuration()`. It returns
  effective settings plus a shared lease. The parsed `load::Configuration`
  retains that lease through worker shutdown, even with persistence disabled.
- `load_persistence` is the existing JSON state codec extracted from `load_lib`.
  State inspection validates through the same decoder and returns the original
  parsed JSON, including unknown compatible fields. It reads a file only once.
  This boundary is where #187 will add a SQLite-backed JSON projection.

Raw YAML remains verbatim in the allocation control document. Effective settings
are composed in memory: only the persistence root and explicitly supplied
connection endpoints are overlaid. A small immutable `config/config.yaml`
reference is the worker entry point; it is **not** a standalone copy of the
source configuration. Do not edit or copy that reference to another directory.
`inspect` returns both the source text and effective configuration. No user
configuration library or provider credentials are expanded during management.

## Concurrency and recovery

Before a mutation, directory-inode locks coordinate the namespace: existing
ancestors are locked shared, top down, and the requested parent (or deepest
existing ancestor) is locked exclusive. Ancestor-allocation checks are repeated
under that guard, which stays held through directory creation/publication/removal.
This closes the race where parent and child preparations both passed independent
checks before either had published its record. Contention returns `busy`, without
waiting while holding a partial lock chain. These locks create no files in system
directories and require readable ancestors with directory `flock` support.

Each root has a stable sibling lock and control record in `.simplex-manager`.
Workers and state readers acquire shared leases; manager mutations require an
exclusive lease. The worker's existing exclusive `session.lock` still prevents
two workers writing one session. Removal checks that legacy lock as well.
Never delete sibling locks or records while these allocations may be referenced.
This uses local POSIX `flock`; it is not a distributed/NFS ownership protocol.

Preparation publishes a `preparing` record before creating the root. Retrying
the same identity/source completes an interrupted preparation. Removal publishes
`removing` before deleting and then replaces the control record with a small
`removed` tombstone containing no configuration/startup credentials. Retrying
finishes partial deletion. A delayed managed worker checks the record under its
lease and cannot recreate a removed allocation. Parent-directory deletion,
manual lock removal, and uncooperative writers are outside this contract.

All mutations use the existing atomic file utilities. Refresh preserves owner,
group and POSIX mode (not ACL/xattrs); new files are mode 0600 and new managed
directories are mode 0700. Publication followed by directory-sync failure is
reported with `published: true`, so callers inspect before retrying. Ancestor
creation has the same power-loss durability limits as `fileio::atomic_write`.

Allocations cannot nest inside one another or inside manager metadata paths.
Retired paths cannot be reused: choose a new target directory/allocation ID.
Existing non-managed directories are refused, including empty ones. This phase
performs no automatic legacy migration. Existing unmanaged worker/Hub sessions
continue operating unchanged; a later explicit migration must require a stopped
worker and verify the old layout before adopting anything.

Configuration preparation checks storage shape/paths and YAML validity, not
provider availability, credentials, prompt assets, or all runtime options. The
ordinary worker startup parser performs those checks before running. A ready
allocation means storage preparation succeeded, not that a worker is running.

The tests cover shared/exclusive lifetime fencing, legacy locks, wrong identity,
symlinks, startup capture, interrupted mutation recovery, full 10 MiB state reads,
and the real CLI/router protocol. No receive-size budget is imposed. Full
in-memory collection is intentional; argv limits still apply to requests.
