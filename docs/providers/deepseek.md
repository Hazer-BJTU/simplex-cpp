# DeepSeek

**Last updated: 2026-10-01.** This page describes the bundled implementation,
not a remotely queried model catalog. Check your account's available models
before deployment.

## Implementation

The `deepseek` plugin uses the shared Chat Completions adapter. Its dialect
supplies `https://api.deepseek.com` with `/chat/completions`, bearer authentication,
thinking/reasoning handling, reasoning-content replay, and cache-hit usage
normalization. Provider execution and retries remain inside the model layer.

The plugin advertises these local runtime options:

| Name | Choices |
| --- | --- |
| `model` | `deepseek-flash`, `deepseek-v4-pro` |
| `reasoning_effort` | `low`, `high`, `max` |

These are the choices returned by `get_options()`, not a statement that every
account or compatible third-party endpoint accepts them. Startup `model` and
provider-specific `config` are distinct from the validated runtime option patch.
The endpoint remains configurable for a compatible service.

## Worker configuration

Copy this provider section into your worker YAML. Shared endpoint,
authentication, and retry semantics are described in
[Support policy](index.md#shared-fields-and-request-ownership).

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

Select this configuration with `driver_model: deepseek`; optionally set
`modality_assist_model: deepseek` for a separate image-assistance instance.
Export `DEEPSEEK_API_KEY` in the environment that actually starts the worker.
The loader expands credentials only for selected model configurations.
For a full worker file, see the
[minimal configuration](../getting-started/configuration.md#hub-managed-sessions),
which names its provider entry `primary` instead.

## Use from the Hub

Save the worker YAML in the configuration library. Select it together with a
local or Docker launch file when creating a session. The Hub fills worker
connection endpoints but preserves provider configuration. For an existing
session, stop it, apply the saved configuration pair, and restart it.

The panel obtains advertised model choices through the worker's `options`
signal. Selected changes are attached to the next run-starting payload; they
do not mutate an active run. Runtime selections are not persisted as startup
configuration in AgentInputState.

## Optional image assistance

```yaml
modality_assist_model: deepseek
```

This creates a **separate** model instance even when both roles select `deepseek`.
It enables an intrinsic tool that reads local image files, encodes them as base64,
and performs one isolated model exchange to return a textual description.
The endpoint/model must support the resulting image input. A driver invocation
can call this tool; merely setting the role does not automatically convert every
incoming attachment. Every supplied image path must succeed or the call fails.

## Failure diagnosis

Check worker logs for missing credentials, incompatible plugin libraries,
endpoint/authentication failures, and unsupported provider requests. Exhausted
model retries are reported as failed runs; continuation is possible only when
the settled state permits it. Provider errors must not be mistaken for lost
conversation state or proof that earlier tool effects were undone.
