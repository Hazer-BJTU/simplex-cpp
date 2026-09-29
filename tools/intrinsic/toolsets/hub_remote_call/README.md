# Hub remote call tools

This optional intrinsic package is the worker-side construction boundary for
future one-shot calls to the hub. It currently exports an **empty toolset** and
an abstract request-tool base. It registers no concrete tools, skill, or
capability groups, and does not initiate network IO during construction.

## Configuration and ownership

```yaml
hub_remote_call:
  endpoint: ws://127.0.0.1:8801/agent/session-1/tools?token=SESSION_TOKEN
  timeout_ms: 120000
```

Omit this mapping to leave the set unloaded. When present, `endpoint` is required
and must be a complete `ws://` or `wss://` base URL. `timeout_ms` defaults to
`120000` and accepts an integer in `1..2147483647`. An invalid explicit mapping
fails configuration loading rather than silently disabling the set.

`Application::construct_runtime()` creates `HubRemoteCallToolSet` from the parsed
endpoint and timeout and adds it to the session registry. This is the same
host-injected intrinsic pattern used by modality assistance. The set owns copies
of its transport settings and has no dependency on core, load, a live application,
or AgentInputState. Its const accessors support constructing future tools.

Hub-generated new sessions enable this mapping only when
`worker.hubRemoteCall: true` is set in hub configuration (default `false`). On
restart the hub refreshes the endpoint of an existing mapping, preserves its
other options, and leaves an omitted mapping disabled.

## Request base

`HubRemoteCallToolBase` derives from `DeclaredTool` and remains abstract through
`invoke()`. Its protected constructor takes a YAML declaration path, endpoint,
timeout and optional confirmation event bus. The base does not choose invocation
or security attributes; future declarations and tools must do so explicitly.
No production declaration files are installed while the set is empty.

The protected `request(query, route, worker_id, session_id, run_id)` method:

1. Validates a code-selected route using the hub's lowercase, slash-separated
   grammar. The route cannot contain queries, encoded separators or traversal.
2. Appends the route to the endpoint pathname, preserving the session token query.
3. Generates a new request ID and sends `query.arguments` in one `tool_request`.
4. Awaits one bounded WebSocket exchange, with no retries or inherited run
   cancellation. Each invocation owns its connection and request state.
5. Validates the response type, all echoed identifiers, route and currently
   supported rejection shape. It returns the response's `data` object.

A returned `status: rejected` envelope **does not mean the tool succeeded**. The
request base returns protocol data; future concrete `invoke()` implementations
must interpret it and produce the appropriate tool result/failure. Currently only
`rejected` with error code `not_implemented` is defined. Other statuses, including
an invented success response, are rejected as malformed protocol. Successful
result types must be specified together with an actual remote operation.

Transport and protocol failures raise `InvokeException` at `Stage::Invoke`,
correlated to the original query. Diagnostics do not include raw frames or the
endpoint query, because these can contain credentials. Worker/session/run IDs
must be supplied by trusted host context, never taken from model arguments.

The owning tool must remain alive through completion. The endpoint is immutable
and each call has independent state, so parallel requests do not share a socket
or mutable run identity. No request cache, persistent queue, automatic retry,
or exactly-once delivery is provided. The timeout uses
`intercom::cancellable_exchange`: it bounds reply validity and joins cleanup;
a running system DNS lookup can delay completion beyond that deadline.

## Adding a concrete operation later

Define its hub route, authorization, arguments/results, side effects, cancellation
and retry policy first. Then add a YAML-declared subclass with a fixed route,
normal argument validation and an explicit security policy, and register it from
this set's constructor. Provide host identity at invocation time rather than
capturing a mutable application reference. Do not expose a generic arbitrary-route
model tool or make the session token an implicit approval grant.

The transport contract is in
[the worker protocol](../../../../core/docs/worker-protocol.md#remote-tool-requests).
The loopback tests cover correlation, query preservation, invalid frames/statuses,
timeout, and an empty catalogue. No model provider or external hub is required.
