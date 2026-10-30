# GLM (Zhipu AI)

**Last updated: 2026-10-30.** This page describes the bundled implementation,
not a remotely queried model catalog. Check your account's available models
before deployment. Wire behavior below was verified against
`https://open.bigmodel.cn/api/paas/v4` on that date with a pay-as-you-go key.

## Implementation

The `glm` plugin uses the shared Chat Completions adapter. Its dialect supplies
`https://open.bigmodel.cn/api/paas/v4` with `/chat/completions`, bearer
authentication, the always-on thinking policy of the GLM-5.3 family, the effort
translation those models require, and the platform's extra `finish_reason`
values. Provider execution and retries remain inside the model layer.

The plugin advertises these local runtime options:

| Name | Choices |
| --- | --- |
| `model` | `glm-5.3-flash`, `glm-5.3-flashx`, `glm-5.3`, `glm-5.2`, `glm-4.7` |
| `reasoning_effort` | `low`, `high`, `max` |

These are the choices returned by `get_options()`, not a statement that every
account or compatible third-party endpoint accepts them. Startup `model` and
provider-specific `config` are distinct from the validated runtime option patch;
any model identifier your account offers can be configured even when it is not
in the advertised list.

`glm-5.3-flash` is the documented default: a native-multimodal model with a 1M
token context and up to 128K output tokens. It is a reasonable
`modality_assist_model` choice as well, because image input is accepted on the
same endpoint.

## Worker configuration

