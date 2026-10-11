# Process I/O environments

`process_io_lib` provides `ProcessIoEnvironment`, the shared `CaptureBuffer`,
and the `AnonymousPipeIo` and Linux `PosixPtyIo` backends. `ProcessHandle`
selects pipes by default or PTY for `LaunchSpec::tty`, retaining child lifecycle policy. PTY is available to
host code through `ProcessIoEnvironment` and `subprocess::Child`; the tools expose TTY selection and generic output through the same lifecycle.

## Ownership and execution

An environment owns input admission, queued writes, output pumps and retained
bytes. It does not own a child process, reap it, apply runtime deadlines, assign
session IDs, authorize tools or format model output. The lifecycle owner decides
when to stop a child and how long to drain descendant-held output pipes.

Construct the environment with the **same strand** used for child lifecycle and
session observation. Awaitable methods marshal work to that strand, including
calls from another io_context. `prepare_attachment()` and `snapshot()` are
synchronous operations. `snapshot()` requires its strand; attachment preparation
also permits exclusive construction before publication, when no task can access
the environment. `capture()` exposes borrowed const storage for strand-side or
fully quiescent observation. A borrowed snapshot and any session cursor updates must be consumed without suspension;
copy bytes explicitly when they must survive later capture mutations.

The normal sequence is:

1. Construct `AnonymousPipeIo` or `PosixPtyIo` and call `prepare_attachment()` once, either on
   its strand or during exclusive construction before publishing the object.
2. Launch `subprocess::Child` with that attachment. The environment must outlive
   the borrowed attachment and the synchronous launch.
3. Await `start()` once after successful launch to supervise all pumps (three for pipes, two for PTY).
4. Enqueue input, request input-end when appropriate and inspect snapshots.
5. Stop admission, stop/reap the child according to caller policy, and optionally
   `drain_until(deadline)` before awaiting `cancel_and_join()`.

Launch failure and unused environments still support `cancel_and_join()`.
Partial pump startup rolls back and joins already scheduled work before throwing.
Keep the facade alive throughout outstanding method calls and its executor
context alive through cleanup. Destruction dispatches defensive cancellation,
but **is not a join fence**. Do not stop the context before awaiting cleanup.

## Input contract

`Accepted` means ownership entered the queue, not delivery or child consumption.
The strand defines queue order; input-end follows all previously accepted bytes.
The queue currently has no strict memory quota; callers must bound their input.
An input accepted before shutdown can be discarded during forced cleanup.

`stop_admission()` rejects subsequent input after its await completes, without
closing output or requesting EOF. `request_input_end()` also closes admission,
then drains accepted input and closes pipe stdin. `cancel_input()` instead
discards pending input and cancels the writer while output continues draining;
the lifecycle owner uses it before signalling a child during forced shutdown. `EndRequested`, `Ended`,
`PeerClosed`, `Stopped` and `Failed` distinguish the outcome. Broken stdin is
normal peer closure, not a fatal pump error. On POSIX, `subprocess::PipeWriter`
prevents SIGPIPE from terminating the host without changing the process-wide
signal disposition or holding a thread signal mask across an await.

## Output and completion

Capabilities describe separate stdout/stderr versus one terminal stream,
terminal identity, input-end semantics and resize support. Pipes expose separate
streams, half-close and no resize. PTY exposes one terminal stream, conditional
terminal EOF and host resize.
Absent streams are `nullopt`; an existing stream can contain an empty string.
A terminal stream is not duplicated into stdout and stderr.

Capture retains raw bytes, including NUL, partial UTF-8 and ANSI sequences.
It does not decode text. One budget applies to all retained bytes in an
environment: stdout plus stderr for pipes, or terminal output alone. Zero is
unlimited; the default is 4 MiB **total**, not 4 MiB per stream. This intentionally
changes the earlier per-stream policy. Labels, escaping and allocator/task
metadata are not counted; this is not a bound on total process memory.

The first read to consume remaining capacity keeps those bytes. The split
between stdout and stderr depends on read scheduling; no fixed shares or exact
cross-stream ordering are promised. Each stream retains its own prefix. Only
actual discarded bytes mark budget truncation on that stream; exactly filling
the budget or receiving an empty chunk does not. EOF and full/delta reads do not
refund retained capacity. Separate environments have separate budgets.

Reaching the limit stops retention, **not drainage**: both readers continue to
EOF so full pipes do not block the child. Forced shutdown/failure can separately
mark capture incomplete even when the budget was not reached.

