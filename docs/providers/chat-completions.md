# Custom Chat Completions

**Last updated: 2026-10-15.** The `chat-completions` plugin is the configurable
fallback for services without a dedicated Simplex provider. It uses the shared
Chat Completions adapter, with protocol differences declared in your worker's
provider configuration. No provider names, model families, thinking modes or
model catalogs are inferred.

Use a dedicated provider when you want its maintained defaults and service
policies. Use this plugin when you want to control the request fields and
compatibility rules yourself. The examples below demonstrate configuration,
not live verification of a particular service.

## Minimal worker configuration

Merge this fragment into an existing worker YAML, retaining its client,
persistence and other settings:

```yaml
providers:
  custom:
    plugin: chat-completions
    endpoint:
      base_url: https://your-service.example
      request_path: /v1/chat/completions
      auth:
        scheme: bearer
        api_key: ${CUSTOM_API_KEY}
    model: your-model
    config:
      temperature: 0.7

driver_model: custom
# modality_assist_model: custom
```

There is no default service origin. The default request path is
`/v1/chat/completions`; authentication defaults to Bearer. The endpoint's path
is appended to the base URL's path prefix. For a base URL ending in `/v1`, use
`request_path: /chat/completions`. Headers, environment expansion and retry
settings follow the [shared configuration contract](index.md#shared-fields-and-request-ownership).

All customization belongs to `config.compatibility` in this provider entry.
The plugin consumes that object locally; it never sends it to the service.
Other `config` fields are native generation parameters. Different named
entries can use this plugin with independent endpoints, rules and options.
Edit the persisted worker YAML and restart the worker to reload declarations.
Runtime options change only the selections described below.

## Request ownership and precedence

Requests are assembled in this order:

1. Supply absent defaults: `stream: true`, `n: 1`, and
   `stream_options.include_usage: true`.
2. Preserve explicit generation parameters, including unknown fields and their
   JSON types. An explicit non-object `stream_options` is also preserved.
3. Build `messages` and `tools` from the current conversation. Configured
   `messages`/`tools` do not replace the live conversation at this step.
4. Apply `request.message_patches` in declaration order to matching roles.
5. Apply `request.patch` in declaration order.
6. Apply selected options' patches in **option declaration order**, irrespective
   of when options were selected.
7. Serialize and send. No parameter normalization follows these patches.

The plugin does not translate `reasoning.effort`, clamp reasoning effort,
remove unsupported parameters, unwrap `extra_body`, or infer behavior from a
model name. A deleted field stays deleted. A field changed by the final patch
stays changed. Set native fields directly or declare the translation you need.
The C++ typed generation preset still produces the shared `reasoning.effort`
envelope; this plugin leaves it unchanged unless you declare a patch.

`replay_assistant_reasoning` defaults to `false`. Set it to `true` to include
stored assistant reasoning as `reasoning_content` before message patches run.
This is independent of whether the service is asked to generate new reasoning.

The transport still sends JSON over HTTP and consumes **SSE Chat Completions**
responses, assembling choice index zero. Input message capabilities remain
those of the shared adapter; output is text, reasoning and agent tool calls.
Setting `stream: false` or requesting multiple candidates is forwarded, but
does not add a non-streaming parser or multiple-result support. SSE framing,
`[DONE]`, tool-call assembly and completion/error handling remain shared.

## Declarative protocol patches

```yaml
config:
  compatibility:
    replay_assistant_reasoning: true
    request:
      message_patches:
        - role: assistant
          patch:
            - op: remove
              path: /reasoning_content
      patch:
        - op: remove
          path: /n
        - op: set
          path: /thinking/type
          value: enabled
    response:
      chunk_patch:
        - op: move
          from: /choices/0/delta/thought
          path: /choices/0/delta/reasoning_content
          if_missing: skip
```

Paths use JSON Pointer: `/` separates components, `~1` escapes a literal `/`,
and `~0` escapes a literal `~`. The empty string addresses the document root.
Paths in message patches are relative to one message; request paths are relative
to the request body; response paths are relative to each decoded JSON chunk.

| Operation | Fields | Behavior |
| --- | --- | --- |
| `set` | `path`, `value` | Set or replace a value, including explicit null. Create missing object parents. |
| `remove` | `path` | Remove an existing field or array element; missing paths do nothing. Cannot remove the document root. |
| `copy` | `from`, `path`, optional `if_missing` | Copy a value to the destination, using `set` semantics. |
| `move` | `from`, `path`, optional `if_missing` | Remove the source, then set the destination. Cannot move the root or move a value into its own descendant. |

This is a small plugin rule language, **not the complete RFC 6902 JSON Patch
standard**. Arrays require explicit existing indices: no wildcards, `-` append,
implicit array creation or expansion. Removing an element shifts subsequent
indices. A move resolves its destination after removing its source. Missing
object parents are created as objects, including numeric-looking keys.
Traversing a scalar or null is an error, not an instruction to replace it with
an object. `set` can replace the root with a new JSON value.

For `copy` and `move`, `if_missing` is `error` by default. Set it to `skip` when a
field is optional, especially in SSE frames where reasoning, choices or usage
may be absent. Invalid array index syntax remains an error even with `skip`.

Rules execute in order. Unknown declaration fields, invalid pointers, duplicate
option/model names and malformed rules are rejected locally. Runtime rule
failures identify the rule's configuration path and propagate as errors; they
are not silently discarded or retried as network failures. Diagnostics do not
include the request's contents. No scripts, templates or network lookups are
executed by these rules.

Response rules run on decoded JSON chunks before the shared parser extracts
text, reasoning, tool calls and finish reasons. `[DONE]` and other SSE framing
are outside this patch mechanism. A response rule must produce the structure
that the shared parser understands.

## Token cost mapping

```yaml
config:
  compatibility:
    token_cost:
      prompt: /prompt_tokens
      generated: /completion_tokens
      cache_hit: /prompt_cache_hit_tokens
```

Each pointer is relative to the **original `usage` object**, before response
patches. Mapping populates the internal `TokenCost` directly; it does not
rewrite usage into OpenAI field names. `extras.usage` retains the original
usage, even if a response patch edits or deletes that object's fields.

| Internal count | Default pointer |
| --- | --- |
| `prompt` | `/prompt_tokens` |
| `generated` | `/completion_tokens` |
| `cache_hit` | `/prompt_tokens_details/cached_tokens` |

Omitting a mapping keeps its default. Setting a mapping to `null` disables it.
An explicit path has no fallback to guessed aliases. Only nonnegative integer
values representable by `uint64_t` are accepted; strings and floating-point
values are not coerced. Missing or invalid prompt/generated counts suppress
the complete cost record. Missing cache counts use the existing `TokenCost`
zero default, which is not evidence that the service measured zero cache hits.
Cache hits are a subset of prompt tokens, not an additional token total.

Usage may accompany a choice or appear in a usage-only frame. The last original
usage object replaces the previous one; partial objects are not merged across
frames, and cumulative counters are not summed. Retry attempts reset usage.
This feature maps token counts, not prices or currency, and supports no arithmetic
expressions. Usage synthesized only by a response patch is not original usage
and is not used for cost accounting.

## User-declared model catalog and options

```yaml
config:
  compatibility:
    models:
      - id: custom-model
        description: My primary deployment
      - id: custom-model-fast
    options:
      - name: model
        default: custom-model
        choices:
          - value: custom-model
            patch:
              - op: set
                path: /model
                value: custom-model
          - value: custom-model-fast
            patch:
              - op: set
                path: /model
                value: custom-model-fast
      - name: thinking
        default: enabled
        choices:
          - value: enabled
            patch:
              - op: set
                path: /enable_thinking
                value: true
          - value: disabled
            patch:
              - op: set
                path: /enable_thinking
                value: false
```

`models` is an array of objects with unique nonempty string `id` fields.
Additional catalog fields are retained. `provider_info()` returns this array
without making an HTTP request; if omitted, it returns `[]`. The plugin does
not contact `/models`, add the current model, or restrict startup model names
to this catalog.

`options` is an ordered array of descriptors. Each has a unique nonempty string
`name`, a nonempty `choices` array, and an optional `default` naming a choice.
Each choice has a unique nonempty string `value` and a `patch` array. Option
values use strings to match the worker protocol; their patches can write
Boolean, numeric, object, array or null values to the request.

`get_options()` exposes these declarations as the existing `{name, options}`
worker descriptors. `get_current_options()` returns explicitly defaulted or
selected values; options without a selection are omitted. No reverse inference
from generation fields takes place. Model catalog entries do not automatically
create a model option: declare its choices and patches separately.

A batch update validates every selected name/value before committing any of
it. The request snapshots generation parameters and all selections together.
In-flight exchanges and their retries keep that snapshot. Each request starts
from its base generation parameters, so switching options never leaves behind
fields added by a previous selection. Current selections override conflicting
base generation updates. Updating native generation fields does not reset the
selected options. As with other providers, persistence of runtime selections
is the host's responsibility.

## Service-shaped examples

These examples show explicit compatibility rules, not automatic service detection
or complete replacements for the dedicated providers.

### DeepSeek-style thinking and cache counts

```yaml
config:
  compatibility:
    replay_assistant_reasoning: true
    request:
      patch:
        - {op: remove, path: /n}
        - {op: remove, path: /frequency_penalty}
        - {op: remove, path: /presence_penalty}
    token_cost:
      cache_hit: /prompt_cache_hit_tokens
    options:
      - name: thinking
        default: enabled
        choices:
          - value: enabled
            patch:
              - {op: set, path: /thinking/type, value: enabled}
          - value: disabled
            patch:
              - {op: set, path: /thinking/type, value: disabled}
              - {op: remove, path: /reasoning_effort}
```

Supply the appropriate endpoint and model in the enclosing provider entry.
Unlike the dedicated DeepSeek plugin, this example does not interpret
`reasoning_effort: none/minimal` as a thinking toggle.

### Qwen-style thinking and history

```yaml
config:
  enable_thinking: true
  reasoning_effort: xhigh
  preserve_thinking: true
  compatibility:
    replay_assistant_reasoning: true
    request:
      patch:
        - {op: remove, path: /n}
    options:
      - name: preserve_thinking
        default: enabled
        choices:
          - value: enabled
            patch:
              - {op: set, path: /preserve_thinking, value: true}
          - value: disabled
            patch:
              - {op: set, path: /preserve_thinking, value: false}
```

This option only changes the server control; it does not remove historical
`reasoning_content`. To omit that history from requests, set
`replay_assistant_reasoning: false`, or add an assistant message patch removing
`/reasoning_content`. There are no model-family defaults, effort conversions or
Qwen-specific parameter checks in this custom plugin.
