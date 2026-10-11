# Worker adapter reference

The worker contract and bundled Hub implementation are maintained together in
[Simplex Loop Worker Protocol](../core/worker-protocol.md).

The [bundled Hub implementation](../core/worker-protocol.md#bundled-hub-implementation)
section covers routes, identity validation, confirmations, remote tools,
reconnects, process supervision, persistence, and known limitations.

For operator setup, see [Hub deployment](../deployment/hub.md). The separate
[Hub panel protocol](hub-protocol.md) describes browser and management clients.

## Headless delegation

The Hub supports clean-fork/send/receive remote routes for directly owned headless
workers. Each child uses a flat `<dataDir>/subagents/<generated-id>/` root, an
independent operator-controlled ask/deny/approve policy, and a bounded primary
conversation projection. It retains no event transcript or reasoning/tool history.
Parent process shutdown/crash cascades; socket disconnect alone preserves the
family. Confirmed child shutdown deletes its persistence. Clean-fork shares any
explicit external workspace and is not a sandbox. The intrinsic `hub_remote_call`
toolset supplies `subagent_fork`, `subagent_send` and `subagent_receive` when its
remote endpoint is configured; the same Hub routes remain usable by other workers.
See the [complete subagent contract](subagents.md) for configuration snapshots,
launch support, request deduplication, recovery and cleanup limits.
