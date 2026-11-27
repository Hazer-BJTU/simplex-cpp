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

The **Configurations** dialog lists Worker YAML and Launch JSONC files in
separate groups.

- Choose **+ Add → New from template** in the target group, enter a name and
  choose **Create**. Launch files offer Local, Docker and Managed templates.
  **+ Add → Upload file** imports a UTF-8 file under its base name (up to 1 MiB).
  Library names accept 1–128 ASCII letters, digits, underscores or hyphens.
- Choose **Open** beside a filename to start the [configured editor on the Hub host](../reference/hub-settings.md#configuration-editor).
  With no host editor configured, the row offers **Download**. Download is
  also available in the actions menu when Open is the primary action.
- To upload an edited copy, choose **⋯ → Replace from file** on the file’s
  row. Choose the local file and confirm with **Replace**. The panel remembers
  the revision you downloaded, even if you rename the file. If another client
  changes it, replacement and deletion report a conflict. Download the current
  version to review it or reconcile your edits before retrying.
- The same menu offers **Validate**, **Copy Hub path**, **Create copy**,
  **Rename** and **Delete**. A launch file also offers **Preview Hub endpoints**.
  Validation checks the saved source; worker startup checks plugins and local resources.

Expand **Session configuration** to choose and explicitly apply a saved pair
to the displayed session. Browsing or editing library files leaves its
selection intact.

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
