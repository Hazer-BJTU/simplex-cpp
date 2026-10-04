# Model providers

The documented providers are **DeepSeek** and **Qwen**. Provider pages
record when their repository implementation and configuration were last reviewed;
the date is not a claim of live service availability or exhaustive remote testing.

| Provider | Plugin name | Adapter | Last updated |
| --- | --- | --- | --- |
| [DeepSeek](deepseek.md) | `deepseek` | Chat Completions | 2026-10-01 |
| [Qwen](qwen.md) | `qwen` | Chat Completions | 2026-10-04 |

The source tree also builds an `openai` provider using the Responses adapter.
Its presence in the installation does not imply the same support/verification
scope as the provider guides. The Hub's mock provider is a test service rather
than a production model provider.

Provider libraries are discovered independently of named YAML configurations.
Only configurations selected by `driver_model` and optional
`modality_assist_model` construct model instances. See
[model plugin development](../plugins/model-providers.md) for adding another
implementation.

## Provider configuration

The [DeepSeek](deepseek.md#worker-configuration) and
[Qwen](qwen.md#worker-configuration) pages contain complete provider-only
YAML examples and each plugin's generation controls. Copy the relevant
`providers` section into your worker YAML and select its entry name through
`driver_model` or `modality_assist_model`, as described in
[Configuration](../getting-started/configuration.md). These are fragments,
not complete worker files.

The examples include every provider-entry, endpoint, authentication, and
retry field understood by the worker. Optional alternatives are commented
out so mutually exclusive settings are not sent together. Generation
`config` is an open JSON object: additional request fields are forwarded
subject to the plugin's transformations and validation. There is no finite
plugin-defined list of all passthrough fields, and forwarding a field does
not guarantee that the remote model supports it.

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
