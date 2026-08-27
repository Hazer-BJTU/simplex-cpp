# LLM model plugins

`llm` owns the provider-plugin ABI and protocol-level model adapters. The
`endpoint` module below it is transport-only: HTTP/HTTPS, SSE framing and retry
drivers do not know which model protocol they carry.

The plugin boundary is **live-object**: a provider `.so` mints model objects
(`create_llm_model(executor, config)` receives an `asio::any_io_executor` and
a `nlohmann::json`; `converse()` returns an `asio::awaitable` whose frame the
host resumes). Those types have no stable ABI, so safety rests on the
**same-execution-context strategy** — the toolchain-fingerprint admission
gate in `extension_framework`, the shared runtime stack
(`libasio`/`libeventbus`/`liblogging`/`libllm_chat_completions`/
`libllm_responses`), and `-rdynamic` hosts. The full contract, its mechanisms,
and their structural tests live in `docs/abi-context.md`.

## Architecture and responsibility boundaries

The central abstraction is deliberately an **exchange**, not an agent. A
conversation model consumes the complete, provider-independent
`model_io::AgentInputState` and returns exactly one finished
`model_io::MessageItem`:

```text
host/session                                      provider wire
────────────                                      ─────────────
AgentInputState
  ├─ meta                 (host only; never sent)
  ├─ system_prompt ─┐
  ├─ tools ─────────┼─> Interpreter ─> HTTP request ─> endpoint transport
  ├─ turns ─────────┘                                      │ SSE frames
  └─ extras              (reserved host regions excluded)  v
MessageItem <──────── Reader <──────── typed deltas <─ StreamHandler
     │
     └─ LLMModel::integrate() ─> updated AgentInputState
```

The layers have intentionally narrow ownership:

| Layer | Owns | Must not own |
| --- | --- | --- |
| Host/session loop | user turns, tool execution and authorization, step limits, persistence, compaction, cancellation policy | provider JSON and SSE parsing |
| `LLMDispatcher` / plugin context | plugin discovery, ABI gating, factory lookup, library lifetime | conversations and network exchanges |
| `LLMModel` | one configured provider client, one `converse()` exchange, result integration/retention policy, the runtime services (`provider_info()` catalogue query, `set_generation()` knob adjustment) | agent loops, tool execution, durable storage |
| Protocol adapter | canonical-data-to-wire mapping and wire-to-canonical assembly | provider-specific deviations that can be expressed by a dialect |
| Dialect | endpoint defaults and small JSON-level provider rewrites | transport, session state, a second model abstraction |
| `endpoint` | DNS/TCP/TLS/HTTP, SSE framing, retry/backoff and producer/consumer plumbing | LLM messages, tools, reasoning semantics |

This boundary is important for retries: a retry repeats the same already-built
request and clears the attempt-local reader. It does not append partial output
to the conversation, execute a tool twice, or rebuild session state. Only a
successfully assembled result returned by `converse()` may be integrated.

## Runtime services on the contract

Two ABI-v2 entry points serve the host between exchanges:

- **`provider_info()`** — the provider's live catalogue: one OpenAI-compatible
  `GET /models` over the model's own endpoint (auth included, path from the
  dialect's `models_path()`), returned as a JSON array whose entries are the
  provider's own model descriptors, verbatim. No retry: a catalogue query is
  cheap to re-issue. `llm_deepseek_chat --list-models` is the smoke entry.
- **`set_generation()`** — two tiers over one validated merge core. The typed
  tier takes an `llm::GenerationPreset` (the pair hosts adjust most often:
  `model` + `ReasoningEffort`) and writes the canonical
  `"reasoning": {"effort": ...}` envelope both adapters translate. The JSON
  tier applies an RFC 7386 merge-patch (present keys written, `null` erases,
  absent keys kept). Both refuse the host-owned `endpoint`/`provider`/`retry`
  keys and any patch that would leave `model` missing or empty; validation is
  atomic and `_config` stays the verbatim construction record — read the
  effective knobs back through `generation()`.

## Conversation model: canonical input and output

### Input: `AgentInputState`

`AgentInputState` is the durable, provider-neutral session value. It contains:

- `meta`: host bookkeeping such as session status and errors. An interpreter
  must never send it to a provider.
- `system_prompt`: a structured `PromptTemplate`. The interpreter renders it
  at request-build time; the rendered string is not the persisted source of
  truth.
- `tools`: `Invocable` definitions (`name`, `description`, JSON Schema, plus
  optional remote/provider metadata). Registration advertises a tool; it does
  not authorize or execute it.
