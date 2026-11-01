# Simplex PTC interpreter

An independent, experimental Python/uv package built on pinned Pydantic Monty.
It executes a Python subset, preserves idle interpreter state across invocations,
and exposes an extension registry with the read-only `manual()` builtin and an
opt-in shell `run_command` and UTF-8 `open_text` editor. No worker integration,
Hub networking, end-to-end integration tests, or release artifacts are included
in this phase.

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

## Interactive manual testing

```sh
cd ptc
uv sync --locked
uv run simplex-ptc repl
# Or retain the test session across console restarts:
uv run simplex-ptc repl --state-directory /tmp/my-ptc-session
```

The default session is temporary. On console exit its daemon is stopped and its
lifetime lock released before the directory is removed. If cleanup cannot be
confirmed, the console reports and preserves the directory. An explicit directory
is retained; leaving that console does not stop the persistent daemon. Single
lines execute on Enter; `:paste` starts a multiline block and `:run` submits it. The block is executed as one snippet:

```text
ptc> manual()
ptc> print(manual("manual").render())
ptc> :paste
... info = manual("manual")
... print(info.signature)
... info.to_dict()
... :run
```

Use `:state` for session metadata, `:abort` to discard a block, `:help` for help,
and `:quit` or Ctrl-D to exit. Ctrl-C clears pending input or interrupts active
execution. Unsubmitted input is never executed on exit. The console labels
printed output, final results, truncation, errors, and committed revisions.

Each submission uses the normal persistent engine and a fresh Monty checkout:
plain variables survive, but host methods do not survive into the next
submission. Use a multiline block to obtain a ManualResult and call its methods,
or save its `to_dict()` result. The console never evaluates code with host Python
and never automatically retries, resets, or acknowledges interrupted executions.
Use the existing session CLI for explicit recovery/reset when needed. Machine
commands (`execute`, `capabilities`, `session`) retain their JSON output contract.

## State and failure semantics

Each state directory is one logical session managed by an automatically started
local daemon. CLI/REPL calls connect through a private Unix domain socket; no
systemd setup or manual service startup is required. The daemon owns admission,
checkpoint publication, recovery, and reset. Each invocation still uses a fresh
isolated runner and Monty checkout restored from the previous successful idle
dump. Supported globals and functions persist. Host handles, processes and
callbacks are not restored from checkpoints. The interpreter does not mount host
filesystems.

The directory contains a daemon lifetime `daemon.lock`, `session.lock`, a single
`checkpoint` (metadata, opaque dump and most recent response), and, while executing, `execution.pending`.
Concurrent execution/reset fails with `busy`. Inspection and cancellation remain
responsive while synchronous callbacks run in the isolated runner. Lock files
are never removed; process exit releases the OS lock. A second daemon cannot
own the same directory, including when clients resolve it through path aliases.
State files are private data in a trusted operator-owned directory, not a snapshot import interface. Checksums detect
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

SIGTERM/Ctrl-C in an executing client requests correlated cancellation; a plain
client disconnect does not cancel or replay admitted work. The runner attempts
normal cleanup before the daemon's cancellation deadline. Force termination or
SIGKILL cannot guarantee cleanup; pending records identify unresolved execution
on restart. External effects are never inferred from the restored heap.
Never infer external effect rollback from the old checkpoint. A future worker
must keep this directory separate from its conversation and compact archives.

## Automatic daemon management

Keep using `--state-directory`; it selects persistent state, not a socket pathname.
Execute, REPL, inspect, recover and reset connect to the same daemon. Inspection
may start a daemon but does not create a checkpoint or repair an interrupted
execution. `capabilities` is a local probe and does not start a daemon.

```sh
simplex-ptc daemon status --state-directory /path/to/session
simplex-ptc daemon stop --state-directory /path/to/session
simplex-ptc session query --state-directory /path/to/session --execution-id ID --epoch EPOCH
simplex-ptc session cancel --state-directory /path/to/session \
  --execution-id ID --epoch EPOCH --generation GENERATION
```

`status` exposes the daemon generation, lifecycle, effective deadlines and active
execution. `status` and `stop` never start a missing daemon. Use the original
request epoch for query/cancel; omit it if the initial request had no epoch.
Cancellation additionally requires the generation from daemon status, so a stale
request cannot target a replacement daemon. Reset changes the durable session
epoch; daemon restart changes the separate live-resource generation.