Each stream distinguishes open capture, natural EOF, forced stop and failure.
Forced stop/failure marks possibly incomplete capture as truncated, even if no
retained prefix reached its size limit. `output_complete` means every present
stream ended; it does not mean lossless output, child exit or joined tasks.
`joined` becomes true only at the explicit cleanup fence.

`drain_until()` permits natural capture until a deadline. Timeout returns false
without closing transports; cancellation interrupts the wait without implicitly
shutting down the environment. `cancel_and_join()` shields cancellation, closes
admission and transports, discards undelivered input, and joins every owned pump.
The first unexpected failure is retained in `Snapshot::failure` and rethrown
**after** joining; repeated cleanup rethrows the same saved failure. Callers must
still finish their own child/session cleanup before propagating that error.

## Build and validation

CMake installs `libsimplex_process_io.so` through the normal shared-library
collection, with the common Asio runtime and `$ORIGIN` library search path.
Public C++ interfaces require rebuilding hosts and plugins together.

`process_io_capture` tests raw capture and stream state. `process_io_pipe` tests
ordering, EOF, truncation with continued drain, blocked writes, cancellation,
failed launch, cross-context calls, destructor cleanup and SIGPIPE preservation.
Linux-only `process_io_failures` injects read/write errors to verify first-error
retention and cleanup-before-rethrow. Fixtures use a controlled child executable.

## Linux terminal behavior

`PosixPtyIo` creates a real controlling terminal, with child stdin, stdout and
stderr attached to one slave. Output is the **merged raw terminal stream**:
stdout/stderr cannot be separated later. The host remains outside the child's
new session. Initial settings are canonical input, signal processing enabled,
echo disabled, 80 columns by 24 rows, and `TERM=dumb`. Explicit `TERM=...` in the
launch environment overrides that default, including with environment
inheritance disabled. Parent TERM is not used as the terminal default.

Canonical input can buffer bytes until a line delimiter; terminal drivers can
transform bytes (for example LF output becomes CRLF), process control keys and
limit canonical line length. Programs may change these settings. Input is not a
transparent binary pipe. Output is retained without stripping escape sequences,
answering queries, interpreting redraws or emulating a terminal.

Input-end is queued after accepted bytes. At execution, the writer checks the
current line discipline, canonical mode and EOF character, including conflicting
control-character/translation settings. Supported settings send two current EOF
characters: one releases a pending unterminated line, the next requests an empty
read. This follows the conditional [termios EOF contract](https://man7.org/linux/man-pages/man3/termios.3.html).
No master close or child signal implements this operation. `input_end_requested`
means queue acceptance; `Ended` means the request was written, **not** a physical
half-close, consumption or guaranteed child exit. A child can change settings
concurrently, quote control characters or ignore EOF.

If the actual mode cannot support EOF, `Snapshot::input_phase` becomes
`Unsupported`; output stays open and the child is not killed. Unexpected I/O
errors become `Failed`, populate `failure`, and are rethrown after join. Inspect
these asynchronous results even when queue admission returned `Accepted`.
`cancel_input()` closes only the independently owned write descriptor. The
master read descriptor remains alive to capture trailing output.

Host-only `resize(columns, rows)` accepts each dimension in 1–65535 and lets the
kernel notify the foreground process group with SIGWINCH. It is not a model
parameter. Direct-child termination remains a separate host action; sending
terminal control characters has foreground-group semantics instead.

Linux master `EIO` is normalized to EOF only after successful launch and a
confirmed slave hangup. EIO with a live slave remains a failure. Child exit is
not output completion: descendants can retain the slave. The host must choose a
bounded drain policy, then close/join if necessary. Final master closure can
hang up surviving slave holders; it is not a process-tree termination guarantee.

The PTY backend requires Linux with `TIOCGPTPEER` (kernel 4.13+) and a usable
devpts mount. Other platforms fail explicitly instead of falling back to pipes.
macOS/BSD allocation, controlling-terminal and hangup behavior need separate
implementations; future ConPTY support belongs behind the same opaque attachment
and capability boundaries, not POSIX descriptors in the public environment API.

`process_io_pty` covers real terminal identity, deterministic defaults, prompts,
ordered/custom/unsupported EOF, resize, merged retention, blocked writes,
closed-standard-descriptor launch and failed-exec cleanup. `process_io_pty_failures`
injects allocation, child setup and live-slave read/write failures and verifies
resource cleanup and error reporting after the task fence.
