#!/bin/bash
# Entrypoint for docker/Dockerfile.hub-test.
#
# Two jobs: say what this container is and how to reach it, then become the hub
# via `exec` so the hub is PID 1 and receives the `docker stop` SIGTERM itself.
# That matters more here than in most images: the hub's shutdown path is what
# stops its worker children over the protocol, waits for them, and lets the
# worker save its session state. A shell wrapper that swallowed the signal
# instead would leave that cleanup to a SIGKILL.
#
# Any arguments replace the default: `docker run -it --rm <image> bash` gets a
# shell with the build tree, the hub and its dependencies already in place.
set -euo pipefail

PORT="${HUB_PORT:-8800}"
TOKEN="${HUB_PANEL_TOKEN:-simplex-hub-dev}"
DATA_DIR="${HUB_DATA_DIR:-/data}"
MOCK_FLAG="--mock"
MOCK_STATE="enabled"
if [ "${HUB_MOCK:-1}" = "0" ]; then
    MOCK_FLAG="--no-mock"
    MOCK_STATE="disabled by HUB_MOCK=0"
fi

if [ "$#" -gt 0 ]; then
    exec "$@"
fi

cat <<EOF

simplex hub — disposable test container
-------------------------------------------------------------------------------
  panel     http://127.0.0.1:${PORT}/?token=${TOKEN}
            (publish the port to loopback: -p 127.0.0.1:${PORT}:${PORT})

  data      ${DATA_DIR}   sessions, generated worker configs, logs, JSONL
            container-local unless you mount a volume; rm the container and
            it is gone

  worker    /src/build/bin/simplex_worker   Debug, built inside this image
  sources   /src                            the tree this image was built from

  mock      ${MOCK_STATE} — the hub serves its own model, no key needed.
            In the panel, create a session with the "mock" provider profile
            (model mock-auto) and send anything: the scripted model proposes a
            run_command call, so you get a real confirmation prompt, a real
            child process, and real output.
  real      docker run ... -e DEEPSEEK_API_KEY=sk-... and pick the "deepseek"
            profile instead. The key only ever enters the worker's environment.

  shell     docker run -it --rm <image> bash

Everything a session executes runs inside this container.
-------------------------------------------------------------------------------

EOF

exec node /src/hub/bin/simplex-hub.js \
    --config /etc/simplex-hub/hub.config.jsonc \
    --listen "0.0.0.0:${PORT}" \
    --panel-token "${TOKEN}" \
    --data-dir "${DATA_DIR}" \
    --log-level "${HUB_LOG_LEVEL:-info}" \
    "${MOCK_FLAG}"
