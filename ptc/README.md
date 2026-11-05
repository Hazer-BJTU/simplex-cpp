# Simplex PTC interpreter

An independent, experimental Python/uv package built on pinned Pydantic Monty.
It executes a Python subset, preserves idle interpreter state across invocations,
and exposes an extension registry with the read-only `manual()` builtin and an
opt-in shell `run_command` and UTF-8 `open_text` editor. The optional worker
adapter is documented below; this package remains independently installable.
Direct Hub networking is not included. GitHub releases provide a separate source
archive; see [INSTALL.md](INSTALL.md) for complete standalone installation,
verification, worker configuration, and upgrade instructions.

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
Definite pre-evaluation refusals report `submitted: false`, `commit_status: "none"`
and an empty `commands_created`; uncertain failures report `commit_status: "unknown"`.
Both use exit 2. A matching error code alone does not prove non-submission, and
pending executions still require explicit recovery. Neither response replays code.

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

The process module registers daemon-owned commands; file editors still close at
each execution boundary. Commands and their retained output survive ordinary PTC
completion, errors and cancelled waits, within the resource-retention policy.
There is no persistent shell, process restoration, or cross-session access.
The internal `Resource`/`Resources` interface owns command-registry cleanup.
Resources are never embedded in a checkpoint; old-generation IDs fail explicitly.

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

## Managed shell commands

Enable the process module explicitly; the default registry still has only `manual()`:

```sh
uv run simplex-ptc --module simplex_ptc.tools.process repl --state-directory /path/to/session
```

The two entry points separate creation from lookup:

```python
run_command(command: str, *, working_directory: str | None = None,
            timeout_seconds: float = 60.0, timeout_action: str = "keep") -> Command
get_command(command_id: str) -> Command
```

Use a multiline REPL block for each example. Start an interactive command and
save its plain ID:

```python
command = run_command("cat", timeout_seconds=0)
command_id = command.id
command_id
```

In another PTC invocation, reacquire the wrapper without launching another process:

```python
command = get_command(command_id)
print(command.send("hello\n", close_stdin=True).render())
command.wait(timeout_seconds=2)
print(command.read().render())
command.release().to_dict()
```

The daemon owns the child, stdin pipe, stdout/stderr capture and read cursors.
The runner only receives a `Command` ClassInstance with an `id` attribute and
methods below. The ID is bound to this session's daemon generation, not an OS
PID. It remains usable across sandbox invocations, not across daemon restarts.
The host wrapper itself is invocation-local: save `command.id`, then call
`get_command` next time. A bare Command cannot be serialized as the final result;
return its ID or an observation instead. Unknown, foreign, released and previous
generation IDs fail; they never launch code or resurrect a PID.

| Method | Behavior |
| --- | --- |
| `status()` | Latest state; does not consume output. |
| `read(full=False, max_bytes=None)` | Incremental text advances shared byte cursors; full reads inspect the retained prefix without advancing them. `max_bytes` optionally sets an aggregate 1–65536 byte read budget; by default delta reads use 16 KiB and full reads return the entire retained capture. |
| `wait(timeout_seconds=60)` | Wait for exit **and** capture settlement, at most 300 seconds. Does not consume output or kill on expiry. |
| `send(text="", close_stdin=False, timeout_seconds=5)` | Write up to 64 KiB UTF-8 to stdin. Close stdin only after all requested bytes are written. Inspect partial-write fields before deciding what remains to send. |
| `stop()` | Kill a still-running process group, reap the direct child and settle capture; retain the record/output. |
| `release()` | Remove an exited command after bounded pipe cleanup; return `released=False` for a running child. Unread and later output is lost. |

These methods return flat `CommandResult` objects with `to_dict()` and `render()`.
Fresh final observations convert automatically to JSON-compatible data; their host
methods still belong to one invocation. Nonzero command exits are observations,
not Python exceptions. `manual("run_command")` and `manual("get_command")` include
full signatures, result fields, lifecycle limitations and examples.

### Launch and timeout policy

Each launch executes `/bin/bash -c` (fallback `/usr/bin/bash`, then `/bin/sh`) in a
new POSIX session, with this invocation's environment, umask and working directory.
An explicit relative `working_directory` is relative to the invocation cwd. Shell
syntax, redirection and permissions are ordinary host behavior, not a sandbox.
Commands do not share `cd`, exported variables or shell functions. No TTY or
`spawn_process` function is provided. stdin is now a writable pipe, **not**
`/dev/null`; use `send(close_stdin=True)` when a program needs EOF.

