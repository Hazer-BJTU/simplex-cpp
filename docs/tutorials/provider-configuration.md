# Configure a model provider

These examples cover each provider in the [supported provider list](../providers/index.md):
DeepSeek and Qwen. Copy the relevant `providers` section into your worker YAML,
then select its entry name through the worker's model settings as described in
[Configuration](../getting-started/configuration.md). They are configuration
fragments, not complete worker files.

Each example includes every provider-entry, endpoint, authentication, and retry
field understood by the worker, plus the plugin's own generation controls.
Optional alternatives are commented out so mutually exclusive settings are not
sent together. Generation `config` is an open JSON object: other request fields
are forwarded to the service, subject to the transformations below. There is no
finite plugin-defined list of all possible passthrough fields, and forwarding a
field does not guarantee that a particular remote model supports it.

## DeepSeek

```yaml
providers:
  deepseek:
    plugin: deepseek
    endpoint:
      base_url: https://api.deepseek.com
      request_path: /chat/completions
      auth:
        scheme: bearer
        api_key: ${DEEPSEEK_API_KEY}
        header_name: x-api-key  # Used only with scheme: custom_header.
        extras: {}             # Metadata; not sent by this plugin.
      user_agent: simplex-cpp/deepseek
      extra_headers: {}
      extras: {}               # Metadata; not generation parameters.
    model: deepseek-flash      # Also advertised: deepseek-v4-pro.
    config:
      reasoning_effort: high   # Public runtime choices: low, high, max.
      thinking:
        type: enabled          # Native toggle: enabled or disabled.
      # Alternative to reasoning_effort, not an additional control:
      # reasoning:
      #   effort: high
      # Optional passthrough examples; values are not plugin defaults:
      # max_tokens: 8192
      # temperature: 1.0
      # top_p: 1.0
      # stop: ["END"]
      # response_format: {type: json_object}
      # tool_choice: auto
      # logprobs: true
      # top_logprobs: 5
    retry:
      max_attempts: 3
      initial_backoff_ms: 500
      max_backoff_ms: 120000
```

The endpoint, authentication scheme, and user agent above are the plugin's
transport defaults. The model and explicit generation settings are example
choices. If both thinking controls are omitted, the plugin enables thinking but
does not supply a reasoning effort.

| Control | Plugin behavior |
| --- | --- |
| `thinking` | A native object is forwarded unchanged and takes precedence over the local effort-based thinking toggle. Use `type: disabled` and omit the effort to disable thinking explicitly. |
| `reasoning_effort` | `low`, `high`, and `max` are advertised to clients. Startup configuration is permissive; other values reach the service unless consumed by the local toggle. |
| `reasoning.effort` | Shared compatibility spelling. Converted to `reasoning_effort`; an explicit top-level value wins. The `reasoning` envelope is removed. |
| `none` / `minimal` effort | Legacy local disable aliases: without a native `thinking` object, disable thinking and remove the effort from the request. They do not override an explicit native object. |
| `n`, `frequency_penalty`, `presence_penalty` | Removed before transmission; do not use them as effective settings. |

