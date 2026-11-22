# DeepSeek

**Last updated: 2026-10-01.** The `deepseek` plugin uses DeepSeek's Chat
Completions API for streamed answers, image inputs and tool calls.
[DeepSeek Chat Completions reference](https://api-docs.deepseek.com/api/create-chat-completion/)

## Worker configuration

Replace the `providers` section in the worker YAML with this example, or add
`primary` to an existing mapping. Select it with `driver_model: primary`.
Export `DEEPSEEK_API_KEY` in the worker environment.

```yaml
providers:
  primary:
    plugin: deepseek
    model: deepseek-flash
    endpoint:
      base_url: https://api.deepseek.com
      request_path: /chat/completions
      auth:
        scheme: bearer
        api_key: ${DEEPSEEK_API_KEY}
    config:
      thinking: {type: enabled}
      reasoning_effort: high
```

## Parameters

| Field | Meaning |
| --- | --- |
| `model` | DeepSeek model ID available to your account. The endpoint and path above are the plugin defaults. |
| `config.thinking.type` | `enabled` or `disabled`. An explicit native thinking object controls the mode. To turn thinking off, set `disabled` and omit the effort. |
| `config.reasoning_effort` | Controls reasoning depth. The plugin enables thinking by default and leaves the effort unspecified when both controls are absent. |
| `config.reasoning.effort` | Simplex also accepts this nested spelling. An explicit top-level `reasoning_effort` wins. |
| Other generation fields | Set native fields such as `max_tokens` or `response_format` under `config`. Service support depends on the model and thinking mode. The plugin removes `n`, `frequency_penalty` and `presence_penalty`. |

For custom headers, authentication alternatives, and retry limits, see
[shared provider fields](../reference/worker-settings.md#provider-fields).

## Runtime options

| Panel option | Choices advertised by Simplex |
| --- | --- |
| `model` | `deepseek-flash`, `deepseek-v4-pro` |
| `reasoning_effort` | `low`, `high`, `max` |

The panel applies selected options to the driver on the next run-starting
payload. Startup YAML can name other models offered by the configured service.

## Usage notes

- Without an explicit `thinking` object, startup efforts `none` and `minimal` disable thinking and are removed from the outgoing request. Use `thinking.type` to express the mode directly.
- The adapter streams one choice, replays assistant reasoning during tool exchanges, and reads cache hits from `prompt_cache_hit_tokens`.
- For local images, select `modality_assist` in `tools.intrinsic` and set `modality_assist_model: primary` with a vision-capable model. The worker creates a separate instance for the image tool. [DeepSeek vision guide](https://api-docs.deepseek.com/guides/vision)
