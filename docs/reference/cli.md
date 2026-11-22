# Command line

| Command | Use |
| --- | --- |
| `simplex run` | Start a worker on its execution target. |
| `simplex manage` | Prepare or inspect target-local session storage, or retire an allocation. |
| `simplex protocol` | Read the installed worker's wire contract offline. |
| `simplex-hub` | Start the Hub service or install a worker on the Hub host. |

## Run a worker

```sh
simplex --version
simplex run --config ./worker.yaml --session demo --threads 2
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--config`, `-c` | `config.yaml` | Worker YAML. Relative paths on this command line resolve against the invocation directory. |
| `--session`, `-s` | Required | Session ID, 1–128 ASCII letters, digits, underscores or hyphens. The worker uses the configured persistence directory directly. |
| `--threads`, `-t` | 1 | Positive count of execution threads including the main thread. Each worker still runs one agent loop at a time. |
| `--help`, `-h` / `--version` | | Print usage or the compiled worker version and exit. |

The worker runs in the foreground. SIGINT and SIGTERM request orderly shutdown.
Supply the session's actual endpoints when starting it yourself. The Hub
launcher fills managed endpoints when it starts the worker. [Worker settings](worker-settings.md)

## Manage worker storage

The Hub normally invokes `simplex manage --request BASE64` on the target.
Each invocation accepts one UTF-8 JSON object encoded as standard padded
Base64 in the command-line argument. All operations require `version: 1`,
`request_id`, `operation`, `allocation_id` and the absolute, normalized
allocation `directory` on that target. Use the allocation ID and path from the
managed session record. The directory belongs to one allocation beneath the configured root.

| Operation | Additional request fields | Result or effect |
| --- | --- | --- |
| `prepare` | `configuration` (worker YAML as a string) on creation, optional `refresh` | Create an allocation and its managed config reference. To change connection overrides on a prepared allocation, send the full replacement `refresh` and its `expected_revision`. |
| `inspect` | None | Read status, revision, and available configuration and startup metadata. |
| `capture_startup` | `lifecycle_id`, `expected_revision` | Capture effective startup configuration for that lifecycle. |
| `clean_fork` | `source` with the parent's `allocation_id`, `directory`, `lifecycle_id` and `revision`; optional `refresh` | Prepare the child from captured startup configuration with independent empty state. |
| `read_state` | None | Return complete decoded SQLite or JSON state. The manager sets no response size cap. |
| `remove` | None | Delete the specified allocation once exclusive ownership is established and retire its identity. |

### Inspect an allocation

On the target, replace the two example values with an existing allocation.
This read-only example prints its status and revision. It requires Python 3.

```sh
python3 - allocation-id /absolute/path/to/allocation <<'PYTHON'
import base64, json, subprocess, sys

request = dict(version=1, request_id="inspect-1", operation="inspect",
               allocation_id=sys.argv[1], directory=sys.argv[2])
encoded = base64.b64encode(json.dumps(request).encode()).decode("ascii")
proc = subprocess.run(["simplex", "manage", "--request", encoded],
                      capture_output=True, text=True)
reply = json.loads(proc.stdout)
if proc.returncode or not reply["ok"] or reply["request_id"] != request["request_id"]:
    raise SystemExit(reply.get("error", "Invalid manager response"))
print(reply["result"]["status"], reply["result"]["revision"])
PYTHON
```

For `prepare`, the optional `refresh` object replaces the endpoint overrides
(`client_endpoint`, `confirmation_endpoint`, `tools_endpoint`). Optional channels
must already be enabled in the source YAML.

Stdout contains one JSON response. Errors carry `error.code` and
`error.message` and return nonzero. A transport failure can leave the result
of a mutation unknown. Inspect the allocation before retrying. Configuration
and state may contain credentials or private conversations. Base64 is an
encoding, and the OS limits argument size. [Allocation ownership](launch-profiles.md#paths-and-ownership)

## Protocol inspection

```sh
simplex protocol > worker-protocol.json
simplex protocol --hash
```

These commands print the compiled manifest or its SHA-256 fingerprint without
loading a configuration or starting a session. [Worker protocol](../core/worker-protocol.md)

## Hub and worker installer

```sh
simplex-hub --version
simplex-hub install-worker --platform linux-amd64-standard --update-path
simplex-hub --data-dir ./hub-data --listen 127.0.0.1:8900
```

Use `--config FILE` for an explicit Hub startup file, `--panel-token TOKEN`
for panel access, and `--log-level LEVEL` for diagnostics. `simplex-hub --help`
and `simplex-hub install-worker --help` list the options for each command.
[Installation](../getting-started/installation.md) · [Hub settings](hub-settings.md)