`execute` submits once and polls its correlated result. A lost connection reports
an uncertain outcome; it never silently resubmits source. `session query` returns
`running`, a terminal response, or an explicit unknown/recovery error. Only the
active request and latest outcome are available, not an execution history.
Repeated active requests with identical source/context do not start another
runner. Exact retries of the latest durable request retain the existing receipt
semantics. Querying an unknown old ID never executes source. Do not reuse IDs.

The current CLI's working directory, environment and effective umask are transmitted
for each execution and applied only in its runner. Daemon and runner bootstrap use
`/`, so removing the first caller's temporary directory cannot break later calls
from a valid directory. Internal startup preserves the owner's resolved module
search paths, including relative extension locations, while leaving environment
values such as `PYTHONPATH` unchanged. A later CLI call does not inherit the first
caller's cwd/environment/umask. The mask is captured without changing the client process's
global umask; the daemon retains its own private mask for internal files. Context
has a separate 1 MiB limit and participates in request identity; changing it,
including the umask, while retrying an ID is an identity conflict.
Keep the original context when reconciling a request. Environment values are not
included in daemon status or diagnostics. Plugins, runtime compatibility, and
lifecycle policy must match the running daemon; mismatch fails instead of
restarting it or discarding resources. Stop an idle daemon explicitly before
changing its configuration. Existing checkpoint compatibility checks still apply.
Compatible idle checkpoints from the pre-daemon interpreter remain readable, but
old receipts lack the new execution-context digest: do not retry an old execution
ID as a new daemon request. Inspect its receipt/state and continue with a new ID;
source is not replayed to migrate state.

Defaults are 15 minutes without execution/live resources, or 1 hour of unattended
retention with future managed live resources. Admitted executions and explicit
resource use renew activity; completion starts a fresh unattended interval.
Polling status, a connected socket, or resource output does not keep a daemon
alive. Running executions are exempt from idle expiry and retain their own
runtime/tool limits. Operator flags go before the subcommand, for example:

```sh
simplex-ptc --idle-seconds 300 --resource-seconds 1800 --cleanup-seconds 10 \
  repl --state-directory /path/to/session
```

Use the same flags on execution/inspection clients of that daemon. Management
operations (`daemon status/stop`, `session query/cancel`) require matching protocol
and directory identity but do not import plugins or require their old configuration;
this permits stopping an old daemon after a plugin/configuration change. Management
connections cannot execute, reset or recover state. Deadlines must be positive
and finite. Shutdown closes admission first, interrupts active execution, attempts
all owned resource cleanup, joins runners, and releases its lock. Cooperative
runner/resource cleanup defaults to 10 seconds; transport/startup overhead can
add time. A stuck runner is force-terminated with an unknown-effects outcome,
not a rollback guarantee. A failed storage fence leaves recovery information.
Stopping a daemon retains its checkpoint and does not delete user files.

Current builtins register **no persistent resources**: file editors still close
at each execution boundary; `run_command` remains a fresh shell with its existing
kill/detach contract. In particular, detached commands are not made daemon-owned
or automatically killed when the daemon stops. There is no persistent shell or
managed process tool in this change. The internal `Resource`/`Resources` interface
in `lifecycle.py` provides generation-scoped IDs, explicit use, liveness, and
cooperative asynchronous cleanup for future adapters. Cleanup must respect
cancellation/deadlines; it cannot block the control loop. Resources are never
embedded in a checkpoint, and old-generation IDs fail with `resource_lost`.

Socket paths are derived in a private `simplex-ptc` directory below a validated
`XDG_RUNTIME_DIR`, or a private `simplex-ptc-UID` directory in the system temporary
directory. State and runtime directories must be owned by the current user and
private (mode 0700); endpoint/lock files are private too. Long state-directory
paths do not need to fit the Unix socket pathname limit. A different runtime
root cannot bypass the durable lifetime lock: a locked but unreachable owner
produces an error, never unsafe takeover. Do not remove lock files or replace
session directories while a daemon is running. Mixed use with older pre-daemon
package versions is unsupported.