- `turns`: oldest-first `UserLoopStep` values. Each turn contains one
  `user_input`, followed by zero or more `AgentLoopStep`s. An agent step is one
  model response plus the tool results produced for its calls.
- `extras`: experimental session data. The reserved `external_status` and
  `events` regions are host/plugin exchange state and must not reach the model.
  Other fields are only consumed when an adapter explicitly defines a mapping;
  normal generation configuration comes from the model config, not this value.

The nested shape preserves the causal relation that a flat chat array loses:

```text
UserLoopStep
├─ user_input
└─ AgentLoopStep[]
   ├─ model_response (possibly containing invokes[])
   └─ invoke_returns[] (correlated by invoke_return.query.id)
```

`MessageItem::content` is an ordered heterogeneous list. `ContentType::Text`
stores UTF-8, `Binary` stores base64 text, and `ExternalRef` stores a URI or
other out-of-band reference. `extras` retains protocol details that do not
deserve a new shared ABI field. An adapter must preserve content order and
must document unsupported content types instead of silently changing their
meaning.

### Output: one `MessageItem`

For a conversation exchange, `converse()` returns a `ModelResponse` item:

- `content`: visible assistant output, in provider order;
- `reasoning`: a separate reasoning/thinking channel when supplied;
- `invokes`: zero or more tool requests. Each `InvokeQuery` carries the wire
  call `id`, tool `name`, parsed `arguments`, execution/security hints, and
  optional raw metadata;
- `cost`: prompt, generated, and cached-prompt token counts when reported;
- `extras`: loss-minimizing protocol metadata such as response id, model,
  terminal reason, native usage, annotations, or unknown compatible fields.

An empty `invokes` value means the host may treat the item as the final answer
for that user turn. A non-empty value means only “the model requested tools”;
the host still validates the name/schema, applies `InvokeSecurity`, schedules
according to `InvokeType`, executes the tool, and creates `InvokeReturn`
messages. Tool failures should normally be represented as tool-result content
so the next model exchange can recover.

`InvokeReturn` duplicates the result in two useful forms: `content` is the
canonical message payload, while `invoke_return.query.id` is provenance and
wire correlation. When constructing one, keep `invoke_return.output` equal to
the first content part. Adapters may use positional correlation only as a
compatibility fallback when provenance is absent.

### Complete ReAct data flow

One user turn can contain several model exchanges:

```cpp
model.integrate(state, user_message); // opens a UserLoopStep

for (std::size_t step = 0; step != max_steps; ++step) {
    model_io::MessageItem response = co_await model.converse(state);
    model.integrate(state, response); // appends an AgentLoopStep

    if (!response.invokes || response.invokes->empty()) {
        co_return;                    // final assistant response
    }

    for (const auto& call : *response.invokes) {
        model_io::MessageItem result = co_await execute_with_policy(call);
        model.integrate(state, result); // attaches to current AgentLoopStep
    }
}
```

The host owns the loop and its maximum-step guard. `integrate()` supplies only
the structural placement rule:

| Item type | Placement |
| --- | --- |
| `UserInput` | append a new `UserLoopStep` |
| `ModelResponse` | append an `AgentLoopStep` to the current user turn |
| `InvokeReturn` | append to the current agent step's `invoke_returns` |

It creates missing parent containers for restored or imperfect state rather
than dropping data. It does not deduplicate, compact, persist, or decide that a
turn is complete. A provider may override integration to change retention of
provider-required replay data, but should not change these placement semantics.

### One exchange, end to end

1. `LLMDispatcher::create_model(provider, executor, config)` finds an
   ABI-compatible plugin and atomically constructs a configured model.
2. `create_model()` invokes `build()` once. The model overlays the configured
   endpoint on dialect defaults, validates it, separates host-only keys
   (`provider`, `endpoint`, `retry`) from generation JSON, and initializes
   immutable exchange state.
3. `converse(state)` asks the protocol Interpreter to render the prompt,
   flatten turns in causal order, map tools and multimodal parts, and rebuild
   builder-owned wire fields.
4. The Dialect applies the last provider-specific request rewrite.
5. `endpoint::complete` resolves/connects, sends the request, and pumps the
   response. Retryable failure starts a fresh connection and resets all
   attempt-local decoding/accumulation state.
6. The StreamHandler performs framing and stateless event/chunk decoding. A
   dialect may normalize the decoded JSON before it becomes a typed delta.
