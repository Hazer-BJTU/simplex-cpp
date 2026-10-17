# Worker adapter reference

The worker contract and bundled Hub implementation are maintained together in
[Simplex Loop Worker Protocol](../core/worker-protocol.md).

The [bundled Hub implementation](../core/worker-protocol.md#bundled-hub-implementation)
section covers routes, identity validation, confirmations, remote tools,
reconnects, process supervision, persistence, and known limitations.

For operator setup, see [Hub deployment](../deployment/hub.md). The separate
[Hub panel protocol](hub-protocol.md) describes browser and management clients.

## Managed sessions

The configuration editor's **Managed worker** template opts new sessions into
`simplex manage` for local, SSH or retained-container Docker storage operations.
Hub bookkeeping and target worker data are separate, and inspection uses the
manager's complete JSON response. Existing profiles are not migrated implicitly.
See [management configuration and lifecycle policy](../reference/management.md#opt-in-managed-sessions).
Managed children use target-side clean-fork and removal with independent flat
allocations; their Hub-local bookkeeping remains under `dataDir/subagents/`.

## Headless delegation

The Hub supports clean-fork/send/receive remote routes for directly owned headless
workers. Each child uses a flat `<dataDir>/subagents/<generated-id>/` bookkeeping root, an
independent operator-controlled ask/deny/approve policy, and a bounded primary
conversation projection. It retains no event transcript or reasoning/tool history.
Parent process shutdown/crash cascades; socket disconnect alone preserves the
family. Confirmed child shutdown deletes its persistence. Clean-fork shares any
explicit external workspace and is not a sandbox. The intrinsic `hub_remote_call`
toolset supplies `subagent_fork`, `subagent_send` and `subagent_receive` when its
remote endpoint is configured; the same Hub routes remain usable by other workers.
See the [complete subagent contract](subagents.md) for configuration snapshots,
launch support, request deduplication, recovery and cleanup limits.
