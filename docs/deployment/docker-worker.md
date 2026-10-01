# Deploy a Docker worker

Run the Hub on the host and the worker in a container. The default Docker launch
template assumes an image named `simplex-worker:latest` containing `/bin/sh` and
an installed `simplex` on PATH. The published build images do not satisfy that
runtime role by themselves.

## Create a runtime image

Download and verify a worker release. Extract it and copy its complete directory
into an image build context as `worker/`. Add this Dockerfile:

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 ca-certificates bash \
    && rm -rf /var/lib/apt/lists/*
COPY worker/ /opt/simplex/
ENV PATH="/opt/simplex/bin:${PATH}"
RUN mkdir -p /root/workspace
WORKDIR /root/workspace
CMD ["simplex", "run", "--help"]
```

```sh
docker build -t simplex-worker:latest .
docker run --rm simplex-worker:latest simplex run --help
```

Add tools such as Git, Python, or compilers to the image only if your tasks need
them. The worker distribution does not supply every program a model may request.
This runtime image is for Linux x86_64, matching the release archive.

## Configure the host Hub

```sh
export MODEL_API_KEY='your-api-key'
simplex-hub --listen 0.0.0.0:8800 --panel-token YOUR_RANDOM_PANEL_TOKEN
```

Keep the listener on a trusted network or behind an authenticated TLS proxy.
The tool listener normally inherits the bind host and uses port 8801. Both must
be reachable from Docker when their features are enabled.

Select the seeded **docker** launch configuration and your configured worker
YAML when creating a session. Its default image is `simplex-worker:latest`;
change it if you used another tag. Set the YAML workspace hint to
`/root/workspace`. The template creates that working directory inside the
container and forwards `MODEL_API_KEY` from the Hub's environment.

The template uses `host.docker.internal:host-gateway` and advertises
`worker.connectHost: host.docker.internal`. Container `localhost` would point
back to the container. The supplied template assumes a POSIX Hub host and a local
Docker daemon; `{gid}` and source mount paths are resolved on that host.

## Mounts and ownership

Only the current session directory is bind-mounted, at its same absolute path.
The nested `config/` directory is mounted read-only. State and memory therefore
survive `--rm`, while `/root/workspace` is disposable unless you add a mount.
To preserve project files, add an explicit project mount and adjust the workspace
and command's working directory together.

The supplied template runs as container root with the Hub's primary GID and
umask `0002`. This is convenient for testing and cleanup, not a least-privilege
production profile. Adapt user IDs, writable directories, network access, and
mounts to your deployment. Do not expose the host Docker socket to worker tools.

## Lifecycle

The Hub launches `docker run` in the foreground with `--init` and `--rm`.
Graceful stop first uses the worker protocol. If a process or tool prevents
shutdown, the supervisor may escalate after its configured timeouts. Keep the
foreground launcher and its config snapshot intact so the Hub can supervise it.

For custom images, verify executable paths and runtime libraries before debugging
WebSocket routing. Logs from the foreground process are captured under the
session's `logs/worker.log`.
