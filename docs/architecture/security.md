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
credentials, and network access. The supplied Hub Docker launch template runs
as container root for convenience; review it before using it for sensitive work.
Never mount the Docker socket or unrelated host data into an untrusted worker.

## Hub authority

Panel access includes configuration editing and worker launch commands. Treat
it as administrative access to the Hub host. A payload can select automatic
approval for confirmation-required tools, so permission to send input also
grants approval-policy authority.

The default listener is loopback. A non-loopback listener requires a panel
token. Use TLS termination and controlled network access for remote deployment.
Worker routes use per-session bearer tokens, separate from the panel token.
Tokens in URLs and configuration files must be protected from logs and sharing.

## Data and model endpoints

Conversation text and tool results can be sent to the configured provider.
Readable archives and JSON snapshots may contain sensitive task data. Limit
filesystem access and retention accordingly. External references are untrusted
input; rendering a URL is not permission to fetch or execute it.

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
See the [complete subagent contract](../hub/subagents.md) for configuration snapshots,
launch support, request deduplication, recovery and cleanup limits.
