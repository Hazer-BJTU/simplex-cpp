# Configuration library

Choose the configuration by **where the change should take effect**. One Hub
serves multiple sessions. Each managed session selects one launch profile and
one worker YAML, and keeps its own copies.

| I want to change… | Where to edit | When it takes effect |
| --- | --- | --- |
| Panel port, access token, appearance or Hub resource limits | [Hub settings](../reference/hub-settings.md) in `hub.config.jsonc` | Restart the Hub. |
| <a id="launch-configuration"></a><a id="docker-management-template"></a>Target, container name, executable, threads or reachable Hub address | [Launch profiles](../reference/launch-profiles.md) in `configs/launch/<name>.jsonc` | Start a session with that saved profile. |
| <a id="worker-templates-and-managed-fields"></a>Model, prompt, tools, loop budgets or storage | [Worker settings](../reference/worker-settings.md) in `configs/worker/<name>.yaml` | The worker loads its session configuration at startup. |
| Model or approval choices offered by a running worker | Panel payload options | On admission of the next run-starting payload. [Worker protocol](../core/worker-protocol.md#operations) |

## Editing the library

In **Configurations**, choose Worker (YAML) or Launch (JSONC), enter a new
file name and select **New from template**. The Hub creates the file in its
library. Select a saved file to download, validate or delete it. To copy or
rename it, enter the destination name and choose **Save copy** or **Rename**.
Library IDs contain 1–128 ASCII letters, digits, underscores or hyphens.

- **Open in editor** launches the [configured editor on the Hub host](../reference/hub-settings.md#configuration-editor).
  Save in the editor. **Copy Hub path** identifies the library file for terminal editing.
- **Download** a configuration, edit it locally, then choose **Replace from file**.
  Replacement checks the selected revision. On conflict, download the current
  file and reconcile the changes before replacing it.
- **Upload new file** imports a UTF-8 file under its base name (up to 1 MiB).
  Uploads are validated before saving. **Validate** checks the current saved
  file after external edits. Worker startup checks installed plugins and local resources.

## Directory and startup configuration

The Hub startup file and `configs/` library live under the Hub's data directory
(default `~/.simplex/hub`). Session selections are saved under
`sessions/<id>/config/`. The worker's live configuration and durable state
belong to its target allocation. Keep the Hub's records and the target storage
available for the lifetime of the session.

## Session lifecycle

Select both saved files when creating the session. Library edits are available
to future selections. A stopped, disconnected session can apply a new pair
before it has a managed allocation. Once bound, restarting reuses its captured
configuration. Create a new session to use a different configuration pair.
<a id="headless-delegation"></a> A clean-fork child starts with the captured startup policy and independent
storage. [Subagent lifecycle](subagents.md#persistence-and-lifetime)

## Configuration API

An independent panel can list files, read templates, preview endpoints, validate
source and save configurations through `/api/configurations`. A create uses
`revision: null`; editing uses the revision returned by the last read. On a
conflict, reload and reconcile the saved source. Use a selection such as
`{"launchConfig":"local","workerConfig":"my-worker"}` in the session's `spec`.
[Panel API and SDK](ui-sdk.md)