Persistent-directory setup synchronizes newly created directory entries before
execution can commit. Each new component has a private empty
`.simplex-ptc-create-UID-HASH.pending` intent in its parent before creation; the
intent is removed after the parent sync succeeds. An interrupted or failed setup
can leave an intent that the next startup resumes, even if the directory already
exists. Existing ancestors without pending creation work need only path traversal
permission; startup does not require opening or synchronizing every ancestor up
to `/`. Failure of a required fence prevents successful startup/commit. Ephemeral
runtime-directory setup does not require these persistence fences.

Internal control uses versioned, length-prefixed JSON, bounded frames and at most
32 concurrent connection handlers. Partial frames and blocked writes have a
10-second deadline. Source, execution-context, public response and private
checkpoint-transfer limits are separate. Daemon/runner standard streams are
not inherited from the submitting client; their raw diagnostics are discarded,
while bounded structured failures return through the CLI. Sandbox `print()`
continues to be captured in the execution response. No new log archive grows
without a bound.

For Python embedding, `Client` provides the same daemon operations. The low-level
`Session` remains available for package tests/direct embedding, but acquires the
lifetime lock and refuses concurrent access to a daemon-owned directory. Only
the daemon coordinator may bypass that reacquisition. Trusted configured host
extensions and same-user clients are not isolated from one another by the socket;
the Monty sandbox does not restrict arbitrary host callbacks.

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

## Function manual and typed results

```python
# No arguments returns all ManualResult objects, sorted by name.
for info in manual():
    print(f"{info.name}: {info.summary}")
info = manual("manual")  # one wrapped ManualResult
print(info.render())  # complete Python-style documentation
info.signature  # member access
saved = info.to_dict()  # detached, persistent plain data
manual(["manual"])  # list containing one wrapped result
manual("manual", "manual")  # list, preserving order and duplicates
manual([])  # empty list
```

No arguments returns all registered functions as a list of ManualResult.
One string returns one object; a list always returns a list. Multiple positional
strings are equivalent to a list. Mixed list/positional inputs and non-string
names raise `TypeError`; any unknown name raises `ValueError` for the entire
query. There is no search, pagination, or implicit printing. Looking up a tool
never invokes its handler or imports an unregistered module.

Each `ManualResult` is a host object explicitly wrapped with Monty's
`ClassInstance`. Its exposed fields are flat: `name`, `summary`, `signature`,
`description`, `inputs`, `returns`, `return_attributes`, `return_methods`,
`raises`, `effects`, `notes`, and `examples`. Values are strings or tuples of
strings, not nested proxy objects. Signatures include defaults and types; the
return description includes exposed attributes, method signatures and lifecycle
constraints for any returned ClassInstance. `render()` returns the complete
formatted description. `to_dict()` returns independent plain data, converting
sequence fields to lists. Those are the only exposed methods; use
`print(info.render())` for readable printing rather than relying on proxy repr.

A fresh ManualResult used as the program's final result is automatically converted
to plain data, including within a list or dictionary, before the ordinary JSON
output budget is applied. Arbitrary objects' conversion methods are never called.
**The host instance is not restored by session persistence.** Saving `info` in a
global does not make its methods usable on a later invocation. Persist
`info.to_dict()` / `info.render()`, or call `manual()` again after restoring.
Returning an old restored host proxy as the final result fails conversion instead
of silently claiming it is a live ManualResult.

ToolDescriptor supplies both the capability manifest and the manual. Optional
flat fields `details`, `inputs`, `return_attributes`, `return_methods`, `raises`,
`notes`, and `examples` enrich its existing signature/result/effects fields.
Sequence fields must be tuples of nonempty strings. All documentation participates
in the compatibility fingerprint. Adding the manual changes that fingerprint:
pre-manual experimental checkpoints are incompatible and are not silently reset
or migrated; explicitly select a new state directory if needed.

## Shell commands: kill or detach

Enable the packaged process module explicitly (before the subcommand):

```sh
uv run simplex-ptc --module simplex_ptc.tools.process repl
```

The default registry still exposes only `manual()`. Enabling/removing modules
changes the session compatibility fingerprint; use a separate state directory
when testing a different capability set. Inspect the complete function and
return-object documentation with `print(manual("run_command").render())`.

