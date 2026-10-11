# Process I/O environments

`process_io_lib` provides `ProcessIoEnvironment`, the shared `CaptureBuffer`,
and the `AnonymousPipeIo` backend. `ProcessHandle` delegates its I/O to this
package and retains child lifecycle policy. Aggregate capture limits and POSIX
TTY support follow separately in issue #122.

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

1. Construct `AnonymousPipeIo` and call `prepare_attachment()` once, either on
   its strand or during exclusive construction before publishing the object.
2. Launch `subprocess::Child` with that attachment. The environment must outlive
   the borrowed attachment and the synchronous launch.
3. Await `start()` once after successful launch to supervise all three pumps.
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
streams, half-close and no resize. A terminal backend is not implemented yet.
Absent streams are `nullopt`; an existing stream can contain an empty string.
A terminal stream will not be duplicated into stdout and stderr.

Capture retains raw bytes, including NUL, partial UTF-8 and ANSI sequences.
It does not decode text. A configured limit retains a prefix **per stream**;
zero is unlimited and the pipe default is 4 MiB per stream. Reaching the limit
marks truncation but continues draining, so full buffers do not block the child.
A later issue #122 stage changes this to an aggregate budget.

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
