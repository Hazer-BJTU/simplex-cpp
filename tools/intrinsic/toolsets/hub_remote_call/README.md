# Hub remote calls and session plans

Core conditionally loads this intrinsic set when `hub_remote_call` is configured.
The set injects endpoint/deadline settings and a host identity snapshot callback
into `PlanTool`. No network connection is opened at startup.

```yaml
hub_remote_call:
  endpoint: ws://127.0.0.1:8801/agent/session-1/tools?token=SESSION_TOKEN
  timeout_ms: 120000
```

Omission leaves the set unloaded. Hub-generated new sessions require
`worker.hubRemoteCall: true` (default false). Existing session configurations keep
their enabled/disabled choice. Timeout is an integer in `1..2147483647`.

## Plan tool

`plan` accepts `{"operation":"read"}` or
`{"operation":"replace","markdown":"- [ ] Work"}`. Replacement is complete;
empty/whitespace Markdown clears the card. Read forbids the markdown argument.
Both sides enforce a 64 KiB UTF-8 limit. No session/path/route arguments are exposed.
Read uses ReadOnly scheduling; replace uses SerialWrite. Both use Trusted policy,
since they access only the current session's plan and perform no arbitrary IO.

The hub owns `<dataDir>/sessions/<id>/plan.json`, containing `markdown`, `revision`
and `updated_at`. It validates the live worker and active run, waiting briefly for
cross-connection event ordering. Replacement publishes atomically before replying
and broadcasting. Failed publication preserves the old plan. Identical content
is a no-op. Plans survive run completion/cancellation, compact and restart.

Read returns plan text using the intrinsic metadata/block format. Replace returns
only revision and a short status. Hub rejections, malformed replies and transport
failures become invocation errors. A disconnect after commit leaves the outcome
unknown; read before replacing again. There is no automatic retry or rollback.

## Request base and extension boundary

`HubRemoteCallToolBase` derives from DeclaredTool. Concrete tools provide their
YAML declaration, argument/security rules, fixed route and invoke implementation.
Its protected request method copies the call and trusted identifiers, appends the
route before the endpoint query, generates a request ID, sends one WebSocket
exchange and validates all echoed identifiers. It accepts succeeded/object-result
or rejected/code-message envelopes; the concrete tool validates its result shape.

The base owns immutable transport settings. Every request owns a separate socket;
no mutable application state is retained. The host identity callback uses a weak
application reference and briefly locks to copy worker/session/run IDs. Network
awaits hold no application lock. Never obtain these IDs from model arguments.

Requests wait through run cancellation. The intercom deadline invalidates late
replies and joins transport cleanup; an active system DNS lookup can delay cleanup.
Transport diagnostics exclude the token-bearing endpoint and raw reply bytes.
Future operations must explicitly define authorization, arguments, results and
side effects before registration. Unknown routes remain not_implemented.

Schemas and the concise skill are installed in `bin/schemas/hub_remote_call`.
The loader uses that installation directory, then its source-tree fallback;
`SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR` can override it. The worker wire contract is
in [the protocol](../../../../core/docs/worker-protocol.md#remote-tool-requests).
