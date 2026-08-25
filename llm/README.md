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
