# Deploy a Docker worker

Run the Hub on the host and workers inside a container that **you** own and keep
running. The Hub never builds an image, creates a container, or starts, stops and
removes one. It only runs the installed `simplex` command inside an existing
container with `docker exec`, so the container's lifetime and the worker's
lifetime are separate decisions.

```
operator: docker run -d --name simplex-worker ...   (once)
Hub:      docker exec simplex-worker simplex manage --request ...   (prepare)
          docker exec simplex-worker simplex run --config ...       (worker)
operator: docker rm -f simplex-worker               (only after sessions are deleted)
```

A launch profile targets that container through
`management.target.kind: "docker"`. See
[Docker deployment](../reference/management.md#remote-targets) for the transport
contract and [the launch profile](#launch-profile) below for the fields.

## Prepare a runtime image

Download and verify a worker release, then copy its complete directory into an
image build context as `worker/`:

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 ca-certificates bash \
    && rm -rf /var/lib/apt/lists/*
COPY worker/ /opt/simplex/
ENV PATH="/opt/simplex/bin:${PATH}"
RUN mkdir -p /root/workspace
WORKDIR /root/workspace
CMD ["sleep", "infinity"]
```

```sh
docker build -t simplex-worker:latest .
```

Add tools such as Git, Python or compilers only if your tasks need them. The
worker distribution does not supply every program a model may request. This
runtime image is for Linux x86_64, matching the release archive.

The image must provide **the same installation for both commands**:

```sh
docker run --rm simplex-worker:latest simplex --version
docker run --rm simplex-worker:latest simplex manage --help
```

`simplex run` starts the worker; `simplex manage` prepares, inspects and removes
its allocation. An older installation without `manage` is refused before a
worker starts.

## Keep a container running

Create one container and leave it running while the Hub uses it. Do not use
`--rm`, and do not make the worker itself PID 1: the Hub needs the container
after a worker exits, until its allocation is deleted.

```sh
# Export on the host before creating the container (both alternatives below).
export MODEL_API_KEY='your-api-key'

# Host networking: the worker can reach the Hub's loopback listeners.
docker run -d --name simplex-worker --network host \
    -e MODEL_API_KEY \
    simplex-worker:latest

# Alternatively, keep Docker's bridge network and map the host by name:
docker run -d --name simplex-worker \
    --add-host host.docker.internal:host-gateway \
    -e MODEL_API_KEY \
    simplex-worker:latest
```

The container's own environment is the worker's environment. Model credentials
must be present there, because the worker expands `${MODEL_API_KEY}` from its
own process environment; `docker exec` does not copy the Hub's environment into
the container. Exporting the key on the host after container creation does not
update the retained container. The bundled worker YAML uses
`api_key: ${MODEL_API_KEY}`; if you choose another variable, change both the YAML
and the container environment.

Verify presence without printing the credential:

```sh
docker exec simplex-worker sh -c 'test -n "$MODEL_API_KEY" && echo "the container can see the key"'
```

Only the allocation directory (`management.root`) holds worker state. It lives
inside the container, so mount it if the container may be recreated:

```sh
docker run -d --name simplex-worker --network host \
    -v simplex-worker-allocations:/var/lib/simplex/allocations \
    -e MODEL_API_KEY simplex-worker:latest
```

The Hub does not need a bind mount of its session directory. It generates the
worker configuration through `simplex manage`, and the manager writes the
effective configuration inside the container.

## Configure the Hub

```sh
simplex-hub --listen 0.0.0.0:8800 --panel-token YOUR_RANDOM_PANEL_TOKEN
```

Keep the listener on a trusted network or behind an authenticated TLS proxy.
The tool listener normally inherits the bind host and uses port 8801. Both must
be reachable from the container when their features are enabled.

Select the seeded **Docker (running container)** launch configuration and your
worker YAML when creating a session, then adjust the container name, executable
and root to your deployment. `worker.connectHost` (or per-channel `endpoints`)
must name an address reachable from the container: the bridge gateway, for
example `172.17.0.1`, or `host.docker.internal` when the container has the
`--add-host` mapping above. Container `localhost` refers to the container.

## Launch profile

```jsonc
{
    "management": {
        "target": {
            "kind": "docker",
            "command": "docker",
            "args": [],                       // global options, e.g. --context
            "container": "simplex-worker",
            "preflight": {                    // optional; omitted fields use these values
                "enabled": true,
                "timeoutMs": 5000,
                "container": true,
                "executable": true,
                "endpoint": { "enabled": true, "channels": ["events"] }
            }
        },
        "executable": "/opt/simplex/bin/simplex",
        "cwd": "/home/operator",              // Hub-local Docker CLI directory
        "env": {},                            // Hub-side Docker connection variables
        "root": "/var/lib/simplex/allocations", // container-local allocation root
        "timeoutMs": 30000
    },
    "worker": {
        "threads": 1,
        "connectHost": "172.17.0.1",
        "stopTimeoutMs": 15000,
        "sigtermGraceMs": 5000,
        "sigkillGraceMs": 2000
    },
    "env": {},
    "endpoints": {}
}
```

`management.cwd` and `management.env` describe the Docker CLI process on the Hub;
`executable` and `root` describe paths inside the container. `args` may carry
global Docker options such as `--context`, but never an image, a `run` command or
a lifetime flag.

## Startup checks

Before the first target mutation, the Hub runs read-only checks in this order:

| Check | How | Failure |
| --- | --- | --- |
| Container state | `docker inspect` state and network mode | `container-missing`, `container-not-running`, `inspect-failed` |
| Installation | `docker exec CONTAINER EXE --version` | `executable-missing` (126/127), `executable-failed` |
| Hub endpoint | effective advertised address versus the container network | `endpoint-loopback`, `endpoint-network-none`, `endpoint-host-mapping`, `inspect-failed` (unparsable advertised endpoint) |
| Docker CLI | spawn failure or deadline | `docker-unavailable`, `inspect-timeout`, `executable-timeout` |

The endpoint check does not open a connection. It rejects a loopback advertised
address when the container uses a private network namespace (host networking is
accepted; a container sharing another container's network namespace is accepted
as an operator assertion), and a `host.docker.internal` name without a host
mapping. Use `--add-host` or a bridge address, or set `worker.connectHost` / the
channel `endpoint` override.

A failed check refuses the start. A new session stays **unbound**, so its launch
and worker configuration can still be edited before retrying. Disable a single check only
when the deployment makes it meaningless; for example
`"preflight": {"executable": false}` for an image that installs `simplex` on a
mount created after the container starts. `"enabled": false` skips all checks.
These overrides belong to a new session: a session that already bound an
allocation keeps its launch policy. Existence is always checked;
`container: false` skips only the running assertion. A stopped container still
refuses an installation probe.

Every Docker CLI call uses `preflight.timeoutMs`. The public error names the
container and, for a CLI failure, one sanitized diagnostic line; it never includes
argv, environment values or the endpoint URL with its session token.

## Lifecycle

1. The Hub prepares the allocation with `simplex manage` and captures the
   effective configuration for this lifecycle.
2. It records dispatch intent, then runs the foreground
   `docker exec CONTAINER EXE run --config ...` transport.
3. A graceful stop sends the worker's own shutdown signal. The Hub waits for that
   foreground command to finish; it does not signal the container.
4. Deleting a session asks the manager to retire the allocation, then removes
   the Hub-local bookkeeping. The container is left running.

If stop cannot confirm that the worker exited (the transport was lost, or the
container disappeared), the Hub reports `remote-ownership-unconfirmed` and keeps
the allocation. Restarting is blocked while a start is unconfirmed. Remote
force-kill is deliberately unsupported: kill the worker inside the container and
let the manager observe an idle allocation, or delete the session to retire it.

Keep the container running until every session that targets it is deleted.
Removing the container leaves allocations unreachable; recreate the container
and start fresh sessions instead of reusing their records.

## Process terminals

Process tools use separate pipes by default. For a program that requires a real
terminal, the model can use `spawn_process` with `tty: true`.
The Linux worker allocates its own controlling PTY; the worker container does
**not** need `docker run -t`, an attached console, privileged mode or host terminal
mounts. It does need a usable `/dev/pts` (devpts) and `/dev/ptmx`, plus Linux kernel
4.13 or newer. Standard Docker containers provide these devices. A deployment
that masks them or forbids allocation receives a tool error for TTY launches;
the worker never silently substitutes pipes, and ordinary pipe launches remain
available.

Terminal stdout/stderr merge into one `output` block. Initial settings are
canonical input, echo off, 80×24 and `TERM=dumb`; child programs can change them.
Control sequences and carriage returns remain in the result. The panel displays
text, not a reconstructed terminal screen. `end_input` requests EOF; it neither
physically half-closes a PTY nor guarantees that the child exits. Unsupported
terminal modes are reported in subsequent read/poll metadata.

Cancelling an agent run leaves existing process sessions available. Worker
shutdown joins owned process and I/O tasks; it terminates the direct child and
bounds capture drainage. Closing the terminal can hang up descendant slave
holders, but this is not a guarantee of process-tree cleanup. IDs from a previous
worker instance do not identify live sessions after restart.

## Security

Run the container with limited mounts, credentials and network access, and prefer
an unprivileged user. The seeded `management.root`
(`/var/lib/simplex/allocations`) is not writable by a non-root user on a stock
image: create it and set ownership in the image, or mount a volume there.
Never mount the Docker socket into the worker container: a worker tool that
reaches it can control the host. The Hub's Docker credentials belong to the
operator's environment (`management.env`), not to worker tools.
