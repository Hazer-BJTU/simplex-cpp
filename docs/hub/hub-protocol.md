# Hub panel wire reference

**Panel protocol** `simplex-hub-panel`, version **1**. Use this appendix to look up
a wire message, capability or error when implementing a panel in any language.
For connection setup and a typed observer, see the [Hub UI SDK](ui-sdk.md).

## JSON API

Read public metadata from `GET /api/meta`. Use operator authentication for
protected endpoints when a panel token is configured. Check both HTTP status
and the response body for supervisor outcomes.

| Surface | Operations |
| --- | --- |
| Sessions and processes | List, create, inspect or delete sessions under `/api/sessions`. Start, stop, restart and force-kill actions are scoped to a session. |
| Display and inspection | Read event/log tails and a saved snapshot by session. Use `POST /api/sessions/:id/answer` for exact answer pages. |
| Configuration | Read templates and validate, save or preview reusable launch and worker files under `/api/configurations`. |
| Delegation | Inspect recovery and update headless approval policy. See [Headless subagents](subagents.md). |
| Skills and decoration | Read the skill library or fetch configured background image bytes. |

Session descriptions expose the effective `spec.autoCompactThreshold` and
`spec.maxAutoCompactions` for the worker's automatic compaction policy. Full
method/path pairs and status-specific request and reply types are available in
[the REST contract](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/hub/ui/rest.ts).

## Panel WebSocket

Connect to `/panel/ws` and exchange JSON text messages with `v: 1` and
a `type` discriminator. The Hub announces its protocol and capabilities in
`welcome.hub`. For paged replay, scope the cursor to the session and
`transcript_epoch`. Confirmations reach every connected panel client.

### Client to hub

| Message | Key fields | Effect |
| --- | --- | --- |
| `subscribe` | `session`, optional `since`, `paged`, `replace`, `request_id` | sends `subscribed` with the transcript after `since`; `paged: true` opts into paced replay when `transcript-pages` is advertised |
| `unsubscribe` | `session` | stops live messages for that session |
| `list_sessions` | — | answers with `sessions` |
| `create_session` | `session`, optional `spec` | answers with `created`, or `session_exists` / `invalid_session` |
| `delete_session` | `session` | requests session removal and emits `session_removed` on success |
| `worker` | `session`, `action`: `start`\|`stop`\|`restart`\|`force-kill`, optional `spec` | answers with `accepted` (carrying the result) or `worker_action_failed` |
| `input` | `session`, `operation?`: `message` \| `continue` \| `compact`, optional `content`, `request_id`, `options` | Omitted `operation` means `message`. `message` requires content; `continue` and `compact` omit it. Validates, sends a payload, answers with `accepted` and `request_id` |
| `history` | `session`, optional `request_id`, `start`, `step`, `limit` | if the current worker advertises `session-history`, sends a read-only payload; otherwise returns `input_not_sent`. The response arrives as a transient `history` worker event |
| `signal` | `session`, `operation`: `status`\|`options`\|`cancel`\|`shutdown`, optional `run_id` | answers with `accepted` or `signal_not_sent` |
| `confirmation` | `session`, `confirmation_id`, `decision`, optional `reason`, `request_id` | answers with `accepted` or `confirmation_rejected` |
| `question_answer` | `session`, `question_id`, `answer` with exactly one of `text` or `option_index` | answers with `accepted` or `unknown_question` / `invalid_answer`; removes the pending question |
| `logs` | `session`, optional `limit` | answers with a bounded tail of captured worker lines |
| `status_snapshot` | `session`, optional `since` | answers with a fresh `snapshot` |
| `ping` | — | answers with `pong` |
| `subagent_policy` | `session`, `policy`: `ask`\|`deny`\|`approve` | updates only new headless confirmation requests; existing prompts remain actionable |

### Hub to client

