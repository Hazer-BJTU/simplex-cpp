# Command-line reference

The installed public worker entry point is `simplex`. Its only subcommand is
`run`. The launcher preserves arguments and uses `exec` to replace itself with
the adjacent `simplex_worker` binary, so signals reach the worker directly.

```sh
simplex --help
simplex --version
simplex run --help
simplex run --version
simplex run --config /path/config.yaml --session demo --threads 2
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--help`, `-h` | — | Show help without requiring other flags |
| `--version` | — | Print the compiled worker version and exit without requiring a session or loading configuration |
| `--config`, `-c` | `config.yaml` | Startup YAML, relative to invocation cwd if not absolute |
| `--session`, `-s` | Required | 1–128 ASCII letters, digits, underscores, or hyphens |
| `--threads`, `-t` | `1` | Positive number of io_context execution threads, including the main thread |

The worker remains attached to the terminal. SIGINT and SIGTERM request orderly
shutdown. Reported startup/runtime exceptions exit with status 1; invalid
launcher subcommands exit with status 2.

`simplex --version`, `simplex run --version`, and `simplex_worker --version`
print the same `MAJOR.MINOR.PATCH` value followed by a newline and exit with
status 0. The version is embedded at build time from the repository's `VERSION`
file; querying an installed worker does not read a local version file or start
network connections. If both `--help` and `--version` are supplied, help takes
precedence. Command-line parsing still rejects unknown options.

`--session` does not append a directory to `persistence.directory`.
`--threads` does not allow overlapping agent-loop invocations.

The deprecated `simplex_shell` source is not installed or built as a public
command. The separately installed `simplex-hub` CLI is described in the
[Hub deployment guide](../deployment/hub.md).
