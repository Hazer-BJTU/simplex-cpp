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

`LaunchSpec::executable` may be a bare name (`grep`) or a path
(`/usr/bin/grep`), and it is resolved in **two steps, in this order**:

1. **A path that exists is the executable.** Whatever the spec names — an
   absolute path, a relative one with a directory in it, or a bare name that
   happens to be a file of the host's working directory — if it is there, that
   is what runs. A caller that wrote a path meant *that* file, and substituting
   a same-named program from PATH would run something nobody asked for.
   Existence is what this step tests, not executability: a path that is there
   but cannot be executed (a directory, a file without the execute bit) fails
   at the launch with the OS's own error, which names the file the caller
   pointed at, rather than quietly becoming a different program.
2. **Otherwise its FILE NAME goes through PATH** — `path::filename()`, the last
   component — so `/usr/bin/grep` on a host whose grep lives elsewhere still
   finds that grep instead of failing on a layout that is not this machine's.
   A bare name is the same rule with nothing to strip.

The PATH searched is the one in the **child's assembled environment** (a
spec-supplied PATH is honoured, including under `inherit_environment=false`,
where the spec's PATH is the only one); when that environment carries no PATH
at all the search falls back to the **parent's** PATH — lookup only, the child's
own environment is unaffected. Nothing is found by either step: the launch fails
at `Stage::ResolveExecutable`, naming the executable and the spec's description.

Step 1 is done here rather than handed to Boost.Process, because
`environment::find_executable()` is a PATH search and nothing else: it appends
the name to each PATH entry with Boost.Filesystem's `operator/`, which
**concatenates rather than replacing**, so an absolute name is looked for as
`<PATH entry>/usr/bin/grep` and never found. Step 2 *is* that function, handed
the file name.

Two consequences worth knowing:

- a **relative** path is checked against the host's working directory, while the
  child is `chdir`'d into `working_directory` before `exec` — so the file this
  code finds and the file the child would resolve can differ. Name an absolute
  path when that matters;
- a bare name that matches a file in the host's working directory resolves to
  **that** file rather than to the one on PATH. That is step 1 applied
  uniformly, and it is the reason a bare name is not simply "a PATH lookup".
