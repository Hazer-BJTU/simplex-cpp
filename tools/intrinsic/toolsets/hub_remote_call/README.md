# Hub remote calls, plans and subagents

Core conditionally loads this intrinsic set when `hub_remote_call` is configured.
The set injects endpoint/deadline settings and a host identity snapshot callback
into `PlanTool` and the three subagent tools. No network connection is opened at startup.

```yaml
hub_remote_call:
  endpoint: ws://127.0.0.1:8801/agent/session-1/tools?token=SESSION_TOKEN
  timeout_ms: 120000
```

Omission leaves the set unloaded. Hub-generated new sessions require
`worker.hubRemoteCall: true` (the default). Existing session configurations keep
their enabled/disabled choice. Timeout is an integer in `1..2147483647`.

## Plan tool

`plan` accepts `{"operation":"read"}` or
`{"operation":"replace","markdown":"- [ ] Work"}`. Replacement is complete;
empty/whitespace Markdown hides the Plan tab. Read forbids the markdown argument.
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
exchange with a 256 KiB assembled reply limit and validates all echoed identifiers. It accepts succeeded/object-result
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
in [the protocol](../../../../docs/core/worker-protocol.md#remote-tool-requests).

## Subagent tools

Public interfaces are declared in `include/tools/intrinsic/hub_remote_call/subagents.hpp`.
The model-facing contract lives in `schemas/subagent_{fork,send,receive}.yaml`;
`src/subagents.cpp` implements argument validation, fixed routes and result rendering.
The toolset publishes separate `plan` and `subagents` capability groups so missing
declarations report partial availability without disabling unrelated tools.

- `subagent_fork({})` creates a fresh direct child from the parent's startup
  configuration, returns immediately and sends no task. Receive its ID until
  ready and connected, then send self-contained instructions.
- `subagent_send` requires `subagent_id` and `operation`. Message requires a
  nonempty text `request`. Include resource paths or references and let the child
  read them itself; the tool does not accept attachments or content metadata.
  Continue/compact forbid request; stop forbids both request and options.
  Optional options accept provider-owned `model` and reserved empty `tools` only.
  The tool converts request to one `{type: text, modality: text, raw: request}`
  part for the existing Hub route, without changing the recorded model call.
  Both the tool arguments and converted wire arguments must fit 64 KiB UTF-8.
- `subagent_receive({})` lists direct children (up to 32 per display). With
  `subagent_id`, it returns status, the latest request outcome and a 16 KiB UTF-8
  preview of the latest visible reply. It does not wait for completion.
- Add `output_path` to export the latest persisted primary dialogue as Markdown
  on the parent worker host. The parent directory must exist; relative paths use
  the worker's current directory. Existing entries, including symlinks, are never
  overwritten. Export is SerialWrite + RequireConfirm; preview/list remain
  ReadOnly + Trusted. Fork/send remain SerialWrite + Trusted.

Only user text/reference content and visible model responses are exported. No
reasoning, tool calls/results, system instructions or arbitrary extras are copied.
The document includes a contents list and unique ASCII `simplex:block:` begin/end
markers. The tool returns the path, size, block count and latest response marker,
not the document body. It explicitly identifies the source as a saved snapshot:
running work may not yet be saved, and compacted-away history is absent.

Hub obtains a complete backend-neutral state through the managed launcher. An
immutable Markdown snapshot is transferred in 32 KiB UTF-8 pages; its cursors are
internal and never model arguments. The Hub retains at most two snapshots or
pending reads, each up to 64 MiB, for 120 seconds. Oversized exports fail explicitly
rather than silently truncating answers. Each page reauthorizes the active parent
run and direct-child relationship. Export requires managed persistence; no fallback
to truncated display history or local Hub filesystem reads is used.

The worker writes each page on a file worker thread, validating snapshot identity,
byte offsets and stable metadata. Only a complete transfer is synchronized and
atomically published with no overwrite. Failures remove temporary data; a directory
sync error after publication explicitly reports uncertain durability. Tool execution
shields inherited cancellation through this publication boundary, consistent with
other side-effectful tools. Hard process termination can leave a private temporary
file named `.simplex-export-*`. No automatic RPC retry is performed.

Unknown future fields are ignored rather than blindly returned. Child text is
untrusted data, not instructions for the parent.

Read required output before stop: this is process-family shutdown with persistent
data deletion, not run cancellation. A parent run finishing/cancelling does not
stop children. Clean-fork gives fresh conversation/memory, but configured external
workspaces may remain shared. Unsupported launch arrangements are Hub rejections.
An older Hub can reject these tools as `not_implemented` while plan continues working.

Every invocation generates a fresh RPC ID. There is no retry/polling/destructor
cleanup and no exactly-once promise for repeated tool calls. A lost response may
follow a committed mutation; list/receive before repeating, and report ambiguity
when bounded outcome retention prevents correlation. Model-facing receive has
no pagination fields; the older wire pagination remains available for older workers. See [the full lifecycle contract](../../../../docs/hub/subagents.md).
