# Configure your first worker

## Hub-managed sessions

Install the Hub and a worker, set `MODEL_API_KEY` in the worker's environment,
and start the Hub. Local workers inherit the Hub's environment at launch.
[Deployment guide](../deployment/hub.md)

In **Configurations**, create a **Worker configuration** from the template.
Replace its `providers` and `driver_model` sections with the fragment below.
Keep the Hub connection and persistence placeholders elsewhere in the template.

```yaml
providers:
  primary:
    plugin: deepseek
    model: deepseek-flash
    endpoint:
      auth: {scheme: bearer, api_key: "${MODEL_API_KEY}"}
driver_model: primary
```

Choose **Validate** and **Save**. Create a session with the `local` launch
profile and the saved worker file. Select **Start**, wait until the worker is
ready, and send a short message to verify model access.

Here `primary` names a configuration and `deepseek` selects the installed plugin.
Choose a model available to your account. Saving this file changes the library;
a session with a bound allocation continues to use its own snapshot.

<a id="paths-and-defaults"></a>

## Common changes

| Task | Reference |
| --- | --- |
| <a id="provider-fields"></a><a id="credentials-and-optional-components"></a>Switch providers or add image assistance | [Model roles and credentials](../reference/worker-settings.md#provider-fields) |
| <a id="system-prompts"></a><a id="custom-prompt-files"></a><a id="startup-and-restored-sessions"></a>Change the system prompt or workspace | [Prompts and environment](../reference/worker-settings.md#system-prompts) |
| <a id="intrinsic-toolset-selection"></a>Enable tools or approvals | [Tools and approvals](../reference/worker-settings.md#tools-and-approvals) |
| <a id="empty-model-responses"></a><a id="optional-automatic-compaction"></a><a id="controlled-research-budgets"></a>Configure compaction or research budgets | [Loop and compaction](../reference/worker-settings.md#loop-and-compaction) · [Research budgets](../reference/worker-settings.md#controlled-research-budgets) |
| <a id="persistence-backend"></a>Choose storage or retention | [Persistence](../reference/worker-settings.md#persistence) |
| Use a Docker worker | [Docker launch profile](../reference/launch-profiles.md#docker-target) |
| Change the Hub listener or panel appearance | [Hub settings](../reference/hub-settings.md) |

Find the file location, examples and effective scope for each setting in the
[configuration library](../hub/configurations.md).
