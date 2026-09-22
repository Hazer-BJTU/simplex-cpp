# process

Child-process management for the core tree: spawning executables (PATH
resolution, argv, inherited environment), feeding their stdin, collecting
stdout/stderr incrementally, waiting with optional timeouts (kill or
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
pipe wiring, pid + start-time stamps; failures
there throw `process::ProcessException`, never a boost type. Afterwards
strand-driven coroutines feed stdin through a thread-safe channel, drain
stdout/stderr incrementally under the spec's shared output cap (the
readers keep draining past the cap so the child never blocks; the result
reports which streams were truncated), and race the spec's initial-wait
deadline (kill or detach; a restarted watcher records the aftermath
either way). The handle must live in a `shared_ptr` — the background
tasks keep it alive until the child's terminal state is observed; see the
class comment for the full lifetime contract. Tests: the exception
contract plus the handle lifecycle against harmless coreutils (echo /
false / cat / env / sleep / seq, plus temp-dir fixtures for executable
resolution and for the spec-supplied PATH); richer
fixtures follow in the container build. Consumers link the `process_iface`
INTERFACE target. A destructive-labelled suite
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
