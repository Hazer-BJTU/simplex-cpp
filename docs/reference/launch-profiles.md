# Launch profiles

Select a launch JSONC file in **Configurations** to tell the Hub where to run
`simplex manage` and `simplex run`. The Hub seeds `configs/launch/local.jsonc`
and `docker.jsonc` on first use. Select the saved file when creating a session.

## Local worker

Use the seeded `local` profile for a worker on the Hub host. This complete
example uses explicit directories. Set `cwd` to an existing directory and
choose a writable allocation root.

```json
{
  "management": {
    "target": {"kind": "local"},
    "executable": "simplex",
    "cwd": "/tmp",
    "env": {},
    "root": "/srv/simplex/allocations"
  },
  "worker": {"threads": 2}
}
```

## Paths and ownership

| Management field | Required value or default | Location and effect |
| --- | --- | --- |
| `target.kind` | Required: `local`, `docker`, or `ssh` | Selects the execution transport for both commands. |
| `executable` | Required; templates use `simplex` | Name on the **target** PATH or absolute path to the installed router. |
| `root` | Required absolute target directory | Each session gets an allocation beneath it. The seeded local profile uses `<dataDir>-workers`; local roots and Hub data must be disjoint. |
| `cwd` | Required absolute Hub directory | Working directory of the local process, Docker CLI or SSH client. The templates use the Hub user's home. |
| `env` | Required object; usually `{}` | String overrides for the Hub-side invocation. Optional profile-level `env` entries override matching keys. For Docker and SSH, provision model credentials in the target environment. |
| `timeoutMs` | 30000 ms | Deadline for management requests. |

The Hub saves the chosen target and allocation identity. Use a new session to
switch a bound target. Inspect saved state through the target-side manager.
[Management commands](cli.md#manage-worker-storage)

## Docker target

Start with the seeded Docker profile. For the Linux host-network deployment,
the following fragment replaces its `worker.connectHost` field:

```json
{"worker": {"connectHost": "127.0.0.1"}}
```

Keep `management.target.kind: "docker"`. The required `target.command`,
`target.args`, and `target.container` in the template are `"docker"`, `[]` and
`"simplex-worker"`. The args are global Docker CLI options. The template uses
`/var/lib/simplex/allocations` **inside the retained container** and advertises
`172.17.0.1` for bridge networking. Choose an address reachable from your
container. For SSH, the corresponding required transport fields are `command`,
`args` and `host`. [Docker deployment example](../deployment/hub.md#run-a-docker-worker)

Docker `management.target.preflight` defaults to `enabled: true`,
`timeoutMs: 5000`, `container: true`, `executable: true` and
`endpoint: {"enabled":true,"channels":["events"]}`. These checks inspect container
status, the installed command and the advertised address. Endpoint channels can
include `events`, `confirm` and `tools`. A successful check of the address still
requires a working network route.

## Worker and network settings

| Profile field | Default | Use |
| --- | --- | --- |
| `worker.threads`, `worker.args` | 1, `[]` | Execution threads and extra argument strings. The manager supplies the config path. |
| `worker.connectHost` | Empty | Advertised hostname/IP. Empty derives it from the listener, using loopback for a wildcard bind. |
| `endpoints.events`, `confirm`, `tools` | Derived | Per-channel WebSocket origins with optional proxy path prefixes. Hub appends the session route and token. |
| `worker.stopTimeoutMs` | 15000 ms | Wait for graceful worker shutdown. |
| `worker.sigtermGraceMs`, `sigkillGraceMs` | 5000, 2000 ms | Local supervisor grace periods during escalation. Remote ownership must be confirmed separately. |

Use **Validate** and the endpoint preview in **Configurations**, then start an
ordinary session to verify target access. Keep the retained container available
through worker stop and allocation retirement.