7. The Reader consumes deltas in wire order, invokes observation hooks,
   accumulates fragmented text/reasoning/tool arguments/usage, recognizes the
   terminal state, and assembles one canonical `MessageItem`.
8. The model rejects non-success terminal states with a protocol exception;
   otherwise it returns the item without mutating `state`.
9. The host calls `integrate()` only after accepting that result. At session
   teardown the plugin-owned deleter calls `release()` and then destroys the
   model while its dynamic library is still pinned.

Streaming deltas are deliberately not the return type of `LLMModel`: they are
attempt-local and protocol-specific, whereas `MessageItem` is stable and
persistable. Live UI/telemetry attaches through reader hooks or process-wide
events. Observers must account for retries: hooks survive `clear()` and can see
more than one attempt, while only the final successful assembly is returned.

## Designing a complete LLM interface

Use the following order when adding a new conversation protocol or provider.
It keeps the shared ABI small and makes most behavior testable without a live
service.

1. **Choose the canonical shape first.** Reuse `AgentInputState ->
   MessageItem` for conversational protocols, including multimodal input. Add
   a new `LLMModelType` and a new appended virtual entry point only when the
   input/output data shape is genuinely different (embedding vectors, image
   generation, transcription, and so on). Never force those products into a
   chat message merely to reuse `converse()`.
2. **Define loss and ownership explicitly.** Specify role mapping, supported
   content parts, tool-call correlation, reasoning replay, terminal success
   states, usage mapping, unknown-field retention, and which component owns
   each request key. Reject impossible mappings; retain harmless unknown data
   in `extras`.
3. **Implement a pure Interpreter.** Its inputs are state, endpoint, and
   generation JSON; its output is a complete HTTP request. It performs no I/O,
   derives empty roles from item type, tolerates sparse restored state, and
   lets builder-owned history/tool/stream keys override stale config values.
4. **Define a typed delta and decoder.** The StreamHandler should only turn
   wire events into deltas and identify `[DONE]`/terminal/error events. It
   should not build conversation objects or own cross-event accumulation.
5. **Implement a per-exchange Reader.** Accumulate fragmented fields by their
   stable wire index/id, parse tool arguments only after completion, preserve
   refusal/reasoning separately, assemble token cost, retain useful native
   metadata, and expose an unambiguous status for completed, truncated,
   filtered, length-limited, cancelled, and failed streams.
6. **Compose the model.** In `build()`, validate config and endpoint without
   throwing across the lifecycle boundary. In `converse()`, create fresh
   interpreter/reader/completer state, run the exchange, translate terminal
   failure to a typed exception, and return only a complete canonical item.
   Keep the instance sequential unless concurrent use is explicitly designed
   and documented.
7. **Isolate provider variance in a Dialect.** Prefer endpoint defaults plus
   `transform_request()` and event/chunk normalization. Derive a new model only
   when behavior cannot be expressed by that narrow policy object. Provider
   types must not cross the plugin ABI.
8. **Export and register the plugin.** Export `create_llm_plugin` for the
   descriptor and the signed `create_llm_model(executor, config)` factory.
   Report the shared `LLM_PLUGIN_ABI_VERSION`; let the context own `build()`,
   `release()`, and dynamic-library pinning.
9. **Test by layer.** Interpreter golden tests cover exact JSON and history
   order; decoder tests cover fragmented/unknown/error events; reader tests
   cover assembly and every terminal status; model tests cover config overlay,
   retries and exception mapping; plugin tests cover aliases, ABI admission and
   factory lifetime. Finally add one multi-turn tool round trip that serializes
   and restores `AgentInputState` between exchanges.

A minimal protocol model therefore has this composition (names are examples):

```cpp
class MyModel final : public llm::LLMModel {
public:
    LLMModelType model_type() const noexcept final {
        return LLMModelType::Conversation;
    }
    bool build() noexcept override; // parse/validate config once
    boost::asio::awaitable<model_io::MessageItem> converse(
        const model_io::AgentInputState&) override;

private:
    model_io::ModelEndpoint endpoint_;
    nlohmann::json generation_;
    std::shared_ptr<const MyDialect> dialect_;
};

// converse():
// MyInterpreter -> complete<MyDelta> -> MyStreamHandler -> MyReader
```

Before calling an interface complete, verify four invariants: canonical state
round-trips through JSON; a failed or retried exchange cannot mutate it; every
tool result can be correlated after restore; and every successful provider
terminal form produces exactly one `ModelResponse`. These invariants are more
important than matching any one provider's vocabulary.

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
