# Qwen

**Last updated: 2026-10-04.** The `qwen` plugin uses the Qianwen AI
platform's OpenAI-compatible endpoint for text and static image input, text
output and tool calls. [Qianwen quickstart](https://platform.qianwenai.com/docs/developer-guides/getting-started/introduction) · [Chat API reference](https://platform.qianwenai.com/docs/api-reference/chat/openai-chat)

## Worker configuration

Add this provider to the worker YAML, select it with `driver_model: primary`,
and export `DASHSCOPE_API_KEY` in the worker environment. The example splits
the platform's SDK base URL and path into the two fields Simplex joins.

```yaml
providers:
  primary:
    plugin: qwen
    model: qwen3.8-flash
    endpoint:
      base_url: https://maas.qianwenaiapi.com
      request_path: /compatible-mode/v1/chat/completions
      auth:
        scheme: bearer
        api_key: ${DASHSCOPE_API_KEY}
    config:
      enable_thinking: true
      reasoning_effort: xhigh
      preserve_thinking: false
```

## Parameters

| Field | Meaning |
| --- | --- |
| `model` | The plugin applies Qwen 3.8 defaults to its two advertised families and their hyphen-suffixed snapshots. Other startup IDs use the supplied generation settings. |
| `endpoint` | The example uses the plugin defaults. With a base URL ending in `/compatible-mode/v1`, set `request_path: /chat/completions` so the prefix occurs once. |
| `config.enable_thinking` | YAML Boolean. Defaults to `true` for Qwen 3.8 Flash and Max. |
| `config.reasoning_effort` | `low`, `medium` or `xhigh` for these families. Simplex supplies `xhigh` when both effort and budget are omitted. It maps startup `high` and `max` to `xhigh`. Top-level effort takes precedence over `reasoning.effort`. |
| `config.thinking_budget` | Alternative to effort. The plugin accepts integers from 0 through 262144 for these families. Set this instead of an effort when you need an explicit reasoning ceiling, and omit both effort spellings. |
| `config.preserve_thinking` | Controls whether prior assistant reasoning is sent back. Defaults to `false` for Flash and `true` for Max. Replayed reasoning contributes to input usage. |
| `config.tool_stream` / `config.tool_choice` | `tool_stream` is an optional Boolean for streamed tool arguments. The plugin rejects `tool_choice: required`. |
| `config.modalities` | If set, use `[text]` for output. Static images travel in user message content. |

Place these fields directly under `config`. The plugin rejects the Python SDK's
`extra_body` wrapper and DeepSeek's `thinking` object. It removes `n` and
requests streaming usage. [Qwen thinking controls](https://platform.qianwenai.com/docs/developer-guides/text-generation/thinking) · [Shared provider fields](../getting-started/configuration.md#provider-fields)

## Runtime options

| Panel option | Choices advertised by Simplex |
| --- | --- |
| `model` | `qwen3.8-flash`, `qwen3.8-max` |
| `enable_thinking` | `enabled`, `disabled` |
| `reasoning_effort` | `low`, `medium`, `xhigh` |

An explicit effort selection replaces a configured thinking budget for subsequent
driver requests. When a budget is configured, the plugin reports its band as
`low` (through 4096), `medium` (through 16384), or `xhigh` (above 16384).
The published menu describes the bundled adapter. The platform
catalog can also list newer model families.

## Usage notes

- For image assistance, include `modality_assist` in `tools.intrinsic` and select a vision-capable entry with `modality_assist_model`. The tool sends the images in a separate exchange and returns text to the driver.
- The plugin has offline request, response and image-tool coverage. Model access and service-side limits depend on the endpoint and account. [Qwen model selection](https://platform.qianwenai.com/docs/developer-guides/getting-started/model-selection)
