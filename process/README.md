# process

Child-process management for the core tree: spawning executables (PATH
resolution, argv, inherited environment), feeding their stdin, collecting
pipe stdout/stderr or merged terminal output incrementally, waiting with optional timeouts (kill or
detach), and terminating stragglers — built on Boost.Process v2 running as
asio coroutines. It knows nothing about *what* the children are for; the
typed data contract it speaks lives in `dataclass/include/dataclass/
process_spec.hpp` (`process::LaunchSpec` / `ExecutionStatus` /
`ExecutionResult`).

**Status — the manager core has landed.** `process_lib` (libsubprocess.so)
carries `ProcessHandle`: one managed child per instance. Construction IS
the spawn — execve-style environment assembly as a MERGE by key (the
spec's `KEY=VALUE` entries override inherited ones; malformed entries
throw at `Stage::Environment`), executable resolution against that same
assembled environment ([see below](#how-the-executable-is-resolved)),
I/O attachment wiring, pid + start-time stamps. Child launch failures use
`process::ProcessException`; environment preparation can also throw transport
or platform-support errors. Afterwards
the handle delegates input ordering, pumps and raw capture to
`process_io::ProcessIoEnvironment`. Startup also schedules its independently
supervised child watcher. Initial waiting applies a grace-period timeout (kill
or detach), but is no longer necessary to start observation or perform cleanup.
The handle must live in a `shared_ptr`; the watcher retains it until exit or
shutdown. Tests: the exception
contract plus the handle lifecycle against harmless coreutils (echo /
false / cat / env / sleep / seq, plus temp-dir fixtures for executable
resolution and for the spec-supplied PATH); richer
fixtures follow in the container build. Consumers link the `process_lib`
shared-library target. A destructive-labelled suite
(`test_process_destructive`: 300-round fd/pid leak audit, a destruction
storm over live children, stdin-after-death handling) runs wherever the
environment is disposable — in the build container, or anywhere with
`SIMPLEX_DESTRUCTIVE_TESTS=1`; on a dev host it passes instantly and
`ctest -LE destructive` skips it outright.

## How the executable is resolved

`LaunchSpec::executable` distinguishes names from explicit paths:

- **Bare names** such as `grep` are searched only through PATH. A same-named
  file in the host working directory has no special priority; it can be found
  only if that directory is included in PATH.
- **Explicit paths** such as `./tool`, `../bin/tool`, or `/usr/bin/grep` identify
  that file only. A missing path fails at `Stage::ResolveExecutable`; it never
  falls back to a same-named program on PATH. An existing file that cannot be
  executed fails at `Stage::Spawn`.

Relative executable paths are resolved against the **host's working directory**,
not `LaunchSpec::working_directory`. The resolved executable is made absolute
before the child changes directories. Relative results from PATH lookup are
anchored the same way. `working_directory` controls the child's working directory
and does not change which executable was selected.

The PATH searched is the one in the **child's assembled environment** (including
an explicitly supplied PATH with `inherit_environment=false`). If that environment
has no PATH, lookup uses the parent's PATH without adding it to the child's
environment. An unsuccessful search fails at `Stage::ResolveExecutable`, with
the requested executable and description in the error.

Only bare names are passed to Boost.Process's `find_executable()`. Explicit paths
are resolved directly, avoiding its PATH-entry concatenation for absolute paths.

## Platform boundary

`utils/subprocess` owns the private Boost.Process object, environment merging,
executable resolution, spawn attachments, signal/reap calls, shell selection,
and executable-location lookup. `ProcessHandle` retains lifecycle policy,
strand serialization, initial-wait timeouts and the pidfd polling workaround.
`process_io` owns the pipe/terminal transport, input queue, reader/writer tasks
and captured bytes.
All three layers reuse one strand per child; no nested strand or duplicated
capture buffer is introduced. See the
[utility contract](../utils/subprocess/README.md).

Public identifiers use `process::ProcessId` (`std::uint64_t`, zero before
launch). A PID is diagnostic data; only an owned child can perform resource
actions. `ProcessException` remains available through its original header as
an alias of the utility exception, preserving stage/context diagnostics.
Public C++ layout and exception type identity have changed: rebuild hosts and
plugins together for v0.5; this is not binary-compatible with v0.4 plugins.

`emergency_cleanup()` is a synchronous, noexcept fallback delegated to the
owned child. Finish store calls, stop its executor and join worker threads
before using it. It may kill/reap but cannot join suspended coroutine tasks;
normal teardown must await `shutdown()` while the context runs. An atomic
`exited()` check is not a substitute for this quiescence requirement.

## Startup, input and cleanup

Construct the handle, await `start_background_io_tasks()`, then optionally await
`await_initial_execution()`. A supplied strand is reused by the handle and its
I/O environment, so session cursor consumption and capture updates serialize.
Construction may prepare an unpublished environment exclusively before a
context runs; all subsequent mutation is strand-owned. Observation is also
allowed after the context and all worker threads have quiesced.

`write_input()` and `close_input()` are now **awaitable** and return
`process_io::InputResult`. Await them in order. `Accepted` means queued, not
written or consumed; `Closed` rejects a new request. Pipe input-end follows all
accepted bytes. There is no second channel inside ProcessHandle and no new
pending-input memory quota. Direct C++ consumers must migrate former
fire-and-forget calls to `co_await`; model-facing tool arguments are unchanged.

Startup failure joins started pumps and terminates/reaps the child before
throwing. `shutdown()` works even if startup or the initial wait was never
called. It stops admission, signals the child, allows a bounded output drain,
then cancels/joins the environment and watcher before reporting the first
failure. A cancelled initial wait does not detach an unsupervised process:
the independent watcher remains active, and the owner can still shut it down.
Unexpected I/O failures are retained and reported by cleanup rather than only
logged. Expected broken stdin is a closed-peer outcome.

ProcessSessionStore reserves capacity before launch and publishes only after
startup. Failure during allocation/publication or cancellation during initial
waiting rolls back the child, tasks, table entry and capacity reservation before
propagating the original failure. Shutdown still excludes concurrent store
spawn/release calls; it attempts every session even if one cleanup fails.

`max_output_bytes` now limits stdout and stderr **together** per process. The
4 MiB default and zero/unlimited setting are unchanged, but two busy streams
can no longer each retain the full limit. Allocation follows read scheduling;
each stream keeps a prefix and reports its own discarded bytes. Both pipes
continue draining after exhaustion. This raw-data budget does not include
allocator/task overhead, and reads do not replenish it. `process_io::PosixPtyIo`
now supplies a Linux PTY backend for explicit host composition with
`subprocess::Child`. `ProcessHandle` selects this backend when `LaunchSpec::tty` is true; tools expose
`tty`, generic `output`, and `end_input` (see the process toolset README).
