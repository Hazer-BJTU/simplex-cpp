# DeepSeek

For complete provider-only YAML examples and field descriptions, see the
[provider configuration tutorial](../tutorials/provider-configuration.md).


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

Use the complete [DeepSeek worker example](../getting-started/configuration.md#hub-managed-sessions)
in the configuration guide. It selects `plugin: deepseek`, `model: deepseek-flash`,
and `reasoning_effort: high`, with the credential read from `${MODEL_API_KEY}`.
Export that variable in the environment that actually starts the worker.
The loader expands credentials only for selected model configurations.

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
modality_assist_model: primary
```

This creates a **separate** model instance even when both roles select `primary`.
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
