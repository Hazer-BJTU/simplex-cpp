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
`llm_chat_completions`, but deliberately has no bundled provider plugin. A
provider derives from the model and may inject a `ChatCompletionsDialect` to
overlay endpoint defaults or rewrite the completed request and each decoded
stream chunk.

The adapter intentionally covers the ReAct core:

- system, user, assistant, and tool messages;
- text and input-image content parts;
- function tool definitions, `tool_choice` passthrough, parallel
  `tool_calls`, and correlated `tool_call_id` results;
- streamed text/refusal deltas, `finish_reason`, optional usage accounting,
  API error chunks, and the `[DONE]` sentinel.

One model exchange produces one `MessageItem`, so the interpreter owns
`stream: true` and `n: 1`. Generation fields otherwise pass through, while
`messages` and `tools` are rebuilt from `AgentInputState`. A finish reason of
`stop`, `tool_calls`, or the deprecated `function_call` is successful;
`length`, `content_filter`, an API error, or a truncated stream is surfaced as
`ChatCompletionsApiException` by the model.

Audio output, legacy request-side `functions`, hosted tools, log probabilities,
and multiple choices are outside this initial layer. Their raw chunk remains
available in `ChatCompletionsDelta::extras`, and a future provider dialect can
normalize compatible extensions without changing the shared model I/O ABI.

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
  "stream_options": { "include_usage": true },
  "retry": { "max_attempts": 3 }
}
```
