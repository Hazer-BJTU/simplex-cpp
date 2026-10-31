# Simplex PTC interpreter

An independent, experimental Python/uv package built on pinned Pydantic Monty.
It executes a Python subset, preserves idle interpreter state across invocations,
and exposes an extension registry. No built-in tools, worker integration, Hub
networking, end-to-end tests, or release artifacts are included in this phase.

## Install and run

Python 3.12+ and uv are required. The current storage implementation supports
Linux/POSIX local filesystems with `flock`, atomic rename and directory `fsync`.
Windows and network filesystem durability are not supported in this prototype.
Monty is installed from its published wheels; no C++ build is needed.

```sh
cd ptc
uv sync --locked
uv run simplex-ptc capabilities
printf '%s' '{"execution_id":"one","source":"x = 40\nx"}' |
  uv run simplex-ptc execute --state-directory /tmp/my-ptc-session
# Substitute the epoch returned by the first response (or session inspect).
printf '%s' '{"execution_id":"two","epoch":"EPOCH_FROM_FIRST_RESPONSE","source":"x + 2"}' |
  uv run simplex-ptc execute --state-directory /tmp/my-ptc-session
uv run simplex-ptc session inspect --state-directory /tmp/my-ptc-session
```

The trailing expression is the result; statements alone return null. Stdout is
one JSON object; sandbox prints are captured in `output`, while interpreter and
extension diagnostics go to stderr. Exit codes: 0 success, 1 recorded execution
failure, 2 request/storage/recovery/interpreter failure. A successful OS exit is
not a substitute for checking the response contract.

The CLI takes a single UTF-8 JSON request (execution_id, source, and epoch) on stdin.
Omit epoch only for the first execution; subsequent calls use the returned epoch.
Reset invalidates requests carrying the old epoch. Retries preserve the original
request, including its epoch (or the initial absence of one).
No shell evaluates the source. Supply a new execution ID for each new request.
The latest ID and source digest are retained: repeating that ID returns its saved
response, and changing its source is rejected. Older IDs are not remembered;
callers must not retry old requests or recycle IDs. This is not an exactly-once
execution service.

## State and failure semantics

Each state directory is one logical session. Each invocation owns a fresh Monty
pool with one subprocess, restores its previous idle dump, executes one feed,
and closes the pool. Supported globals and functions persist. Host handles,
processes and callbacks do not. The interpreter does not mount host filesystems.

The directory contains `session.lock`, a single `checkpoint` (metadata, opaque
dump and most recent response), and, while executing, `execution.pending`.
Concurrent execution/reset fails with `busy`. Lock files are never removed;
process exit releases the OS lock. State files are private data in a trusted
operator-owned directory, not a snapshot import interface. Checksums detect
accidental damage, not malicious replacement.

A successful result is converted and bounded before publishing the new state.
Uncaught execution/conversion errors retain the previous Python state and save a
failure receipt. External extension effects are never rolled back. Write/sync
failures may occur after publication: inspect state rather than assuming rollback
or repeating source. A matching committed receipt identifies the published generation, but does not
prove durability. Before replaying a receipt or admitting a new request with a
matching leftover marker, execution synchronizes the checkpoint directory, then
removes and durably settles the marker. Persistent synchronization errors remain
storage failures; source is never re-executed during reconciliation. Cached
replies repeat this fence even if the marker's unlink succeeded before a failed
sync. Inspection remains observational. An unmatched marker blocks further code until explicitly acknowledged:

```sh
uv run simplex-ptc session recover --state-directory /tmp/my-ptc-session \
  --epoch EPOCH_FROM_INSPECT --execution-id PENDING_ID
uv run simplex-ptc session reset --state-directory /tmp/my-ptc-session \
  --epoch EPOCH_FROM_INSPECT
```

Recovery records an unknown-effects failure without replay. Reset creates an
empty epoch, preserves session identity and external effects, and requires any
pending interruption to be acknowledged first. Inspect never dumps the heap or
repairs state. Incompatible/corrupt snapshots fail visibly; preserve the directory
for diagnosis and explicitly select a new empty directory after such failures.
There is no migration, source replay, or suspended-execution restoration.

SIGTERM/Ctrl-C unwind owned runtime contexts where possible. SIGKILL cannot
execute cleanup; pending records provide interruption detection on restart.
Never infer external effect rollback from the old checkpoint. A future worker
must keep this directory separate from its conversation and compact archives.

## Limits and compatibility

Defaults: 1 MiB request, 48 KiB JSON response, 16 KiB captured print output,
32 MiB encoded checkpoint, 64 MiB sandbox heap, 5 seconds sandbox execution per
feed, 1 second total sleeping. The Monty pool additionally has a 10-second
per-turn backstop. These are independent limits; synchronous trusted callbacks
must bound their own blocking work. They are not configurable by submitted code.

Print output is truncated with omitted UTF-8 byte counts. Oversized JSON results
become explicitly marked previews; unsupported values fail conversion. The
entire escaped response is budgeted before publication. Dumps carry their own
limits: runtime version, resource policy and extension descriptors must match
exactly before restore. The pinned runtime resets feed clocks for each feed;
its session dump is used only at idle boundaries. Checkpoints do not grow into
an unbounded archive. Crash-left interpreter staging files are never loaded and are removed under
the session lock on the next operation. Other files are left untouched.

## Extensions

No production host tools are bundled. Install trusted modules into the same
virtual environment and select them explicitly with `--module package.module`
(before the subcommand). A module exports `register_tools(registry)`:

```python
from simplex_ptc import ToolDescriptor


def register_tools(registry):
    registry.add(
        ToolDescriptor(
            name="double",
            description="Double an integer.",
            signature="double(value: int) -> int",
            result="integer",
            effects="none",
            version="1",
        ),
        lambda value: value * 2,
    )
```

The manifest and Monty bindings share one registry. Duplicate/invalid names fail. Python builtin names (including `len`, `print`,
and `open`) are reserved, even when Monty does not yet implement them: its
external-name lookup cannot override builtins. Use distinct tool names such as
`read_file`. User code must also avoid shadowing tool names with its own globals.
Change a descriptor's version when implementation semantics change. Extensions
are trusted host code; their descriptors do not confer permissions or sandbox
their effects. Initial callbacks are synchronous and exchange plain JSON-compatible
values only (string-keyed objects, arrays, strings, finite numbers, booleans and
null; tuples become arrays). Nesting is limited to 128 levels. They must settle their own resources before returning, must not
write directly to protocol fd 1, and must not retain live resources in returned
values. Async callbacks and execution-owned resource services can be added when
real adapters need them; this package does not pretend to provide those yet.

## Development

```sh
uv run --locked pytest
uv run --locked ruff check .
uv run --locked ruff format --check .
```

Tests exercise only this package, real Monty and local temporary storage, with
fixture callbacks. There is no worker/Hub end-to-end integration. The independent
PTC workflow runs only for `ptc/**` or its own workflow; native CI classifies those
paths as independent. Changes to the shared CI classifier still conservatively
run native validation. No Python dependency is added to the C++ build.
