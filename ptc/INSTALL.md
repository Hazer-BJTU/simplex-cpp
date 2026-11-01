# Install the PTC source release

This is the standalone Python source for the experimental Simplex PTC interpreter.
It is not a worker binary or a complete Python distribution. No Hub service is
required. The archive's release tag and `SOURCE_VERSION` identify its matching
Simplex release; `pyproject.toml` retains the interpreter's independent package
version. Use the source asset from the same release as your worker.

## Requirements

- Linux/POSIX with a local filesystem supporting Unix sockets, `flock`, atomic
  rename and directory `fsync`. Windows and network state directories are not
  supported.
- Python 3.12 or newer and [uv](https://docs.astral.sh/uv/getting-started/installation/).
  Python 3.12 and 3.14 are tested in interpreter CI.
- Network access to install the locked dependencies from PyPI. Pydantic Monty
  includes native wheels: your Python/platform must have a compatible wheel.
  The source archive does not bundle Monty, Python, uv, or dependency licenses;
  installed dependencies retain their own licenses. Simplex's MIT license is
  included as `LICENSE`.
- Bash and the external programs you intend to use, when enabling process tools.

## Download, verify, and install

Download `simplex-ptc-vX.Y.Z-source.tar.gz` and `SHA256SUMS` from the matching
GitHub release. Replace the example version below with that release's tag:

```sh
tag=vX.Y.Z
archive="simplex-ptc-${tag}-source.tar.gz"
# Check only this asset; the shared manifest also lists worker binaries.
grep -F "  ${archive}" SHA256SUMS | sha256sum --check --strict -
tar -xzf "${archive}"
cd "simplex-ptc-${tag}"
uv sync --locked --no-dev --python 3.12
./.venv/bin/simplex-ptc capabilities
./.venv/bin/simplex-ptc --help
```

Keep the source directory and its virtual environment together. After moving the
installation, recreate `.venv` with `uv sync` because virtual environments are not
relocatable. No system-wide installation, C++ build, or manual daemon service is
required. `uv sync --locked --no-dev` uses the supplied lockfile; do not edit it
merely to bypass an installation failure.

## Try the optional tools

Only `manual` is registered by default. Enable process/file functions explicitly:

```sh
./.venv/bin/simplex-ptc \
  --module simplex_ptc.tools.process \
  --module simplex_ptc.tools.files repl
```

At the prompt:

```python
for info in manual():
    print(info.name, info.summary)
print(manual("run_command").render())
```

The default REPL uses a temporary session and stops its daemon on exit. To retain
Python state, add `--state-directory /absolute/path/to/ptc-state` after `repl`.
The daemon starts automatically. With a retained session, stop it explicitly when
finished (supply the same module selection):

```sh
./.venv/bin/simplex-ptc \
  --module simplex_ptc.tools.process \
  --module simplex_ptc.tools.files daemon stop \
  --state-directory /absolute/path/to/ptc-state
```

These host tools can modify files and execute commands with your account's
permissions. Python checkpoint rollback does not undo those external effects.

## Connect a worker

Install this package on the **worker host**, not necessarily on the Hub host.
Add the following to an otherwise complete worker configuration, replacing paths:

```yaml
tools:
  intrinsic: [ptc, archive_memory, modality_assist, hub_remote_call]

persistence:
  enabled: true
  directory: /absolute/path/to/worker-session

ptc:
  executable: /absolute/path/to/simplex-ptc-vX.Y.Z/.venv/bin/simplex-ptc
  arguments:
    - --module
    - simplex_ptc.tools.process
    - --module
    - simplex_ptc.tools.files
```

Conditional tools in the list require their own settings. Add `process`,
`reading`, or `editing` if you also want native tools; they are not implied by PTC.
The worker supplies the subcommand and `--state-directory`; do not put either in
`ptc.arguments`. Interpreter state lives at `<persistence.directory>/ptc/`.
Each worker needs an independent persistence directory. The worker starts and
stops the daemon; do not point a separate REPL at its live state directory.

Worker restarts preserve checkpoints; clean forks start independently. Upgrades
may make checkpoints incompatible with the interpreter/module fingerprint. Stop
the worker and its daemon and back up the directory before an upgrade. Do not
delete state or automatically replay code after an uncertain execution. Consult
the session inspect/recovery procedures in `README.md`.

## Test the extracted package

```sh
uv sync --locked
uv run --locked ruff check .
uv run --locked ruff format --check .
uv run --locked pytest
```

These tests are independent of the worker and Hub. See `README.md` for the CLI,
session lifecycle, built-in manuals, limits, and extension development. The
packaging script itself is for a repository checkout, not for installing the
extracted archive.
