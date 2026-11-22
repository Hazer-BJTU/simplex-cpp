# GLM (Zhipu AI)

**Last updated: 2026-10-30.** The `glm` plugin targets Zhipu's BigModel Chat
Completions API. The GLM-5.3 Flash integration supports image input, streamed
text and tool calls. [BigModel Chat Completions reference](https://docs.bigmodel.cn/api-reference/模型-api/对话补全)

## Worker configuration

Add this entry to the worker YAML and select it with `driver_model: primary`.
Export your BigModel API key as `ZHIPU_API_KEY` in the worker environment.

```yaml
providers:
  primary:
    plugin: glm
    model: glm-5.3-flash
    endpoint:
      base_url: https://open.bigmodel.cn/api/paas/v4
      request_path: /chat/completions
      auth:
        scheme: bearer
        api_key: ${ZHIPU_API_KEY}
    config:
      thinking: {type: enabled, clear_thinking: false}
      reasoning_effort: high
```

## Parameters

| Field | Meaning |
| --- | --- |
| `model` | BigModel model ID. GLM-5.3 family members always use thinking mode. |
| `endpoint.base_url` | The example uses the standard API. Zhipu reserves `https://open.bigmodel.cn/api/coding/paas/v4` for authorized Coding Plan clients. Confirm eligibility before selecting that endpoint. [Coding Plan requirements](https://docs.bigmodel.cn/cn/coding-plan/quick-start) |
| `config.thinking.type` | Use `enabled` for GLM-5.3. The plugin rejects `disabled` for this family. |
| `config.thinking.clear_thinking` | Set `false` to request preserved thinking between turns. The adapter replays assistant `reasoning_content` during tool exchanges. |
| `config.reasoning_effort` | For GLM-5.3 use `low`, `high` or `max`. The plugin translates `medium` to `high` and `xhigh` to `max` for this family. Top-level effort wins over `reasoning.effort`. |
| `config.tool_stream` / `config.tool_choice` | Set `tool_stream: true` for incremental function arguments. If supplying `tool_choice`, use `auto`. |
| Other generation fields | The model can accept native options such as `max_tokens` or `response_format`. The plugin removes `n`, `frequency_penalty`, `presence_penalty` and `thinking_budget`. |

The adapter requests streaming usage and reads cached input tokens from
`prompt_tokens_details.cached_tokens`. [Shared provider fields](../reference/worker-settings.md#provider-fields)

## Runtime options

| Panel option | Choices advertised by Simplex |
| --- | --- |
| `model` | `glm-5.3-flash`, `glm-5.3-flashx`, `glm-5.3`, `glm-5.2`, `glm-4.7` |
| `reasoning_effort` | `low`, `high`, `max` |

The menu describes local driver options. The account model catalog and access
rights come from the service.

## Usage notes

- For local image descriptions, select the `modality_assist` toolset and set `modality_assist_model: primary` when that model accepts images.
- For diagnostics, the plugin maps the service finish reasons `sensitive` and `model_context_window_exceeded` to content filtering and length limits respectively.
- The October 30 adaptation was checked against the standard pay-as-you-go endpoint, including streaming tool calls, usage and reasoning replay. An HTTP 429 can also represent an account quota or balance issue. Check the service error before increasing `retry.max_attempts`.