| Message | Key fields | Meaning |
| --- | --- | --- |
| `welcome` | `hub`, `sessions`, `subscriptions` | sent once per connection |
| `sessions` | `sessions` | full list, on request |
| `session` | `session` | one session changed |
| `session_removed` | `session` | removed from the panel session list |
| `subscribed` | `session`, `transcript`, `logs`, `latest`, `transcript_epoch`, optional `plan`, `questions`, `replay_more`, `replay_reset`, `request_id` | subscription replay page |
| `created` | `session` | session created by this client |
| `event` | `session`, `hub_seq`, `envelope` | one bounded worker display event |
| `confirmation` | `session`, `open`, `confirmation`, optional `outcome` when closing | prompt opened or retired/answered; broadcast to connected panels |
| `process` | `session`, `process` | worker process state changed |
| `connection` | `session`, `connected`, `identity` | event connection opened or closed |
| `request` | `session`, `request` | payload outcome changed |
| `logs` | `session`, `lines`, `dropped` | log tail, only in response to `logs` |
| `snapshot` | `session`, `transcript` | transcript plus description, on request |
| `accepted` | `action`, `session`, plus action-specific fields | the command was accepted |
| `error` | `error`, `message`, optional `request` | the command was refused |
| `plan` | `session`, `plan` | latest persisted session plan; an empty markdown string clears it |
| `questions` | `session`, `questions` | complete pending-question set after any change; an empty array removes the cards |
| `pong` | `at` | heartbeat reply |

### Error codes

| Code | Meaning |
| --- | --- |
| `bad_json` | the message was not JSON |
| `bad_message` | the message was not a JSON object |
| `unsupported_version` | `v` is not 1 |
| `unknown_session` | no such session |
| `invalid_session` | the session id is not 1–128 `[A-Za-z0-9_-]` |
| `session_exists` | the id is taken |
| `session_busy` | a worker is running or connected; stop it first |
| `headless_restricted` | direct panel conversation/control is unavailable for headless workers |
| `invalid_policy` | safety policy operation requires a headless session |
| `input_not_sent` | validation failed or the worker is not connected |
| `signal_not_sent` | same, for signals |
| `unknown_confirmation` | that prompt is no longer open |
| `confirmation_rejected` | the decision was refused (already answered, or identity unverified) |
| `unknown_question` | that question is no longer open |
| `invalid_answer` | the answer was malformed or did not fit the question |
| `worker_action_failed` | worker supervision failed; inspect the diagnostic |
| `binary_not_supported` | panel messages must be text |
| `internal_error` | the hub failed to process the message; it is still serving |

### Capabilities

| Capability | Meaning |
| --- | --- |
| `worker-events` | worker envelopes are forwarded, unknown event names included |
| `confirmations` | tool confirmations are surfaced and can be answered |
| `operator-questions` | pending `ask_user` questions are shown inline and can be answered |
| `supervisor` | worker processes can be started, stopped, and force-killed |
| `transcript-replay` | `subscribed` replays the transcript after a cursor |
| `transcript-pages` | opt-in subscription replay uses paced, byte-bounded pages |
| `snapshot-view` | the worker's persisted snapshot can be read, never written |
| `transcript-epoch` | `transcript_epoch` is reported, so a stale cursor is detectable |
| `global-confirmations` | confirmations reach every client, not only subscribers |
| `answer-pages` | read exact worker answer content through authenticated bounded pages |
| `session-history` | panel can query a worker's simplified conversation history |
| `context-compact` | panel can request worker context compaction and display the result |
| `headless-subagents` | authenticated parent delegation and headless operator safety controls |

## Predefined skill library

These read-only HTTP routes share panel authentication. They do not change the
worker protocol or the panel `input` WebSocket message. Client implementations
expand selected instruction text into ordinary message content before sending.
The SDK exports `SkillSummary`, `SkillDocument`, `SkillList`, and `SkillTemplate`.

| Route | Successful response |
| --- | --- |
| `GET /api/skills` | `{ skills: SkillSummary[], errors: [{ id, message }] }` |
| `GET /api/skills/template` | `{ text: string }` containing authoring YAML |
| `GET /api/skills/:id` | `{ id, name, title, description, keywords, revision, text }` |

Summaries contain the document fields except `text`. `revision` is an opaque
content hash; clients retain the selected snapshot until an explicit refresh.
IDs are filename stems, never paths. `template` is reserved. Missing files return
404; invalid IDs or documents return 400. A malformed individual file is listed
under `errors`; exceeding the 256-file library limit fails the list request.
The limit is 64 KiB per source file. Store UTF-8 YAML files under
`<dataDir>/skills/` with `name`, `title`, `description`, `text`, and optional
`keywords`. The panel expands `@{skill:ID}` in place when sending a message,
up to 16 references and 256 KiB of expanded text. For an editable example, see
[the code review skill](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/hub/schemas/skills/code-review.yaml).

## Optional panel decoration

Set `panel.background` in `hub.config.jsonc` and restart the Hub to load a
local PNG or JPEG. Clients receive a URL and revision in `HubMetadata.background`
and can request the image over authenticated HTTP. [Configuration example](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/hub/hub.config.example.jsonc)
