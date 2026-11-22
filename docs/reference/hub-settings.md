# Hub settings

The Hub reads `hub.config.jsonc` in its data directory (default
`~/.simplex/hub`). Use `simplex-hub --data-dir DIR` to choose a root or
`--config FILE` to select a file explicitly. The CLI writes a compact file on
first startup if needed. An explicit `--config` path must already exist.
Settings are applied in order: built-in defaults,
file, then CLI overrides. Restart the Hub after editing the file.

## Listeners and access

This complete startup file changes the main port while preserving loopback access:

```json
{
  "listen": {"host": "127.0.0.1", "port": 8900},
  "toolRequests": {"port": 8901}
}
```

| Field | Default | Use |
| --- | --- | --- |
| `dataDir` | `~/.simplex/hub` | Hub-owned library, session metadata and logs. Explicit file paths resolve against the config directory; CLI path arguments resolve against the invoking shell's directory. |
| `listen.host`, `listen.port` | `127.0.0.1`, 8800 | HTTP panel/API and worker event and approval sockets. |
| `panel.token` | Empty | Shared operator credential. A non-loopback main listener requires a nonempty token. |
| `toolRequests.host`, `port` | Main listener host, 8801 | Remote-tool listener. Choose a distinct port. |
| `toolRequests.timeoutMs`, `questionTimeoutMs`, `maxConnections` | 120000 ms, 1800000 ms, 128 | Remote request deadline, operator question deadline, and concurrent connection cap. |
| `forceKillProcessGroup` | `false` | Allow force-stop to target the local process group, including descendants. |

Use TLS termination and protect access when exposing listeners outside a
trusted local network. A worker's advertised address belongs in the
[launch profile](launch-profiles.md#worker-and-network-settings).

## Panel background

Merge this fragment into the Hub startup file, retaining any existing panel token:

```json
{"panel": {"background": {
  "image": "./assets/background.jpg", "size": "cover", "position": "center",
  "opacity": {"light": 0.12, "dark": 0.08}
}}}
```

`image` is a local static PNG or JPEG relative to the startup file (up to 8 MiB
and 33,554,432 pixels). An empty path disables it. `size` defaults to `cover`
and also accepts `contain`, `auto`, or one/two dimensions in px or percent.
`position` defaults to `center` and accepts an edge or corner such as `top right`.
Each opacity accepts a number from 0 to 1 or a light/dark pair. Defaults are
0.12/0.08 for `opacity`, 0.6/0.6 for `toolCardOpacity`, and 0.3/0.45 for
`frameOpacity`.

## Resource limits

Use positive integer counts or byte sizes unless indicated. For example,
`{"limits":{"logFiles":3},"subagents":{"maxChildren":2}}` keeps three worker
log files including the active file and limits each parent to two direct children.

| Keys under `limits` | Default | Controls |
| --- | --- | --- |
| `transcriptEvents`, `transcriptBytes` | 5000, 32 MiB | Per-session in-memory event retention. |
| `logLines`, `logRingBytes` | 500, 256 KiB | In-memory worker log retention. |
| `logBytes`, `logFiles` | 8 MiB, 2 | Disk log rotation size and file retention. |
| `maxMessageBytes` | 32 MiB | Worker message and request-body ceiling. Individual routes may impose smaller caps. |
| `pingIntervalMs`, `confirmIdentityHoldMs` | 30000, 15000 ms | Worker heartbeat interval (0 disables) and maximum wait to verify an approval's worker identity. |

| Keys under `subagents` | Default | Controls |
| --- | --- | --- |
| `maxLive`, `maxChildren`, `maxDepth` | 16, 4, 3 | Live children across the Hub, per parent and maximum delegation depth. |
| `startupTimeoutMs` | 30000 ms | Time allowed for child startup. |
| `maxReceipts`, `receiptTtlMs` | 200, 3600000 ms | Retained operation receipts and their lifetime. |
| `conversationBytes` | 1 MiB | Bounded primary conversation per child. |

Subagent limits are positive integers. The maxima are 128 live children, 32
direct children per parent, depth 16 and 1000 receipts. The conversation budget
must be at least 4096 bytes. Worker log ring capacity starts at 64 bytes.

Verify the resolved listener in Hub startup output. See the
[Hub startup template](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/hub/hub.config.example.jsonc)
and [subagent limits](../hub/subagents.md#resource-configuration).