```python
result = run_command("printf hello; printf warning >&2")
print(result.stdout)
print(result.stderr)
result.to_dict()

run_command("sleep 10", timeout_seconds=0.1, timeout_action="kill")

run_command(
    "python3 job.py </dev/null >job.log 2>&1",
    timeout_seconds=0,
    timeout_action="detach",
)
run_command("tail -n 40 -- job.log")
```

Use a single `:paste` block for obtaining an object and calling its methods in
the REPL. The signature is:

```python
run_command(
    command: str,
    *,
    working_directory: str | None = None,
    timeout_seconds: float = 60.0,
    timeout_action: str = "kill",
) -> CommandResult
```

The host executes `/bin/bash -c`, falling back to `/usr/bin/bash`, then `/bin/sh`.
Shell syntax depends on the selected shell. Environment variables are inherited;
use shell syntax to customize them. stdin is `/dev/null`; no TTY is allocated.
Relative working directories use the host's current directory. This is arbitrary
host code with host permissions, not a filesystem/process sandbox or an approval
mechanism. Monty restrictions do not constrain shell effects.

- **kill:** the finite wait expires, the still-running process group receives
  SIGKILL, and the direct child is reaped. The result reports `status="killed"`
  and `timed_out=True`. This is an execution timeout, unlike the native worker
  tool's initial-wait timeout. The default is 60 seconds; accepted range (0, 300].
- **detach:** if waiting expires, return `status="detached"` and `timed_out=False`
  without terminating the command. Zero means immediate return; accepted range
  [0, 300]. A command finishing during the wait returns `status="exited"` instead.
  The result is a snapshot, not a live query. No eventual exit status is collected
  for the caller.

Process creation and bounded pipe cleanup can add latency beyond the requested
wait. An ordinary nonzero exit is returned as data, not raised. Launch/IO failures
raise exceptions. Kill-mode handled interruptions attempt termination/reaping;
detach-mode interruptions or later Python failures do not kill the command.
Side effects are not rolled back if the Python checkpoint fails.
Handled SIGINT/SIGTERM are briefly deferred during main-thread process acquisition
until the returned child handle is inside the cleanup scope. The original handler
then runs, so a launch interruption cannot bypass kill-mode cleanup. This does
not block signals in the child or change inherited ignored/default dispositions.

**Detach closes this tool's stdout/stderr capture pipes on return.** If a program
continues writing to those pipes, it can receive SIGPIPE, BrokenPipeError/EPIPE,
lose output, fail operations or terminate prematurely. There is no promise that
an arbitrary unmodified program survives detach. Redirect stdin/stdout/stderr
explicitly, or use a session manager such as tmux/screen to own the background
job. Manage its logs, output, lifetime and later inspection yourself through
shell commands. The returned PID is the shell/direct-child PID at launch; it can
be reused and is not a durable identity or a safe later kill target by itself.

For shell commands there is **no task registry, status/log directory, supervisor,
polling/control API, process restoration, or automatic record retention**. There is also no
`spawn_process`. The only background housekeeping is a daemon wait thread to
reap the direct child while the invocation runner is alive; it neither signals the
child nor keeps Python alive. A fresh POSIX session avoids inheriting the host's
terminal session, but container/service cgroups, system shutdown and application
behavior can still end the command. SIGKILL of the Python host cannot execute
kill-mode cleanup; descendants that escape the process group are not contained.

Output capture retains at most 64 KiB **combined** across stdout and stderr while
continuing to drain excess data. Streams remain separate, with no invented
cross-stream ordering. `CommandResult` exposes flat `status`, `pid`, `exit_code`,
`termination_signal`, `timed_out`, `duration_seconds`, `stdout`, `stderr`,
`output_complete`, per-stream `*_omitted_bytes`, and `hints`. It provides only
`to_dict()` and `render()`, using the same ClassInstance lifetime rules as the
manual. A fresh final result automatically converts to plain data.

`output_complete` means EOF was observed on both captured pipes, independently
of truncation. Omission counts cover observed discarded bytes only; they cannot
count output after detach. Invalid UTF-8 and cut multibyte sequences are replaced
and reported. If an exited shell leaves descendants holding its pipe writers,
EOF draining lasts at most 0.1 seconds before capture is closed and reported as
incomplete; those descendants may then encounter the same pipe failures.
Redirect potentially large or long-lived output to files yourself. Final JSON
responses remain subject to the independent 48 KiB envelope budget.