Copy this provider section into your worker YAML. Shared endpoint,
authentication, and retry semantics are described in
[Support policy](index.md#shared-fields-and-request-ownership).

```yaml
providers:
  glm:
    plugin: glm
    endpoint:
      base_url: https://open.bigmodel.cn/api/paas/v4
      request_path: /chat/completions
      auth:
        scheme: bearer
        api_key: ${ZHIPU_API_KEY}
        header_name: x-api-key  # Used only with scheme: custom_header.
        extras: {}             # Metadata; not sent by this plugin.
      user_agent: simplex-cpp/glm
      extra_headers: {}
      extras: {}               # Metadata; not generation parameters.
    model: glm-5.3-flash       # Also advertised: glm-5.3-flashx, glm-5.3,
                               # glm-5.2, glm-4.7.
    config:
      reasoning_effort: high   # Public runtime choices: low, high, max.
      thinking:
        type: enabled          # The GLM-5.3 family cannot disable thinking.
        clear_thinking: false  # Opt into preserved thinking (see below).
      # tool_stream: true      # Opt in: stream function-call arguments.
      # Optional passthrough examples; values are not plugin defaults:
      # max_tokens: 8192
      # temperature: 1.0
      # top_p: 0.95
      # stop: ["END"]
      # response_format: {type: json_object}
      # tool_choice: auto
    retry:
      max_attempts: 3
      initial_backoff_ms: 500
      max_backoff_ms: 120000
```

`ZHIPU_API_KEY` is this page's convention, not a requirement: any environment
variable name works, and `ZAI_API_KEY` or `ZHIPUAI_API_KEY` are equally common
spellings. Export it in the environment that actually starts the worker; the
loader expands credentials only for selected model configurations.

### GLM Coding Plan

A Coding Plan subscription uses a dedicated endpoint. Override only `base_url`;
`request_path` and the model catalogue path follow it.

```yaml
    endpoint:
      base_url: https://open.bigmodel.cn/api/coding/paas/v4
```

The plan documents a different effort mapping than the standard API (it folds
`none`/`minimal`/`low` to `low`). The plugin applies its own validation before
the request leaves the process, so a Coding Plan account configured with
`reasoning_effort: max` or `high` behaves identically on either endpoint.

| Control | Plugin behavior |
| --- | --- |
| `thinking.type` | Forwarded unchanged, except that `disabled` is refused locally on the GLM-5.3 family and on `glm-4.7`, which the platform documents as always thinking. The service answers that request with HTTP 400 and code `1210`; refusing it here names the field instead of spending a round trip. |
| `thinking.clear_thinking` | Forwarded unchanged. `false` opts into preserved thinking: the worker already replays intermediate `reasoning_content`, which that mode requires. The service default is `true`, which discards historical reasoning between turns. |
| `reasoning_effort` | `low`, `high`, and `max` are the documented values for the GLM-5.3 family. `medium` is translated to `high` and `xhigh` to `max`, matching the aliases the family's predecessors document. `none`, `minimal`, and unknown values are refused, because the model cannot stop thinking and silently landing on `low` would be a different request. |
| `reasoning.effort` | Shared compatibility spelling. Converted to `reasoning_effort`; an explicit top-level value wins. The `reasoning` envelope is removed. |
| `tool_stream` | Opt-in, default off. When enabled, the service streams function-call arguments incrementally; the plugin assembles them exactly as it does OpenAI fragments. |
| `tool_choice` | `auto` only; any other value is refused locally. |
| `n`, `frequency_penalty`, `presence_penalty`, `thinking_budget` | Removed before transmission. `n` is not part of this platform's request, and the other three are accepted but ignored, so a stale configuration cannot look like a working control. |
| `stream_options` | Kept. The service accepts it and answers the final chunk with `usage`, which the shared reader turns into cost accounting. |

Assistant messages are replayed with their `reasoning_content`, which the
platform requires for interleaved thinking with tools. The responses' own
`reasoning` envelope stays internal to the worker.

Two `finish_reason` values are translated for the shared reader: `sensitive`
(moderation) becomes `content_filter` and `model_context_window_exceeded`
becomes `length`. Without the mapping both would be reported as a generic
failure. `network_error` is left alone and fails, as a model-side fault should.

Sampling, output limits, structured output, and other passthrough parameters are
validated by the remote service. Consult the
[GLM chat-completions reference](https://docs.bigmodel.cn/api-reference/%E6%A8%A1%E5%9E%8B-api/%E5%AF%B9%E8%AF%9D%E8%A1%A5%E5%85%A8)
for current server-side constraints.

Select this configuration with `driver_model: glm`; optionally set
`modality_assist_model: glm` for a separate image-assistance instance. For a
full worker file, see the
[minimal configuration](../getting-started/configuration.md#hub-managed-sessions),
which names its provider entry `primary` instead.

## Use from the Hub

Save the worker YAML in the configuration library. Select it together with a
local or Docker launch file when creating a session. The Hub fills worker
connection endpoints but preserves provider configuration. For an existing
session, stop it, apply the saved configuration pair, and restart it.

The panel obtains advertised model choices through the worker's `options`
signal. Selected changes are attached to the next run-starting payload; they do
not mutate an active run. Runtime selections are not persisted as startup
configuration in AgentInputState.

## Optional image assistance

```yaml
modality_assist_model: glm
```

This creates a **separate** model instance even when both roles select `glm`.
It enables an intrinsic tool that reads local image files, encodes them as a
base64 data URL, and performs one isolated model exchange to return a textual
description.

Image content travels as an `image_url` part whose `url` is either a public URL
or that data URL. The platform documents a 5 MB and 6000×6000 limit per image,
`jpg`/`png`/`jpeg` encodings, and up to 50 images for the GLM-5.3-Flash family.
Every supplied image path must succeed or the call fails.

## Live checks

What this page's verification covered on 2026-10-30, for a reader repeating it:

1. `provider_info()` returned the account's model catalogue from
   `/api/paas/v4/models` with the same bearer credential.
2. A streamed exchange with one registered tool produced a complete
   `tool_calls` sequence, a correlated tool result round trip, and a `usage`
   block with `prompt_tokens`, `completion_tokens` and
   `prompt_tokens_details.cached_tokens`.
3. `thinking.type: disabled` and `reasoning_effort: medium` on
   `glm-5.3-flash` both answer HTTP 400 with code `1210` — the requests the
   dialect now refuses locally.
4. `frequency_penalty`, `presence_penalty` and `thinking_budget` are accepted
   and ignored.
5. A history containing `reasoning_content` beside a tool result is accepted
   with `thinking.clear_thinking: false`.

`provider_info()` queries the platform catalogue; unlike the DeepSeek dialect
this one attaches no account-balance companion, because the platform documents
no equivalent endpoint.

## Retry behavior

The shared retry policy treats every 429 as recoverable. Zhipu's 429 family is
mixed: `1302` (rate limit) and `1305` (overloaded) are worth retrying, while
`1113` (insufficient balance) and the `1308`/`1310`/`1316`–`1321` quota and plan
errors are not — the same request will draw the same answer. A retry budget of
`0` or `1` avoids spending a few seconds of backoff on an account-level refusal;
set it in `retry.max_attempts` for a strict deployment.
