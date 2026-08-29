# process

Child-process management for the core tree: spawning executables (PATH
resolution, argv, inherited environment), feeding their stdin, collecting
stdout/stderr incrementally, waiting with optional timeouts (kill or
detach), and terminating stragglers — built on Boost.Process v2 running as
asio coroutines. It knows nothing about *what* the children are for; the
typed data contract it speaks lives in `dataclass/include/dataclass/
process_io.hpp` (`process_io::ProcessSpec` / `ProcessExecution` /
`ProcessReport`).

**Status — foundations only.** The exception contract
(`include/process/exceptions.hpp`, `process::ProcessException`) is the first
resident: the manager framework lands next and will translate Boost.Process
spawn failures into it, so no caller ever catches a boost type. When the
manager arrives this target gains `Boost::process` + `asio_iface`
dependencies (and the top-level `find_package` regains the `process`
component).

It is **header-only** today. Consumers link the `process_iface` INTERFACE
target.
