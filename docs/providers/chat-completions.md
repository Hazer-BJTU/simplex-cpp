# Custom Chat Completions

**Last updated: 2026-10-15.** Use `chat-completions` for an endpoint that
accepts Chat Completions messages and returns streamed Server-Sent Events
(SSE). You configure any service-specific translations. The parser assembles
choice zero, including its text, reasoning and tool calls.
[Chat Completions API reference](https://platform.openai.com/docs/api-reference/chat/create)

<a id="minimal-worker-configuration"></a>

## Worker configuration

This example uses the OpenAI Chat Completions endpoint. For a compatible
deployment, substitute its URL, API key and model ID. Set `driver_model: primary`
in the containing worker YAML and export the key in the worker environment. [Example model and supported endpoints](https://developers.openai.com/api/docs/models/gpt-4.1-mini)

```yaml
providers:
  primary:
    plugin: chat-completions
    model: gpt-4.1-mini
    endpoint:
      base_url: https://api.openai.com
      request_path: /v1/chat/completions
      auth:
        scheme: bearer
        api_key: ${OPENAI_API_KEY}
    config:
      temperature: 0.7
```

## Parameters

| Field | Meaning |
| --- | --- |
| `endpoint.base_url` | Required service origin with an optional path prefix. If it ends in `/v1`, use `request_path: /chat/completions`. The default path is `/v1/chat/completions`. |
| `config` | Native generation fields are preserved with their JSON types. Use fields supported by your service and model. |
| `config.compatibility` | Optional startup rules for this named profile. The plugin consumes the declarations locally. |

The adapter supplies absent defaults for `stream: true`, `n: 1` and
`stream_options.include_usage: true`. Explicit values survive, but the service
must still return SSE chunks. See [shared provider fields](../reference/worker-settings.md#provider-fields)
for authentication, headers and retries.

## Request ownership and precedence

| Order | Rule or source | Operates on |
| --- | --- | --- |
| 1 | Generation settings, defaults and the worker conversation | Builds the outgoing body with current `messages` and `tools`. `replay_assistant_reasoning: true` includes saved assistant reasoning before patching. Default is `false`. |
| 2 | `request.message_patches` | Each rule visits every message whose current `role` exactly matches its selector. Its pointers start at that message. A role change is visible to later rules. |
| 3 | `request.patch` | Applies rules to the complete request body, including generated fields. |
| 4 | Selected `options[].choices[].patch` | Applies the selected choices to the body in **option declaration order**. Later writes to the same path win. |
| On receipt | `response.chunk_patch` | Applies to each decoded JSON chunk before the SSE reader interprets deltas or finish reasons. The SSE framing and `[DONE]` marker stay outside the rules. |

Rules within each patch run in list order. Each request starts from its base
generation settings and current selections. An in-flight request and its
retries retain their original settings.

## Declarative protocol patches

For a service that calls assistant reasoning `thought`, add this fragment
under the provider's `config`. The request rule renames saved reasoning on
assistant messages. The response rule performs the reverse mapping on streamed
deltas. The body rule removes the adapter's default `n` field.

```yaml
compatibility:
  replay_assistant_reasoning: true
  request:
    message_patches:
      - role: assistant
        patch:
          - {op: move, from: /reasoning_content, path: /thought, if_missing: skip}
    patch:
      - {op: remove, path: /n}
  response:
    chunk_patch:
      - {op: move, from: /choices/0/delta/thought, path: /choices/0/delta/reasoning_content, if_missing: skip}
```

For example, `{"choices":[{"delta":{"thought":"plan"}}]}` becomes
`{"choices":[{"delta":{"reasoning_content":"plan"}}]}`. A text-only delta or
usage-only chunk passes through because the source is absent and the rule
specifies `if_missing: skip`.

| Operation | Required fields | Effect |
| --- | --- | --- |
| `set` | `path`, `value` | Writes a value, including explicit null. Creates missing object parents. |
| `remove` | `path` | Removes an object member or array element. A missing target is ignored. Removing the root is invalid. |
| `copy` | `from`, `path` | Reads the source and writes a separate copy using `set` semantics. |
| `move` | `from`, `path` | Removes the source before resolving the destination. Moving the root or moving into a descendant is invalid. |

Pointers use `/` for each path segment, `~1` for a literal slash, and `~0`
for a tilde. An empty string refers to the root. Arrays require existing
numeric indices for writes. There is no wildcard or append syntax. Removing
an element shifts later indices. A numeric key beneath a newly created object
is still an object key.

For `copy` and `move`, an absent source raises an error by default.
`if_missing: skip` skips that rule only. Malformed array indices, traversal
through a scalar, and invalid destinations still fail. Invalid declarations are
rejected at startup. A patch failure during an exchange reports its rule location.

## Token cost mapping

Under the same `compatibility` mapping, set pointers **relative to the original
usage object**. For a service that reports cache hits at `usage.prompt_cache_hit_tokens`:

```yaml
token_cost:
  prompt: /prompt_tokens
  generated: /completion_tokens
  cache_hit: /prompt_cache_hit_tokens
```

The first two pointers are defaults. The default cache pointer is
`/prompt_tokens_details/cached_tokens`. Omit a key to keep its default, or
supply null to disable it. Counts must be nonnegative integers representable
as `uint64_t`. Missing prompt or generated counts suppress the cost record.
Missing cache hits yield zero in that record. The last original usage object
wins, including in a usage-only frame. Response patches do not affect these
counts or the original usage stored in metadata.

## Runtime options

The generic plugin advertises options only when declared. Add `models` and
`options` under the same `compatibility` mapping to offer a model selector:

```yaml
models:
  - {id: fast}
  - {id: accurate}
options:
  - name: model
    default: fast
    choices:
      - value: fast
        patch: [{op: set, path: /model, value: fast}]
      - value: accurate
        patch: [{op: set, path: /model, value: accurate}]
```

Replace both example IDs with models your endpoint serves. `models` publishes
a local catalog for `provider_info()`. Each entry requires a unique nonempty
`id`. The catalog neither restricts startup model IDs nor creates a panel menu.
Each menu option needs a unique name, a nonempty list of uniquely named string
choices and a patch for each choice. The optional default must name a choice.
A selection can write any JSON value, including a Boolean or number. An
invalid option batch leaves previous selections intact.

## Usage notes

Keep separate named profiles for endpoints that use different rules. For a
Hub-managed worker, select the saved configuration pair when creating a new
session. The bundled plugin's adaptation date covers its rule engine. Service
models and features follow the endpoint you select.
