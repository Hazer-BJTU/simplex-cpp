# subprocess platform utilities

`subprocess_lib` (`libsimplex_subprocess_platform.so`) contains platform
mechanisms shared by process management and asset loaders. Consumers include
`subprocess/child.hpp` or `subprocess/platform.hpp`. Boost.Process and native
process objects are private implementation details. Asio is consumed through
`asio_iface`, preserving one shared reactor/runtime across hosts and plugins.

## Ownership and execution

`Child` owns the native child process. It consumes a `process::LaunchSpec` data
record but has no dependency on the high-level `process_lib` lifecycle manager.
Its constructor preserves environment merge/validation, explicit executable
path semantics, child-PATH search and parent-PATH fallback when the child has
none. Launch errors retain stage, executable and description through
`ProcessException` (also aliased as `process::ProcessException`).

`IoAttachment::pipes()` borrows the caller's Asio pipe objects for synchronous
launch. The opaque implementation contains the native launcher attachment;
callers do not pass tuples of descriptors. Moving an attachment transfers that
representation; attempting to launch with the moved-from object is rejected.
The pipes remain caller-owned. This attachment is a launch bridge, not the
`ProcessIoEnvironment` abstraction, and must not be retained beyond the
lifetime of its borrowed objects.

Serialize `running()`, `wait()`, `terminate()` and `request_exit()` on the owning
executor. `running()` can reap; `wait()` propagates coroutine cancellation to
the native wait. There must be only one active exit observer. The high-level
manager retains its periodic probe for the Boost.Process pidfd arming race.
No deadline, capture, input queue or session cursor is implemented here.

`emergency_cleanup()` performs best-effort synchronous direct-child kill/reap
and returns an error code without throwing. It requires a quiescent executor
and joined worker threads. It cannot join suspended asynchronous tasks and
must not replace the normal manager `shutdown()` fence. Its repeated calls use
owned process state, never a caller-supplied PID. Destruction retains native
process ownership cleanup as a final safeguard. This is not process-tree
termination and does not guarantee cleanup if the OS rejects resource actions.

## Platform helpers

`platform_shell()` returns an executable plus an argument prefix. On the
currently supported Linux platform it chooses a regular file at `/bin/bash`,
then `/usr/bin/bash`, otherwise `/bin/sh`, with `{"-c"}`. This preserves existing
behavior, including failure at launch if that selected file is not executable;
it does not consult a model-controlled PATH or add interactive flags.

`executable_location()` delegates to `utils/executable` to locate the running
application, even inside this shared library or under the bundled musl loader.
Native launches retain the Boost.DLL fallback. The error-code overload enables asset
loaders to preserve their fallback order: explicit environment override,
installed directory beside the executable, then build-time directory. The
throwing overload serves configuration loading where location is mandatory.

## Portability and packaging

The public process ID is unsigned 64-bit diagnostic data with zero meaning
unassigned; no public native PID, fd or Boost.Process type is required. These
boundaries reserve room for a different child attachment (for example ConPTY),
but this change does not deliver Windows or macOS/BSD process support. Linux PTY mechanisms are implemented here; macOS/BSD allocation, terminal setup
and EOF differences still need separate implementations. Linux EIO normalization
is specific to a successfully launched PTY with a confirmed slave hangup.

CMake's derived release target collection installs this shared library to
`lib/`, with the normal `$ORIGIN` library RPATH. Boost.Process and Boost.DLL are
private link dependencies. Public class/type changes require rebuilding C++
hosts and dynamic plugins together; no v0.4 binary compatibility is promised.

## Safe pipe writes

`PipeWriter` supplies the anonymous-pipe environment with ordered asynchronous
byte writes. Before launch, `launch_pipe()` supplies the borrowed attachment;
after launch, `prepare()` transfers its ownership to the platform writer.
The caller serializes operations and keeps write buffers alive until completion.
Closing the writer cancels pending readiness waits; owners must still join them.

On POSIX, each nonblocking write temporarily blocks SIGPIPE on the calling
thread, consumes a newly generated signal on EPIPE, and restores the original
mask. Already-pending signals and the process-wide disposition are preserved.
No signal mask is held across coroutine suspension. Broken stdin is an error
code, not termination of the worker. Native descriptors remain private.

## Linux PTY launch and transport

`Terminal` owns a nonblocking master, an independently cancellable duplicate for
writing, and a temporary slave. `attachment()` borrows it for `Child` launch.
The slave closes in the parent on success **and on any launch/validation failure**;
retaining an attachment cannot keep output alive. Allocation uses
`posix_openpt`, `grantpt`, `unlockpt`, then Linux `TIOCGPTPEER` to open the exact
peer without static slave-name buffers or pathname races. All native ownership
is RAII-managed and descriptors are CLOEXEC. Resources are relocated above 2 so
closed host standard descriptors cannot collide with terminal remapping.

Pipe launch still uses the normal Boost.Process initializers. PTY uses a private
fork/exec adapter and adopts the successfully launched child into the same
Boost.Process object used for wait, signal and reap. This is deliberate: the
inspected Boost 1.91 default launcher creates its error pipe before initializer
setup, calls `on_exec_setup` in initializer order, then closes non-whitelisted
handles and execs. Its private error-pipe descriptor can be 0, 1 or 2; a terminal
initializer's `dup2` would overwrite it when host stdio is closed. Hook-only
integration cannot relocate that private channel reliably.

The PTY adapter creates a CLOEXEC error channel above stdio. It materializes all
arguments and environment in the parent. Child setup uses native operations
only: create a session, acquire the controlling terminal, set its foreground
process group, duplicate the slave to 0/1/2, change directory, mark remaining
descriptors CLOEXEC and exec. It does not allocate, log, use Asio, or acquire C++
locks after fork. Stage plus errno travels back through the error channel;
failure kills/reaps the child before throwing, including failure to adopt it.
`ProcessException` retains the native error code and launch context.

On recent Linux kernels, `close_range(CLOSE_RANGE_CLOEXEC)` prevents inheriting
unrelated host descriptors. Older kernels use native `getdents64` on `/proc/self/fd` plus `fcntl`; this
fallback requires procfs and also covers descriptors above a lowered resource
limit without scanning a potentially enormous limit. The parent descriptor table, terminal,
TERM, signal dispositions and session/group membership are not modified.
As with any fork-based host, third-party atfork handlers remain the host's
responsibility. No shell wrapper, `script` subprocess, or new process-tree
manager is involved.

The transport supports raw asynchronous read/write, current-mode EOF inspection,
validated host resize and independently cancelled input. It does not own input
queues, output retention, coroutine supervision or child lifecycle policy; these
remain in the environment and host. Destruction requires all outstanding native
operations to have completed, so use the environment's join fence.
