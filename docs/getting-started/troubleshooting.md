# Troubleshooting

Use the logs from the host that runs the Hub or worker. Redact API keys, panel tokens, session tokens, and full endpoint URLs before sharing output.

## Worker cannot connect or keeps reconnecting

A worker opens its WebSocket connections to the Hub. Check the worker's standard error around a retry and verify that the resolved `client.endpoint` points to the registered session's `/agent/<session>/events` endpoint with its session token. The Hub-managed template fills this endpoint automatically; a manually launched worker must use the current endpoint and token from the Hub session. The panel token is a different credential.

On the Hub host, confirm that the configured listeners are bound and reachable from the worker:

```sh
ss -ltnp '( sport = :8800 or sport = :8801 )'
```

Port 8800 is the default panel, API, confirmation, and event listener; port 8801 is the default remote tool listener. If a reverse proxy is used, verify that it forwards the event and confirmation routes to the main listener and tool routes to the tool listener. The worker retries connection-establishment failures, including invalid credentials and certificates, so a running process alone does not confirm a healthy connection.

See [Hub deployment](../deployment/hub.md) and [remote workers](../deployment/remote-worker.md).

## Prebuilt worker exits on startup

The prebuilt Linux x86_64 worker requires glibc 2.34 or newer and OpenSSL 3. Check the host:

```sh
ldd --version | head -n 1
openssl version
```

If either requirement is missing, run the release in a supported host/container or build for the target host using the [local build guide](../building/local.md) and [portable build image](../building/docker.md#portable-release-build).

## Provider credential is missing or empty

Check only whether the variable is set; these commands do not print its value:

```sh
test -n "${MODEL_API_KEY:-}" && echo MODEL_API_KEY=set || echo MODEL_API_KEY=unset-or-empty
test -n "${DEEPSEEK_API_KEY:-}" && echo DEEPSEEK_API_KEY=set || echo DEEPSEEK_API_KEY=unset-or-empty
```

Use the variable named by the selected provider configuration, and export it in the environment that actually starts the worker (or the Hub when it launches the worker). A referenced but unset or empty variable cannot be expanded. See [worker configuration](./configuration.md#credentials-and-optional-components) and [Hub deployment](../deployment/hub.md).

## Panel or worker token is rejected

For a Hub configured with a panel token, check the panel credential against the Hub's authenticated API response:

```sh
curl --silent --show-error --output /dev/null --write-out 'HTTP %{http_code}\n' \
  --header "Authorization: Bearer ${PANEL_TOKEN}" \
  http://127.0.0.1:8800/api/sessions
```

A 401 response means the panel token is missing or invalid. Worker WebSocket connections use a separate token for each Hub session; compare the worker's resolved endpoint with that session's token in the Hub data directory. Do not substitute the panel token or share either token. See [Hub deployment](../deployment/hub.md#advertised-endpoints-and-reverse-proxies) and [remote workers](../deployment/remote-worker.md#reconnect-and-identity).

## Hub reports that a port is already in use

Check which process owns the default listener ports:

```sh
ss -ltnp '( sport = :8800 or sport = :8801 )'
```

Stop or reconfigure the conflicting service, or change `listen.port` and `toolRequests.port` in the Hub configuration. Ensure the worker, browser, firewall, and reverse proxy use the updated ports. See [Hub deployment](../deployment/hub.md#startup).

## A plugin is rejected for an ABI or toolchain fingerprint mismatch

Check the worker startup log for the plugin admission error, including `[fingerprint differs]`. Plugins must be built in the same execution context as the worker because their C++ interfaces do not provide a stable ABI. Rebuild the worker and plugins together with the supported toolchain; do not bypass the admission check. See [local build requirements](../building/local.md), [plugin development](../plugins/development.md), and [security boundaries](../architecture/security.md#native-plugins).

## A session is stuck in `tools` or `blocked` recovery

Inspect the worker's saved state and Hub event log before resuming. The `tools` phase means a dispatched tool may already have caused external effects; `blocked` means automatic progression was refused pending investigation. Confirm what the interrupted operation did before deciding whether it is safe to continue, and do not replay a side-effecting request blindly. See [recovery phases](../architecture/agent-loop.md#recovery-phases) and [Hub session recovery](../deployment/hub.md#stop-restart-and-update-configuration).

## A locally built worker cannot find shared libraries

For the documented local install, check for unresolved libraries and set the private Boost library directory in the same shell that starts the Hub:

```sh
ldd ./install-local/bin/simplex | grep 'not found'
export LD_LIBRARY_PATH="$SIMPLEX_DEPS/boost/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

No `not found` output from `ldd` means it found no missing libraries. Keep the environment variable set when launching the Hub so its workers inherit it. See [local build requirements](../building/local.md#configure-build-test-install).
