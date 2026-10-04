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
