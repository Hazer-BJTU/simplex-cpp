# One-to-one terminal shell

simplex_shell --listen 127.0.0.1:8765 listens for one worker. Configure routes
with --events-path /agent/events and --confirmation-path /agent/confirm.
Unknown paths return HTTP 404; a second active event connection gets 409.

Normal input sends a user message. /continue, /cancel, /status and /shutdown
control the worker. /quit stops the shell. Use /approve ID or /deny ID for a
displayed confirmation UUID. Prompts never block ordinary input or networking.
Each can be answered once and expires on disconnect or the example's 120-second
deadline. The worker may impose a shorter deadline.

The shell binds to the first observed session, retains it across reconnection,
and refuses a different worker incarnation while old confirmations are pending.
It never resubmits messages or approvals. Use /status after reconnect.

Console writes run on one executor in complete labeled blocks. Terminal
controls are escaped; process output retains stdout/stderr labels. This example
has no authentication and supports plain local WebSockets only. See the
[worker guide](../README.md) for configuration, protocol and container testing.
A production authenticated hub is a separate application.
