# LLM model plugins

`llm` owns the provider-plugin ABI and protocol-level model adapters. The
`endpoint` module below it is transport-only: HTTP/HTTPS, SSE framing and retry
drivers do not know which model protocol they carry.

## Responses adapter

`llm::responses::ResponsesModel` is a complete `LLMModel` implementation for
the canonical Responses API. It combines:

- `ResponsesInterpreter`: `AgentInputState` to a streaming `POST /responses`
  request;
- `ResponsesStreamHandler`: SSE event decoding with raw unknown-event
  passthrough;
- `ResponsesReader`: item/content accumulation, authoritative terminal-output
  fallback, token usage and reasoning/function-call round-tripping;
- `ResponsesDialect`: provider defaults and JSON-level request/event rewrites.

A provider plugin derives from `ResponsesModel`, injects a dialect and exports
the standard `create_llm_plugin` and `create_llm_model` aliases. The bundled
`openai` plugin defaults to `https://api.openai.com/v1/responses` with Bearer
authentication and is emitted under `bin/plugins/llm`.

### Message content

`model_io::MessageItem::content` is an ordered `std::vector<Content>`. A single
user message may therefore combine text, images, and files, for example:

```cpp
model_io::MessageItem message;
message.type = model_io::MessageItemType::UserInput;
message.content = {
    {model_io::ContentType::Text, "Describe this image"},
    {model_io::ContentType::ExternalRef,
     "https://example.com/image.png",
     nlohmann::json{{"type", "input_image"}, {"detail", "high"}}},
};
```

The serialized `content` field is always a JSON array. Deserialization also
accepts the former single-`Content` object and promotes it to a one-element
array so existing persisted conversations remain readable. The Responses
adapter preserves list order and emits every entry as one content part in the
same provider message. `Text` becomes `input_text`; `ExternalRef` defaults to
`input_image`; `Binary` defaults to `input_file`. Set `extras.type` to
`input_image` or `input_file` when the default is not appropriate, and put
provider fields such as `detail`, `filename`, or `file_id` in `extras`.

## Configuration

```json
{
  "model": "provider-model-id",
  "endpoint": {
    "auth": { "scheme": "bearer", "api_key": "..." }
  },
  "temperature": 0.2,
  "reasoning": { "effort": "medium" },
  "retry": {
    "initial_backoff_ms": 500,
    "max_backoff_ms": 120000,
    "max_attempts": 4
  }
}
```

Provider endpoint defaults are recursively overlaid by `endpoint`. Host-only
keys (`provider`, `endpoint`, `retry`) are removed before request construction;
the remaining keys pass through to the provider body, while the interpreter
owns `input`, `tools`, `instructions` and `stream`.

`retry.max_attempts` is the maximum number of **retries**: only retries count
(the initial exchange does not), a successful exchange returns immediately,
and `0` disables retrying — one exchange whose failure propagates as-is.

When `store` is false or omitted, the adapter requests
`reasoning.encrypted_content` so completed reasoning items can be replayed in a
later stateless turn.

## Chat Completions adapter

`llm::chat_completions::ChatCompletionsModel` is the provider-neutral,
OpenAI-compatible `POST /chat/completions` counterpart. It is built as
`llm_chat_completions`; the bundled `deepseek` provider plugin (below) is its
first concrete provider. A provider derives from the model and may inject a
`ChatCompletionsDialect` to overlay endpoint defaults, rewrite the completed
request and each decoded stream chunk, or opt into assistant reasoning replay
(`replay_assistant_reasoning` — off by default because strict servers reject
the unknown `reasoning_content` message field).

The adapter intentionally covers the ReAct core:

- system, user, assistant, and tool messages;
- text and input-image content parts;
- function tool definitions, `tool_choice` passthrough, parallel
  `tool_calls`, and correlated `tool_call_id` results;
- streamed text/refusal/reasoning (`reasoning_content`) deltas,
  `finish_reason`, usage accounting, API error chunks, and the `[DONE]`
  sentinel.

