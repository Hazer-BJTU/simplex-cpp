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
assembled environment (falling back to the parent's PATH when the child
environment carries none), pipe wiring, pid + start-time stamps; failures
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
false / cat / env / sleep / seq, plus a temp-dir PATH fixture); richer
fixtures follow in the container build. Consumers link the `process_iface`
INTERFACE target. A destructive-labelled suite
(`test_process_destructive`: 300-round fd/pid leak audit, a destruction
storm over live children, stdin-after-death handling) runs wherever the
environment is disposable — in the build container, or anywhere with
`SIMPLEX_DESTRUCTIVE_TESTS=1`; on a dev host it passes instantly and
`ctest -LE destructive` skips it outright.
