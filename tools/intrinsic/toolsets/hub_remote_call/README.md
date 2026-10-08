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
  nonempty payload `content` array of `{type, modality, raw, extras?}` parts.
  Continue/compact forbid content; stop forbids both content and options.
  Optional options accept provider-owned `model` and reserved empty `tools` only.
  The complete serialized argument object must fit 64 KiB UTF-8.
- `subagent_receive({})` lists direct children. With `subagent_id`, optional
  cursor (default 0) and limit (default 5, 1–10), it returns a snapshot of status,
  latest request outcomes and one primary-conversation page. It does not wait.
  A displayed assistant step's `answer_source` is a read-only source. Pass
  `answer: {source, part: 0, offset: 0}` with `subagent_id` (no cursor/limit) to
  retrieve exact text in 32 KiB UTF-8 segments. Follow `next_part`/`next_offset`,
  resetting offset to zero on a new part, until `done`. These pages bypass the
  96 KiB presentation clip. Source identity and offsets are checked; disconnect,
  restart, compaction or stop can make a source unavailable. Read answers before
  stopping the child; no generic URL/filesystem download is used.

Fork/send are SerialWrite and receive is ReadOnly. All are Trusted; child tool
approvals keep the independent user-owned policy. Ownership is enforced by the
Hub against the fresh worker/run identity. No caller IDs, arbitrary routes,
launcher settings or confirmation policy are accepted as model arguments.

Outputs use plain intrinsic metadata and readable content blocks. They retain
request/operation IDs, truthful dispatch/run states, visible turn/step association,
pagination and history completeness/freshness flags. Response diagnostics and
conversation bodies are clipped on UTF-8 boundaries with `output_truncated`;
bodies share a 96 KiB budget and the rendered document cannot exceed 256 KiB.
The budget is allocated before rendering: each turn's latest nonempty assistant
step and compact summaries share space first, then older assistant steps and
user input use the remainder. Display stays chronological, with original indices
and explicit clipping markers even for bodies omitted entirely. If the prioritized
result bodies together exceed the budget, they are also clipped.
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
when bounded outcome retention prevents correlation. Receive cursors address the
current bounded turn array, not original worker turn IDs; reset to zero on a
revision/worker change. See [the full lifecycle contract](../../../../docs/hub/subagents.md).
