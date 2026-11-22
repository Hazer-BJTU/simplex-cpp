# Security

The Hub controls worker launches and operator decisions. Each worker runs tools
with its operating-system account and sends model requests to its configured
providers. Set limits at the account, container and network boundaries before
giving an agent access to files or credentials.

## Hub access and credentials

| Credential | Who uses it | What it grants |
| --- | --- | --- |
| `panel.token` | Operators and panel clients | Access to protected Hub APIs, including configuration editing, worker supervision and approvals. Treat panel access as administrative access to the Hub host. |
| Per-session worker token | Worker and Hub | Access to the session's event, confirmation and enabled remote-tool routes. The Hub also checks worker and run identity for operations that require a live run. |
| Provider API key | Worker on its execution target | Access to the selected model service under the provider account. Set it in the worker environment, including inside a Docker target. |

The Hub's main listener defaults to loopback. Binding it to a non-loopback
address requires a panel token. On shared hosts or with Docker host networking,
other processes can reach loopback too. Set a panel token there. Use TLS
termination for connections across untrusted networks, restrict access to both
Hub listeners, and redact token-bearing URLs from proxy logs. [Hub settings](reference/hub-settings.md)

## Worker permissions and cancellation

Process, file-editing and PTC tools can affect files and processes accessible
to the worker account. `worker.environment.workspace` tells the model where to
work and supplies PTC's working directory. Enforce filesystem access with OS
permissions and container mounts.

Run workers under an unprivileged account, mount only the data they need, and
keep the host Docker socket and Hub credentials outside the container. The Hub
invokes `simplex` inside an operator-managed container through `docker exec`.
Docker CLI credentials belong on the Hub host. [Launch profiles](reference/launch-profiles.md#paths-and-ownership)

Cancellation interrupts model waits and allows dispatched tools to settle.
Files already written, requests already sent and child processes may outlive a
cancelled run. If a tool's outcome is uncertain, inspect its effects before
repeating the action. [Worker recovery](core/worker-protocol.md#cancellation-shutdown-and-recovery)

## Tool approvals and delegation

| Tool classification | Worker behavior |
| --- | --- |
| `trusted` | Executes without an approval prompt. |
| `require_confirm` | In `ask` mode, requests an operator decision. A missing endpoint, timeout or mismatched reply denies the call. `approve` and `deny` decide locally. |
| `default_deny` | Refuses the call. |

A client allowed to submit run payloads can select automatic approval for
confirmation-required calls. Grant input access only to principals trusted to
make that decision. The panel can show a truncated argument preview for a
large call. Approval applies to the entire invocation. Deny the request if the
preview leaves its effects unclear.

The packaged `run_command` policy uses strict mode.
Enabling command bypass rules allows matching commands to run as trusted;
the worker account's permissions still determine their possible effects.
PTC approval covers the submitted program. [Plugin configuration](plugins/configuration.md)

A headless worker starts with its own `ask` policy. Operators can change it to
`approve` or `deny` for future prompts. A parent can delegate to its direct
children but cannot change their approval policy. A clean fork has independent
session storage and can still access a shared workspace. Loss of the worker
socket leaves process ownership unresolved; independently verify termination
before authorizing recovery cleanup. [Headless subagents](hub/subagents.md#safety-approvals)

## Native plugins

Native providers, toolsets and hooks run in the worker process. A library can
execute initialization code as soon as it is loaded, before its descriptor and
ABI fingerprint are checked. Only scan directories controlled by trusted
publishers or administrators. Use modules built for the worker's toolchain and
runtime. [Plugin roles](plugins/overview.md)

## Model traffic and stored data

The driver can send conversation history and tool results to its provider. The
optional image-assistance tool sends selected local images to the auxiliary
model endpoint. Choose both destinations with the data's sensitivity in mind.

Restrict access to the Hub's saved configurations, transcripts, logs and plans,
and to the target's SQLite or JSON state, compaction archives, summary memory
and backups. The panel shows bounded previews; stored records can contain the
full text. Managed inspection can return complete decoded state over stdout.
Keep that output and credentials out of shared logs. [Worker storage](architecture/persistence-backends.md)

## Report a vulnerability

Report security issues privately through GitHub. Include the affected version,
deployment and impact, and redact credentials and private conversation data.
[Private vulnerability reporting](https://github.com/Hazer-BJTU/simplex-cpp/security/advisories/new)