## Chunked file reading and explicit editing

Enable the file module explicitly; it adds only `open_text`:

```sh
uv run simplex-ptc --module simplex_ptc.tools.files repl
# Both optional modules can be selected in the same invocation:
uv run simplex-ptc --module simplex_ptc.tools.process --module simplex_ptc.tools.files repl
```

Use one `:paste` block for a complete interaction with a file object:

```python
file = open_text("example.py", writable=True)
part = file.read_lines(start=20, count=10)
print(part.render())
file.replace("old_value", "new_value")
print(file.preview(context_lines=3).render())
file.commit().to_dict()
```

`open_text(path, *, writable=False)` returns a wrapped host `TextFile`. Default
access is read-only. It opens an existing regular file, not a directory, FIFO,
device or final-component symlink. Relative paths use the host working directory;
intermediate symlinks and `..` are resolved by the filesystem in that order, not
lexically collapsed. The absolute lookup spelling is retained for display and
conflict checks. This is not a filesystem sandbox. The original path and `size_bytes` are exposed
as data attributes. The methods are:

| Method | Semantics |
| --- | --- |
| `read_lines(start=0, count=200)` | Selected original logical lines; LF, CRLF and CR are preserved. |
| `read_bytes(start, count)` | Selected original UTF-8 bytes, with strict decoding. |
| `replace(old_text, new_text)` | Stage one unique exact UTF-8 byte match; overlapping occurrences also count. |
| `replace_range(start_byte, end_byte, text)` | Stage an original `[start_byte, end_byte)` range replacement. |
| `insert(byte_offset, text)` | Stage insertion at an original UTF-8 boundary, including EOF. |
| `preview(context_lines=3)` | Bounded before/after text, aligned zero-based line numbers, `-`/`+` changed-line markers. Accepts 0–20 context lines. |
| `commit()` | Validate and atomically publish the complete staged edit plan, then close. |
| `discard()` / `close()` | Idempotently discard pending edits and close; never save. |

Indices are zero-based. Edits always refer to the version opened, so earlier
staged insertions do not shift later edit coordinates. Reads still show the
original file; preview shows the prospective result. Empty replacement text
means deletion. Overlapping edits, two insertions at the same position, or an
insertion touching another edit's start/end boundary are rejected instead of
relying on implicit ordering. Adjacent nonempty replacement ranges are allowed.

Reads return a flat `TextSlice` with `text`, `start_byte`, `end_byte` (exclusive),
`start_line`, `end_line` (exclusive), and `truncated`. Line fields are null for
byte reads. A partial last line counts in `end_line`; byte positions describe
only the text actually returned. Reads past EOF return an empty fragment at EOF.
`render()` returns the selected plain text, and `to_dict()` returns detached data.
Always check `truncated` before using a fragment's returned range as an edit:
a budget-cut fragment does not represent the entire requested range.

Preview returns a flat `EditPreview(text, edit_count, truncated)`. It has
`render()` and `to_dict()`, as does every data result. Long lines and excessive
hunks are explicitly truncated; this never truncates the staged edits themselves.
EOF insertions/deletions use an explicit `[EOF]` anchor where needed. Preview is
not required before commit and does not implicitly publish anything.

Commit returns a flat `CommitResult(path, changed, edit_count,
original_size_bytes, size_bytes)`. No staged edits means no file rewrite. A
successful commit closes the editor; reopen for another edit rather than reuse
stale offsets. Only explicit `commit()` writes the target. Python failure after
a successful commit **does not undo the committed file**; put commit after all
checks. Separate file commits are not a multi-file atomic transaction.

### Failure and resource ownership

Any failed file operation invalidates **all still-open file editors** in the
current invocation and discards their staged changes. Catching a match/range/IO
error does not make earlier staged edits eligible for commit. This includes
missing/extra entry-point arguments and argument conversion rejected by the
registry before the file handler runs. Explicitly reopen
to start a new batch. Already committed files are unaffected. At invocation exit,
normal or exceptional, remaining editors close and discard without autosave.
There is no undo stack, cross-invocation file handle or automatic recovery.