- `keep` is the default. Initial waiting accepts 0–300 seconds; expiry returns a
  handle and leaves the command running. Zero starts without waiting. This is
  a wait deadline, not a command execution limit or a failed execution.
- `kill` requires a positive wait up to 300 seconds. If waiting expires, terminate
  a still-running process group, reap and bound capture cleanup. Interruption of
  the initial kill-mode wait likewise cleans up that command. After initial
  completion, later handle operations have their own semantics.
- The old unmanaged `detach` policy is removed. Commands remain daemon-owned;
  there is no promise to survive daemon shutdown or retention expiry.

Both initial wait and `wait()` leave output unread. A command that exits quickly
still retains its record until explicitly released or the daemon expires. At most
16 commands may have unsettled process/capture state, and 64 records may be
retained (at most 4 MiB total capture). At most 64 commands may be launched in one
invocation so execution-effect metadata remains bounded. Capacity exhaustion
fails explicitly; release exited records rather than silently evicting live work.

### Capture, cursors and truncation

Capture follows the native worker's **shared bounded prefix** policy: retain up
to 64 KiB total across stdout and stderr, separately labelled. When full, keep
draining and discard new bytes. Reads never refund capacity; this is not a ring
buffer and does not keep the newest tail. No cross-stream ordering is invented.
Redirect potentially large or lossless output to files and read selected portions.

Each record owns shared stdout/stderr cursors, counted in raw bytes. `read()`
advances them across all wrappers and across PTC invocations. `full=True` returns
a bounded view from the start without consuming output; it cannot recover data
already discarded by the capture budget. Invalid UTF-8 is replaced. A growing,
incomplete suffix waits for more bytes, capture termination or truncation;
a read budget alone does not split a code point into replacement characters.
Increase `max_bytes` if it is too small for the next character.

`CommandResult` includes:

- `id`, `pid`, `status` (`running`, `exited`, `killed`), `exit_code`,
  `termination_signal`, `duration_seconds` and `timed_out`. Status observations
  retain initial-wait expiry; a `wait()` result reports that wait's expiry.
- `stdout`, `stderr`, shared `stdout_cursor`/`stderr_cursor`, and `read_limited`.
  Non-read operations return empty text. A read may be reduced further to fit
  the private transport envelope before its cursors advance.
- `output_complete` means both captures ended; `stdout_end`/`stderr_end` are
  `open`, `eof`, `stopped` or `failed`. Completion is not a lossless-output promise.
- `output_truncated` covers discarded bytes and forced/failed capture closure.
  Per-stream `*_omitted_bytes` count only observed discarded bytes; output after
  closure is unknowable.
- `input_bytes_written`, `input_complete`, `released` and concise `hints`.

`read_limited`, capture truncation and the final PTC JSON result budget are
separate. Inspect previews/truncation flags; use bounded reads and small selected
results. Output consumption and partial stdin writes are external effects and
are not rolled back by a subsequent Python failure or lost reply. `full=True`
can reread retained output without moving consuming cursors.

Successful `send()` results always count a complete UTF-8 prefix, even when
backpressure causes `input_complete=False`. Writes use character-aligned chunks
no larger than the pipe's `PC_PIPE_BUF`; the byte count is never rounded down
after an effect. To resume, send only
`text.encode("utf-8")[result.input_bytes_written:].decode("utf-8")`.
Do not slice the original string by a byte count or resend the complete input.
A zero timeout writes immediately available chunks until completion/backpressure
without waiting for capacity. `close_stdin=True` closes input only after the
entire supplied text was written. An exception or lost reply still requires
inspection rather than assuming a known resumable prefix.

A child may exit while descendants still hold output pipes. Capture continues
until EOF, explicit release/stop or daemon cleanup. `wait()` can expire with an
exited child but incomplete capture. Explicit cleanup allows 100 ms of additional
drain before closing remaining streams and marking them truncated. Those
closures may cause SIGPIPE/EPIPE in descendants. Signalling is confined to the
owned group while its unreaped leader still identifies it; escaped descendants
are not contained.

### Failure and lifecycle

