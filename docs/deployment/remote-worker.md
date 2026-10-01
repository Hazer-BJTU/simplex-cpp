# Deploy a worker on a remote host

The worker can run on a different host because it initiates every WebSocket
connection. A remote deployment still needs a registered Hub session, its bearer
token, and endpoints reachable from the remote host. The Hub does not currently
provide a built-in SSH deployment service.

## 1. Provision a session on the Hub

Configure the Hub for private-network access or a TLS reverse proxy, as described
in [Hub deployment](hub.md). Create a session with the desired configuration
pair, but do not start a local worker for it. For example, an authenticated API
request can register a session:

```sh
curl --fail-with-body -X POST http://127.0.0.1:8800/api/sessions \
  -H "Authorization: Bearer $PANEL_TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"session":"remote-demo","spec":{"launchConfig":"local","workerConfig":"default"}}'
```

Run this on the Hub host or through a protected connection. Library IDs must
already exist. The Hub stores session tokens in `<dataDir>/hub.json`; a trusted
administrator can obtain the `remote-demo` entry's token there. The panel token
is a different credential and cannot authenticate worker sockets. Do not paste
`hub.json` into logs or distribute other sessions' tokens.

## 2. Prepare the remote host

Install the worker release and create a private configuration file based on the
[worker example](../getting-started/configuration.md). Replace its managed
markers with actual URLs for the registered session:

```yaml
client:
  endpoint: wss://hub.example/agent/remote-demo/events?token=SESSION_TOKEN
security:
  confirmation:
    endpoint: wss://hub.example/agent/remote-demo/confirm?token=SESSION_TOKEN
hub_remote_call:
  endpoint: wss://tools.example/agent/remote-demo/tools?token=SESSION_TOKEN
persistence:
  enabled: true
  directory: /srv/simplex/remote-demo
  state: state
  memory: memory
  restore: if_present
```

This fragment supplements provider/driver configuration; it is not a standalone
complete configuration. Replace example hosts and token, and omit remote tools
if not needed. The proxy must forward events and confirmation to the main Hub
listener and tools to the separate tool listener. Direct private-network
connections can instead use `ws://HOST:8800` and `ws://HOST:8801`.

Set paths for the remote filesystem. Do not reuse an absolute Hub-host
persistence path unless the same path really exists and is intended remotely.
Protect the YAML file, exported API credentials, state, and archives with the
remote account's permissions.

## 3. Start and supervise

```sh
export MODEL_API_KEY='your-api-key'
cd /srv/project
/opt/simplex/bin/simplex run \
  --config /srv/simplex/remote-demo/config.yaml \
  --session remote-demo --threads 2
```

Use an OS service manager for automatic restart and remote logs. The Hub can send
protocol cancellation and shutdown to a connected worker, but it cannot signal
an unknown remote PID or restart the remote process through its local supervisor.
Avoid the Hub's local Start/Restart actions for this manually managed session.

History queries, model events, confirmation, and plans work over the protocol.
The Hub's snapshot endpoint reads its own filesystem and cannot inspect remote
`state.json` without an explicitly managed shared storage arrangement. Process
logs also remain remote. Archive paths reported by the worker are remote paths;
the Hub does not delete those files.

## Reconnect and identity

The stable event client intentionally retries connection-establishment failures
indefinitely with capped backoff, including bad credentials or certificates.
Inspect logs when it cannot connect; a living process is not proof of a healthy
session. Binary application frames are fatal protocol errors. A reconnect does
not automatically resume the agent loop or replay uncertain requests.

Keep the Hub's session metadata and token across restarts. Deleting and recreating
its session changes the token; stop the old worker and provision new endpoints
before starting again. Only one worker should own a session, even across hosts
whose independent local locks cannot protect the same logical identity.
