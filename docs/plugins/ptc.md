# Experimental Python tool composition

PTC lets the model compose installed functions in a Python program and return a
small selected result. It is optional, source-installed, and not a general Python
shell. The worker and interpreter are separate programs; no Python installation
is needed when PTC is not configured.

## Install and enable

Download the separate `simplex-ptc-vX.Y.Z-source.tar.gz` asset from the matching
[GitHub release](https://github.com/Hazer-BJTU/simplex-cpp/releases). Verify it
against `SHA256SUMS`, extract it, and follow the included `INSTALL.md`. The
archive contains source, tests, the lockfile and license; Python and dependencies
are installed locally, not bundled. This is not a PyPI publication.

Alternatively, from a matching source checkout, install the standalone
interpreter with [uv](https://docs.astral.sh/uv/):

```sh
uv sync --directory ptc --locked
```

Add this to the worker YAML, using paths available **on the worker host**:

```yaml
tools:
  intrinsic: [ptc, archive_memory, modality_assist, hub_remote_call]

persistence:
  enabled: true
  directory: /absolute/path/to/session
  state: state
  memory: memory

ptc:
  executable: /absolute/path/to/simplex-cpp/ptc/.venv/bin/simplex-ptc
  arguments:
    - --module
    - simplex_ptc.tools.process
    - --module
    - simplex_ptc.tools.files
```

The arguments precede the worker-supplied CLI subcommand. They are literal argv,
not shell syntax; do not include `execute` or `--state-directory` yourself.
Only trusted installed extension modules should be configured. Omitting a module
removes its functions; `manual()` discovers the actual registry.

PTC requires enabled persistence and an explicit directory. State is stored in
`<persistence.directory>/ptc/`, separately from the conversation backend. Ordinary
restarts and compaction retain it. Clean-fork children use their own persistence
root and inherit no PTC state. A fresh worker history with existing PTC state
(`restore: never`) is rejected; choose a fresh directory or deliberately manage
that interpreter session first. Interpreter/tool upgrades can invalidate its
checkpoint fingerprint and require an explicit operator decision.

The configured `worker.environment.workspace`, when present, is also used as
PTC's execution working directory and must exist. It is not a filesystem sandbox.
Other native tools keep their existing working-directory behavior.

## Model interface

The worker exposes one program-level tool, `ptc_execute(source)`. Source is limited
to 128 KiB and is sent over stdin, never interpolated into a shell command.
It follows the normal confirmation policy. Native process/file tools remain
available; Python source never receives shell regex bypass treatment.

```python
for info in manual():
    print(info.name, info.summary)

print(manual("open_text").render())
```

Programs may print output and return a final expression. Strings are shown as
text; other values as JSON. Result previews and omitted output are explicitly
marked. Keep returned data small; redirect large command output to files.
See the [interpreter guide](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/ptc/README.md)
for command handles, original-coordinate file editing, and rendering options.

Successful execution commits interpreter state. Handled Python errors preserve
the preceding Python checkpoint, but **do not undo files, processes, or consumed
output**. Persist command IDs, not host handles. File edits require explicit
commit. Commands created before a failed execution are reported when available.

Cancellation asks the interpreter to settle the exact execution. It does not
promise to stop `keep` commands; they remain owned by the daemon until explicitly
stopped, retention expires, or worker shutdown stops the daemon. Worker shutdown
waits for daemon cleanup before releasing its session ownership. Never delete a
PTC directory while its daemon is alive.

Definite admission refusals (for example, a stale epoch after an operator reset)
report `submitted: false`, `commit_status: none` and an empty `commands_created`
list. The worker re-inspects session metadata without retrying the code, so a
later new call can proceed with the current epoch. If that inspection fails, PTC
remains disabled until the operator resolves the state and restarts the worker.

A transport/storage error or recovery-required state can leave the outcome unknown.
These are distinct from admission refusals. The worker blocks further
PTC execution in that instance and reports an execution ID. Inspect the session
using the same executable, modules and state directory before restarting; never
retry the original source merely to obtain a missing result. No automatic reset,
recovery acknowledgement, or source replay is performed.
