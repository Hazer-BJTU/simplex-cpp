# Deploy the Hub and a worker

Run the Hub on your Linux host and choose where the worker runs: on the same
host or inside a Docker container. Both use the same panel and worker YAML.

## Start the Hub

Follow the [installation guide](../getting-started/installation.md) to install
the Hub. For a local worker, also install the worker and put `simplex` on the
Hub's PATH before startup. In a terminal on the host, run:

```sh
export MODEL_API_KEY='your-provider-key'
simplex-hub
```

The panel, worker events, and approval requests share the main listener, which
defaults to loopback (`127.0.0.1`) on port **8800**. To use a different port, start the Hub
with, for example, `simplex-hub --listen 127.0.0.1:8900`, or set `listen.port`
in `~/.simplex/hub/hub.config.jsonc` and restart. Optional remote tool calls
use a separate listener on port **8801** by default (`toolRequests.port`).

Leave the Hub running while using the panel. Local workers inherit its
environment. For Docker, supply the credential when creating the container.

## Configure the model

In **Configurations**, create a worker configuration from the template. Open it in
the configured Hub editor, or download and edit a copy locally. Replace
its `providers` and `driver_model` sections with settings for your account. For
example, this fragment uses the bundled DeepSeek plugin and its default endpoint:

```yaml
providers:
  primary:
    plugin: deepseek
    model: deepseek-flash
    endpoint:
      auth:
        scheme: bearer
        api_key: ${MODEL_API_KEY}
driver_model: primary
```

Save the file (use **Replace from file** for a downloaded copy). Keep the template's Hub endpoint and session directory
placeholders so the Hub fills them for each session. See the
[provider guides](../providers/index.md#provider-configuration) for other models.

## Run a local worker

Run `simplex --version` in the Hub's shell environment to check the installation.
Choose the `local` launch configuration when creating the session. If the
executable is outside the Hub's PATH, set `management.executable` in the launch
configuration to the absolute path of the installed `bin/simplex`.

## Run a Docker worker

On a Linux Docker host, extract the verified standard Linux x86_64 worker
release into the build context so that `worker/bin/simplex` exists. Save this
`Dockerfile` beside the `worker/` directory:

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      bash ca-certificates libssl3 util-linux && rm -rf /var/lib/apt/lists/*
COPY worker/ /opt/simplex/
ENV PATH="/opt/simplex/bin:${PATH}"
RUN useradd --create-home worker && \
    install -d -o worker -g worker /var/lib/simplex/allocations
USER worker
WORKDIR /home/worker
CMD ["sleep", "infinity"]
```

In a second host terminal, build the image and start one persistent container.
Host networking lets the worker reach the Hub's loopback listeners. The named
volume holds worker allocations and the container user needs write access to it.
Protect panel access on the shared network by setting `panel.token` in
`~/.simplex/hub/hub.config.jsonc` and restarting the Hub.

```sh
docker build -t simplex-worker:latest .
export MODEL_API_KEY='your-provider-key'
docker run -d --name simplex-worker --network host \
  -v simplex-worker-allocations:/var/lib/simplex/allocations \
  -e MODEL_API_KEY simplex-worker:latest
docker exec simplex-worker simplex --version
docker exec simplex-worker sh -c 'test -n "$MODEL_API_KEY" && echo "Key available"'
```

In **Configurations**, open the `docker` launch configuration. For this
host-network example, change its existing `worker.connectHost` to
`"127.0.0.1"` and save. The template already targets `simplex-worker` and
uses `/var/lib/simplex/allocations`. The Hub runs `simplex manage` and
`simplex run` inside that container through `docker exec`. Keep the container
running until its sessions have been deleted. See
[security boundaries](../security.md#worker-permissions-and-cancellation).

## Start, verify, and stop a session

1. Create a session in the panel, select the `local` or `docker` launch
   configuration and your saved worker YAML, then select **Start**.
2. Wait for the worker to connect and become ready. Send a short message to
   verify the provider configuration.
3. Use **Cancel run** to interrupt a request or **Stop** to shut down the
   worker. Starting the same session again reuses its saved configuration and
   state. Once a managed session has an allocation, create a new session to use
   a different configuration pair.

If startup fails, check `<dataDir>/sessions/<id>/logs/worker.log`, the
`simplex` executable on the selected target, and Docker container status.
For model authentication errors, check the key in the worker's environment.