TextFile is a host ClassInstance, not persistent data. Returning an editor as the
final result is a conversion error, not an implicit save. Save a fragment,
preview or receipt with `to_dict()` for use after session restore. Host methods
on an old editor cannot be resumed in a later call. Cleanup errors prevent a new
Python checkpoint from being acknowledged as successful.

### UTF-8, streaming and limits

This interface is for UTF-8 text, not arbitrary binary buffers. Invalid selected
text or byte ranges splitting a code point fail explicitly; there is no lossy
replacement decoding. Output-budget cuts alone trim an incomplete final code
point. Every edited source byte, including deleted regions, is validated as
UTF-8 before publication. Opening alone does not scan the whole file.

- Maximum source and resulting file: 1 GiB.
- Reads: at most 64 KiB of UTF-8 text per result; preview is also bounded.
- At most 16 simultaneously open files and 128 staged edits per file.
- Aggregate replacement data: 1 MiB per PTC invocation; closing/discarding releases
  its budget. Exact-match patterns are at most 64 KiB.
- Original data is read with bounded `pread` chunks; even a huge single line is
  never accumulated in full. No whole-file buffer or per-byte/line index is built.
- One unique-match search followed by commit scans source bytes twice; commit
  also streams one destination write. Explicit previews may add before/after
  scans, and repeated independent queries/searches naturally add their own IO.

### Publication contract

The editor pins the parent directory and file descriptor, checking device/inode,
size, timestamps and metadata before/after operations and immediately before
publication. Path replacement, parent retargeting and detected concurrent writes
invalidate the batch. This is optimistic conflict detection, **not** an atomic
compare-and-swap against an uncooperative writer: a final check-to-rename race
still exists.

Commit streams into a unique same-directory temporary file, preserves owner,
group and ordinary permission bits, synchronizes it, replaces the target via the
pinned directory descriptor, and synchronizes the directory. Writable opens
reject multiple hard links, special permission bits and extended attributes/ACLs
rather than silently losing unsupported metadata. The temporary is also checked
for inherited extended attributes/ACLs immediately after creation at mode 0600,
before writing staged content or restoring permissions. Unsupported inherited
metadata causes rejection and temporary cleanup; the target is unchanged. This
can reject editing an ACL-free file inside a directory with a default ACL.
Read-only access has no such metadata-preservation restriction.

Failure before publication leaves the original target untouched and attempts to
remove the temporary. Once replacement is attempted, an error explicitly says
publication **may already have occurred**; inspect before retrying. A post-rename
sync error is never described as rollback. Forced process termination can leave a
`.simplex-edit-*` temporary beside the file; there is no automatic save or replay.

## Extensions

Only `manual()` is built in by default. Install trusted modules into the same
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
their effects. Initial callbacks are synchronous and accept plain JSON-compatible arguments
(string-keyed objects, arrays, strings, finite numbers, booleans and null; tuples
become arrays). Results may also use the explicitly supported `ManualResult`, `CommandResult`,
`TextSlice`, `EditPreview`, or `CommitResult` ClassInstance wrappers. `TextFile`
is an invocation-owned callable handle and is not a serializable final result. Arbitrary host objects and wrappers remain rejected.
Nesting is limited to 128 levels. They must settle their own resources before returning, must not
write directly to protocol fd 1, and must not retain live resources in returned
values. The file module uses the invocation-local resource scope; it does not
persist host handles. `registry.add(descriptor, handler, on_error=cleanup)` can
register synchronous failure cleanup covering boundary validation, argument
binding, handler execution and result conversion. Cleanup must not publish
side effects; cleanup failures are attached without hiding the original error.
The file module uses this boundary to discard pending edits on rejected inputs. Async callbacks remain outside this implementation.

## Development

```sh
uv run --locked pytest
uv run --locked ruff check .
uv run --locked ruff format --check .
```

Tests exercise only this package, real Monty, local UDS/daemon/runner processes
and temporary storage, with fixture callbacks. They cover concurrent startup,
cancellation, disconnects, owner death, pending recovery and bounded lifecycle
policies, and stop test daemons on teardown. There is no worker/Hub end-to-end
integration. The independent PTC workflow runs only for `ptc/**` or its own workflow; native CI classifies those
paths as independent. Changes to the shared CI classifier still conservatively
run native validation. No Python dependency is added to the C++ build.
