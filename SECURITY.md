# Security policy

## Trust boundaries

Simplex workers perform local operations with the permissions of their process:

- Native model, tool and loop-hook plugins run inside the worker. Load only
  trusted code from trusted directories; compatibility checks are not a sandbox.
- Local tools can execute shell commands and read or modify accessible files.
  Workspace hints and tool approval do not restrict OS access.
- Cancellation does not undo completed effects or guarantee that an in-progress
  external operation has stopped.
- Hub panel access includes configuration editing and worker launch commands.
  Worker payloads can change confirmation policy, including automatic approval;
  permission to send input therefore also grants tool-approval authority.

Use restricted accounts or containers, limit mounted data and credentials, and
protect Hub access and session tokens. Conversation snapshots, readable memory
archives, logs and model requests may contain sensitive data.

See the full [security boundaries](https://hazer-bjtu.github.io/simplex-cpp/architecture/security.html)
for plugin loading, local operations, Hub authority and remote deployment.

## Reporting a vulnerability

Do not open a public issue or pull request with details of an unpatched
vulnerability. Use [GitHub private vulnerability reporting](https://github.com/Hazer-BJTU/simplex-cpp/security/advisories/new)
to send a report to the repository maintainers.

Include the affected worker/Hub version or source commit, deployment and plugin
configuration, reproduction steps, expected security boundary, and potential
impact. Remove API keys, bearer tokens, private conversation data and unrelated
personal information. Share a minimal reproduction rather than live credentials.

Use public bug reports for ordinary functional defects that do not expose a
security vulnerability.