Keep native thinking and effort settings consistent. Sampling, output limits,
structured output, and other passthrough parameters are validated by the remote
service; some depend on thinking mode. For JSON output, also instruct the model
to produce JSON. Consult the [DeepSeek request reference](https://api-docs.deepseek.com/api/create-chat-completion/)
for current server-side constraints.

## Qwen

```yaml
providers:
  qwen:
    plugin: qwen
    endpoint:
      base_url: https://maas.qianwenaiapi.com
      request_path: /compatible-mode/v1/chat/completions
      auth:
        scheme: bearer
        api_key: ${QWEN_API_KEY}
        header_name: x-api-key  # Used only with scheme: custom_header.
        extras: {}             # Metadata; not sent by this plugin.
      user_agent: simplex-cpp/qwen
      extra_headers: {}
      extras: {}               # Metadata; not generation parameters.
    model: qwen3.8-flash        # Also advertised: qwen3.8-max.
    config:
      enable_thinking: true
      preserve_thinking: false # Flash default; Max defaults to true.
      reasoning_effort: xhigh  # low, medium, or xhigh.
      # Alternatives: remove reasoning_effort before enabling either one.
      # reasoning:
      #   effort: xhigh
      # thinking_budget: 16384 # Cannot coexist with either effort spelling.
      tool_stream: true       # Explicit choice; no local default is injected.
      modalities: [text]      # Output modality; image input remains supported.
      tool_choice: auto       # "required" is rejected locally.
      # Optional passthrough examples; values are not plugin defaults:
      # max_tokens: 8192
      # temperature: 1.0
      # top_p: 1.0
      # stop: ["END"]
      # response_format: {type: json_object}
    retry:
      max_attempts: 3
      initial_backoff_ms: 500
      max_backoff_ms: 120000
```

The endpoint, authentication scheme, and user agent above are the plugin's
transport defaults. The following family defaults apply to `qwen3.8-flash`,
`qwen3.8-max`, and their hyphen-suffixed snapshots. Other model names are accepted,
but omitted thinking controls are not filled in using these family defaults.

| Control | Type and behavior |
| --- | --- |
| `enable_thinking` | Boolean; defaults to `true` for the two supported families. YAML uses `true`/`false`, not the UI option labels `enabled`/`disabled`. |
| `preserve_thinking` | Boolean; defaults to `false` for Flash and `true` for Max. Controls replay of previous assistant reasoning as well as the outgoing request field. |
| `reasoning_effort` | `low`, `medium`, or `xhigh`; defaults to `xhigh` when neither effort nor budget is supplied. For these families, startup aliases `high` and `max` normalize to `xhigh`. |
| `reasoning.effort` | Shared compatibility spelling; the top-level effort takes precedence and the envelope is removed. |
| `thinking_budget` | Nonnegative integer, at most `262144` for the supported families. Mutually exclusive with an explicit effort, including `reasoning.effort`. Suppresses the default effort. |
| `tool_stream` | Boolean; omitted unless configured. Remote model support still applies. |
| `modalities` | If supplied, must be exactly `[text]`. This restriction concerns generated output, not image input. |
| `tool_choice` | `required` is rejected locally. Other values are forwarded for server validation. |

Put Qwen controls directly under `config`; the Python SDK's `extra_body` wrapper
and DeepSeek's `thinking` object are rejected. Audio output settings are also
rejected. `n` is removed before transmission. For other model families, an explicit
effort must be a nonempty string and a budget must be a nonnegative integer;
the server determines which values it supports.

Sampling, output limits, and structured-output fields are passed through. Check
the [Qwen Chat Completions reference](https://platform.qianwenai.com/docs/api-reference/chat/openai-chat)
for the chosen model's current constraints. See the [Qwen guide](../providers/qwen.md)
for vision input, reasoning replay, and usage accounting.

## Shared fields and request ownership

| Field | Meaning and default |
| --- | --- |
| Provider entry name | Worker-local identifier; it may differ from the plugin name. |
| `plugin` | Installed model plugin name. Defaults to the provider entry name when omitted. |
| `model` | Required nonempty model identifier. Startup loading is not restricted to the runtime option catalog. |
| `endpoint.base_url` | Service origin, optionally with a path prefix; inherits the plugin default when omitted. |
| `endpoint.request_path` | Appended to the base URL's path prefix. Avoid duplicating `/compatible-mode/v1` when using a Qwen SDK-style base URL. |
| `endpoint.auth.scheme` | `bearer` (default), `custom_header`, or `none`. Custom-header authentication uses `header_name` and sends the key without a Bearer prefix. |
| `endpoint.auth.api_key` | Credential; `${NAME}` references resolve from the worker environment. Omit for unauthenticated endpoints. |
| `endpoint.auth.header_name` | Defaults to `x-api-key`; relevant only to custom-header authentication. |
| `endpoint.auth.extras` / `endpoint.extras` | Optional metadata retained in endpoint configuration. Neither plugin uses these to add request fields. |
| `endpoint.user_agent` | Inherits the plugin-specific default when omitted. |
| `endpoint.extra_headers` | Additional string-valued headers. Values support environment expansion; these headers are applied after authentication and User-Agent headers and can override them. |
| `config` | Generation object, default `{}`. Do not nest host-owned `model`, `provider`, `endpoint`, or `retry` here. |
| `retry.max_attempts` | Retries **after** the initial request; default `3`, so at most four attempts. Set `0` to disable retries. |
| `retry.initial_backoff_ms` | Positive initial delay; default `500`. |
| `retry.max_backoff_ms` | Positive delay cap, at least the initial delay; default `120000`. |

Retry integers must fit a signed 32-bit integer. Retry policy only retries
recoverable failures; it does not make every error retryable.

Environment substitution applies to the selected provider's API key and extra
header values, not arbitrary YAML strings. Referenced variables must exist and
be nonempty. Prefer environment references over storing credentials in YAML.

The worker constructs `messages` and `tools` from the current loop state. The
shared adapter enforces streaming and usage collection (`stream: true` and
`stream_options.include_usage: true`), and both providers remove `n`. These are
request mechanics, not user-configurable alternatives. Other members of
`stream_options` are preserved. A provider configuration does not supply image
content: images arrive through message content or the modality-assist tool.

When editing an existing Hub session, update its persisted worker configuration
and restart the worker for startup settings to take effect. Runtime model options
expose a smaller set of controls; they are not a replacement for this YAML.