One model exchange produces one `MessageItem`, so the interpreter owns
`stream: true`, `n: 1`, and `stream_options.include_usage: true` (the
empty-choices usage trailer is what the reader's cost accounting consumes;
sibling `stream_options` survive and a dialect may strip the whole object).
Streamed reasoning assembles into `MessageItem::reasoning`, which default
`integrate()` keeps in replayed history. The shared host envelope's
`reasoning.effort` is translated to the top-level `reasoning_effort` (an
explicit top-level value wins; the `reasoning` object is always consumed —
chat servers reject unknown top-level parameters). Other generation fields
pass through, while `messages` and `tools` are rebuilt from
`AgentInputState`. A finish reason of `stop`, `tool_calls`, or the deprecated
`function_call` is successful; `length`, `content_filter`, an API error, or a
truncated stream is surfaced as `ChatCompletionsApiException` by the model
(DeepSeek's `insufficient_system_resource` therefore reads as a failed
exchange).

Audio output, legacy request-side `functions`, hosted tools, log probabilities,
and multiple choices are outside this initial layer. Their raw chunk remains
available in `ChatCompletionsDelta::extras`, and a future provider dialect can
normalize compatible extensions without changing the shared model I/O ABI.

### Live observation

Streamed reasoning is observable live: `converse()` broadcasts every
reasoning increment as `llm::chat_completions::ReasoningDeltaEvent` on
`eventbus::default_bus()` (`llm/chat_completions/events.hpp` carries the full
contract) — synchronous, wire-ordered, one stable `reasoning_id` per exchange
(retries re-broadcast under the same id), with `provider` (the dialect's
`provider_name()`) and `model` attached. No subscribers means a silent no-op;
subscribers run inline on the exchange's I/O thread and must not throw. Bus
uniqueness across the plugin boundary comes from the SHARED eventbus library
(`default_bus()` is deliberately not inline): a host executable and its
dlopened providers bind the same SONAME, hence one bus per process.
`llm/example` subscribes to mirror the thinking to stderr as it streams.

Configuration follows the Responses adapter's host envelope. The endpoint
must be supplied by the eventual provider or caller:

```json
{
  "model": "provider-model-id",
  "endpoint": {
    "base_url": "https://provider.example.com",
    "request_path": "/v1/chat/completions",
    "auth": { "scheme": "bearer", "api_key": "..." }
  },
  "retry": { "max_attempts": 3 }
}
```

## DeepSeek provider

`llm/providers/deepseek` emits `libllm_deepseek.so` next to `libllm_openai.so`
under `bin/plugins/llm` and routes as provider name `deepseek`. The
header-only dialect (`llm/deepseek/dialect.hpp`) carries every deviation from
the neutral wire:

- endpoint defaults `https://api.deepseek.com/chat/completions` with Bearer
  auth and user agent `simplex-cpp/deepseek`; models `deepseek-v4-flash` and
  `deepseek-v4-pro` (the experimental `deepseek-v4-flash-vision-exp` image
  model works through the adapter's input-image content parts unchanged);
- thinking mode: the dialect always emits `thinking` — `enabled` by default,
  `disabled` when the effort is `none`/`minimal` — unless the config already
  carries a native `thinking` object, which passes through verbatim;
- `reasoning_effort` passes through verbatim: live 2026-08 the server accepts
  `none|minimal|low|medium|high|xhigh|max` and rejects anything else with a
  400 that enumerates the vocabulary, so an invalid value fails loudly at
  the server instead of being silently reshaped;
- `n` is rejected server-side ("currently only n = 1 is supported", live
  2026-08) and the deprecated no-op `frequency_penalty`/
  `presence_penalty` are stripped from every request;
- intermediate assistant messages replay their `reasoning_content`: the
  endpoint-prototype era documented a hard 400 when thinking+tools omitted
  it, which live 2026-08 probing no longer reproduces — replay is kept as
  the canonical multi-turn shape (and it keeps the replayed prefix
  byte-stable, which DeepSeek's automatic context cache rewards: cache hits
  grow turn over turn in live sessions), so the dialect opts into
  `replay_assistant_reasoning`;
- usage reports cache hits as `prompt_cache_hit_tokens`; the dialect bridges
  that spelling into `prompt_tokens_details.cached_tokens` so
  `MessageItem::cost.cache_hit` is populated, while `extras.usage` keeps the
  native fields.

```json
{
  "provider": "deepseek",
  "model": "deepseek-v4-flash",
  "endpoint": { "auth": { "api_key": "${DEEPSEEK_API_KEY}" } },
  "reasoning": { "effort": "high" },
  "temperature": 0.3
}
```

The `endpoint` object overlays the dialect's defaults recursively, so
`base_url`/`request_path` only need spelling out for a proxy or a
self-hosted gateway.
