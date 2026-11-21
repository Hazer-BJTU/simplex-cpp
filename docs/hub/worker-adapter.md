# Worker adapter reference

The bundled Hub receives worker connections and routes events, approvals and
remote tool requests by session. See the [Simplex Loop Worker Protocol](../core/worker-protocol.md)
for the wire reference and an approved tool exchange.

## Managed sessions

The Hub uses the target's `simplex manage` command to prepare allocations and
inspect saved state. See [worker storage management](../reference/management.md).

## Headless delegation

Headless workers have a parent session and independent approval policy. See
[headless subagents](subagents.md) for route and lifecycle semantics.
