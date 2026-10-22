# Security boundaries

## Native plugins

Plugins run inside the worker with its permissions. Shared-library initialization
can execute before a descriptor or compatibility fingerprint is checked. ABI
admission prevents certain incompatible modules from being used; it does not
establish provenance, audit behavior, or sandbox code.

Trust every configured plugin search directory and its contents. Provider
discovery scans all compatible providers; tool/hook enable lists control
construction and do not make an untrusted library directory safe to scan.
Rebuild host and plugins together rather than bypassing compatibility checks.

## Local operations

Process and file tools can modify everything accessible to the worker account.
The workspace setting is a prompt hint, not an access restriction. Tool approval
is an operator decision, not an OS isolation boundary. Cancellation cannot undo
already completed operations.

Use a dedicated unprivileged account or a container with limited mounts,
credentials, and network access. For a managed Docker target the operator owns
the container and its user, mounts and network policy; the Hub only runs
`docker exec` inside it. Never mount the Docker socket or unrelated host data into
an untrusted worker.

## Hub authority

Panel access includes configuration editing and worker launch commands. Treat
it as administrative access to the Hub host. A payload can select automatic
approval for confirmation-required tools, so permission to send input also
grants approval-policy authority.

The default listener is loopback. A non-loopback listener requires a panel
token. Use TLS termination and controlled network access for remote deployment.
Worker routes use per-session bearer tokens, separate from the panel token.
Tokens in URLs and configuration files must be protected from logs and sharing.

A Docker management target stores its Hub-side Docker CLI environment in the
launch configuration (`management.env`) and in private session snapshots. Proxy,
registry or context credentials placed there remain sensitive even though the
files are mode 0600. The Hub does not copy its own environment into the
container, does not inspect container environment variables, and never reads the
container's filesystem. Worker credentials belong to the container's environment,
which the operator controls. See
[Docker worker deployment](../deployment/docker-worker.md#security).

## Data and model endpoints

Conversation text and tool results can be sent to the configured provider.
JSON snapshots and compact archives contain the complete conversation state,
including tool results, and legacy Markdown archives retain what they exported.
Limit filesystem access and retention accordingly. External references are
untrusted input; rendering a URL is not permission to fetch or execute it.

## Headless delegation

The Hub supports clean-fork/send/receive remote routes for directly owned headless
workers. Each child uses a flat `<dataDir>/subagents/<generated-id>/` root, an
independent operator-controlled ask/deny/approve policy, and a bounded primary
conversation projection. It retains no event transcript or reasoning/tool history.
Parent process shutdown/crash cascades; socket disconnect alone preserves the
family. Confirmed child shutdown deletes its persistence. Clean-fork shares any
explicit external workspace and is not a sandbox. The optional C++ `hub_remote_call`
toolset exposes trusted fork/send/receive operations without extra parent approval.
Child tool approvals remain governed by the independent operator-owned policy;
parent calls cannot override it through payload options.
Unknown process ownership never authorizes automatic deletion. The separate operator
recovery API can retry cleanup or explicitly attest independently verified termination;
the latter can delete child persistence and is unavailable to worker remote tools.
See the [complete subagent contract](../hub/subagents.md) for configuration snapshots,
launch support, request deduplication, recovery and cleanup limits.