Command creation is registered before initial waiting. Every execution response
and active execution query carries `commands_created`, including Python failures
that roll back the heap. Use these IDs to reacquire commands when a variable was
not checkpointed. Boundary failures remain queryable through `session query`;
only the active/latest execution outcome is retained, not an unbounded history.
Exact request retries return the receipt without launching again.

A failed program or cancellation of a keep-mode wait does not undo process/input
side effects or stop the command. `stop()` explicitly terminates it. Normal daemon
shutdown and unattended resource expiry stop all owned commands and settle
capture. SIGKILL/host crash cannot run cleanup and may leave external processes;
a replacement daemon never adopts them by PID or replays their source. Its old
command IDs are invalid. Checkpoints can retain IDs only as historical plain data.

The process registry is reachable only through a private invocation-owned runner
channel, not a new public daemon operation or network listener. The process module
must be enabled. Low-level direct `Session` evaluation cannot start managed
commands: use `Client` or the CLI so there is a daemon resource owner. Enabling
these revised descriptors changes the process-module compatibility fingerprint;
older experimental process-enabled checkpoints require a new state directory,
not silent migration or source replay.

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
`TextSlice`, `EditPreview`, or `CommitResult` ClassInstance wrappers. `TextFile` and `Command`
are invocation-owned callable handles and are not serializable final results. Arbitrary host objects and wrappers remain rejected.
Nesting is limited to 128 levels. Invocation-local resources must settle before
returning; explicitly managed commands instead remain owned by the daemon.
Callbacks must not write directly to protocol fd 1 or hide unowned live resources
in returned values. The file module uses the invocation-local resource scope; it does not
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
policies, and stop test daemons on teardown. These package-local tests do not
require a worker or Hub. A separate PTC worker workflow covers real integration.
The independent Python workflow runs only for `ptc/**` or its own workflow;
the ordinary native classifier keeps those paths independent. Changes to the shared CI classifier still conservatively
run native validation. No Python dependency is added to the C++ build.

### File result display formats

Reading and presentation are separate: formatting uses the bounded captured
result and never rereads the file. All indexes are zero-based.

```python
file = open_text("example.py", writable=True)
part = file.read_lines(10, 5)
print(part.render())  # Exact original text, including line endings
print(part.render(format="lines"))  # Aligned original line numbers
print(part.render(format="bytes"))  # Aligned [start:end) byte ranges
file.replace("timeout = 30", "timeout = 60")
print(file.preview().render())  # Default: numbered before/after sections
print(file.preview().render(format="bytes"))
file.discard()  # No write
```

- `TextSlice.render(format="plain")` accepts `plain`, `lines`, or `bytes`.
  Annotated formats use LF display separators, remove original line terminators
  from display text, and align coordinates across the slice. Byte ranges include
  the retained original terminators and count UTF-8 bytes, not characters.
  A truncated slice labels only returned bytes. `read_bytes()` supports plain
  and bytes; lines is rejected because byte reads do not scan the preceding
  file to discover its line number.
- `EditPreview.render(format="lines")` accepts the same formats. Plain removes
  coordinates but retains Before/After sections, change markers and gap notices.
  Before coordinates describe the original file; After coordinates describe
  the complete staged result. **After coordinates are never valid input edit
  offsets.** Clipped rows label only their retained source prefix, excluding
  the synthetic truncation notice; EOF anchors use an empty byte range.
- `EditPreview.text` and `to_dict()["text"]` keep the default numbered display.
  Internal preview rows are bounded and are not exposed in the serialized result.
  Result formatting remains available after the file handle is closed, within
  the same invocation. Invalid formats raise `ValueError`.

Every edit and exact-match search targets the opened original version.
For example, after staging `replace("old", "new")`, a subsequent
`replace("new", "final")` still searches the original file, not the staged
replacement. Stage the intended final replacement directly instead.

A failed file operation invalidates **all still-open editors in that invocation**,
even if Python catches the exception. Reopen files to start a new edit batch.
Already committed files remain committed. Rendering a detached result is a pure
display operation; a formatting error does not invalidate editors.

The file tool descriptor is version 2. Existing checkpoints made with its
previous interface have a different compatibility fingerprint; use a fresh
state directory rather than silently restoring an incompatible tool interface.

### Worker integration

The optional intrinsic `ptc_execute` adapter is documented in
[INSTALL.md](INSTALL.md#connect-a-worker). The interpreter remains a
separately installed program. Its isolated unit CI is retained; an additional
path-filtered worker integration workflow tests the real CLI/daemon contract.
