"""Shell execution with bounded capture and kill/detach timeout policies.

There is no task store, supervisor or persistent process identifier service.
Detached children are reaped by a daemon waiter while this host remains alive;
that waiter never captures output, sends signals or keeps Python alive.
"""

import math
import os
import selectors
import signal
import subprocess
import threading
import time
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from pathlib import Path

from pydantic_monty import ClassInstance

CAPTURE_BYTES = 64 * 1024
MAX_TIMEOUT_SECONDS = 300.0
DRAIN_SECONDS = 0.1
REAP_SECONDS = 5.0


@dataclass(frozen=True)
class CommandResult:
    """An immutable snapshot; neither its PID nor methods control a live task.

    Omission counts cover bytes actually read and discarded, not unknowable
    output written after capture closes. Host methods are invocation-local.
    """

    status: str
    pid: int
    exit_code: int | None
    termination_signal: int | None
    timed_out: bool
    duration_seconds: float
    stdout: str
    stderr: str
    output_complete: bool
    stdout_omitted_bytes: int
    stderr_omitted_bytes: int
    hints: tuple[str, ...]

    def to_dict(self) -> dict:
        """Return detached JSON-compatible data; no process handle is retained."""
        value = asdict(self)
        value["hints"] = list(self.hints)
        return value

    def render(self) -> str:
        """Keep status and stdout/stderr visibly separate without repeating source."""
        lines = [
            f"status: {self.status}; pid: {self.pid}; exit_code: {self.exit_code}; signal: {self.termination_signal}"
        ]
        lines.extend(self.hints)
        if self.stdout:
            lines.extend(["[stdout]", self.stdout])
        if self.stderr:
            lines.extend(["[stderr]", self.stderr])
        return "\n".join(lines)


class _Capture:
    """Nonblocking pipe draining with one aggregate retention budget."""

    def __init__(self, process: subprocess.Popen):
        self.selector = selectors.DefaultSelector()
        self.data = {"stdout": bytearray(), "stderr": bytearray()}
        self.omitted = {"stdout": 0, "stderr": 0}
        self.remaining = CAPTURE_BYTES
        try:
            for name in self.data:
                pipe = getattr(process, name)
                os.set_blocking(pipe.fileno(), False)
                self.selector.register(pipe, selectors.EVENT_READ, name)
        except BaseException:
            self.selector.close()
            raise

    @property
    def complete(self) -> bool:
        """True only after EOF was observed on both pipes, not merely child exit."""
        return not self.selector.get_map()

    def read_ready(self, timeout: float) -> None:
        """Read at most one chunk per ready stream so no producer starves deadlines."""
        for key, _ in self.selector.select(timeout):
            try:
                chunk = os.read(key.fd, 8192)
            except BlockingIOError:
                continue
            if not chunk:
                self.selector.unregister(key.fileobj)
                continue
            kept = min(self.remaining, len(chunk))
            self.data[key.data].extend(chunk[:kept])
            self.omitted[key.data] += len(chunk) - kept
            self.remaining -= kept

    def drain(self) -> None:
        """Bound EOF waiting when descendants still hold inherited writers."""
        deadline = time.monotonic() + DRAIN_SECONDS
        while not self.complete:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            self.read_ready(min(0.02, remaining))

    def close(self) -> None:
        """Close polling resources; caller owns the pipe handles."""
        self.selector.close()


def _reap_later(process: subprocess.Popen) -> None:
    """Only reap the direct child; do not supervise, terminate or retain output."""
    threading.Thread(target=process.wait, name="ptc-child-reaper", daemon=True).start()


def _kill_and_reap(process: subprocess.Popen) -> bool:
    """Kill the new process group while its unreaped leader still owns the PID."""
    signalled = False
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGKILL)
            signalled = True
        except ProcessLookupError:
            pass
    process.wait(timeout=REAP_SECONDS)
    return signalled


def _shell() -> str:
    """Match native shell preference; do not resolve the shell via user PATH."""
    for name in ("/bin/bash", "/usr/bin/bash", "/bin/sh"):
        if os.access(name, os.X_OK) and Path(name).is_file():
            return name
    raise FileNotFoundError("run_command requires /bin/bash, /usr/bin/bash or /bin/sh")


@contextmanager
def _defer_launch_signals():
    """Delay handled termination signals until Popen has returned an owned handle.

    Python handlers can raise after fork/exec but before Popen returns. Record
    SIGINT/SIGTERM during that acquisition window, restore the original handlers,
    then invoke them inside the caller's cleanup scope. No OS signals are blocked:
    children must not inherit a blocked signal mask. SIG_IGN and SIG_DFL are left
    unchanged, preserving inherited ignore behavior and unhandled termination.
    Python dispatches signal handlers only on the main thread, so other calling
    threads neither need nor are permitted to replace these handlers.
    """
    if threading.current_thread() is not threading.main_thread():
        yield
        return
    handlers = {}
    pending = {}

    def defer(signum, frame):
        # Coalesce repeated instances, like ordinary pending POSIX signals.
        pending.setdefault(signum, frame)

    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            handler = signal.getsignal(signum)
            if callable(handler):
                handlers[signum] = handler
                signal.signal(signum, defer)
        yield
    finally:
        for signum, handler in handlers.items():
            signal.signal(signum, handler)
        for signum, frame in pending.items():
            handlers[signum](signum, frame)


def run_command(
    command: str,
    *,
    working_directory: str | None = None,
    timeout_seconds: float = 60.0,
    timeout_action: str = "kill",
) -> ClassInstance:
    """Run shell source, then kill or release ownership when waiting expires.

    stdin is /dev/null, environment is inherited, and a new POSIX session is
    created. A detach result closes capture pipes immediately: callers must
    arrange redirection or a session manager if the command must survive that.
    An interrupted detach-mode invocation also leaves the command independent.
    """
    if not isinstance(command, str) or not command or "\0" in command:
        raise ValueError("command must be nonempty text without NUL")
    if working_directory is not None and (
        not isinstance(working_directory, str) or not working_directory or "\0" in working_directory
    ):
        raise ValueError("working_directory must be a nonempty path without NUL")
    if timeout_action not in ("kill", "detach"):
        raise ValueError("timeout_action must be kill or detach")
    if type(timeout_seconds) not in (int, float):
        raise ValueError("timeout_seconds must be a finite number")
    try:
        valid_timeout = (
            math.isfinite(timeout_seconds) and 0 <= timeout_seconds <= MAX_TIMEOUT_SECONDS
        )
    except OverflowError:
        valid_timeout = False
    if not valid_timeout or (timeout_seconds == 0 and timeout_action == "kill"):
        raise ValueError("timeout_seconds must be in (0, 300], or [0, 300] for detach")

    started = time.monotonic()
    process = None
    capture = None
    primary_failure = False
    try:
        with _defer_launch_signals():
            process = subprocess.Popen(
                [_shell(), "-c", command],
                cwd=working_directory,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
                close_fds=True,
            )
        capture = _Capture(process)
        deadline = started + timeout_seconds
        status = "exited"
        while process.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                if timeout_action == "kill":
                    status = "killed" if _kill_and_reap(process) else "exited"
                else:
                    status = "detached"
                break
            # With both pipes at EOF, select still sleeps for this bounded slice.
            capture.read_ready(min(0.02, remaining))
        if status != "detached":
            capture.drain()
        else:
            # Collect only immediately available bytes, never wait after detach.
            capture.read_ready(0)
        code = process.returncode
        texts = {}
        hints = []
        for name, data in capture.data.items():
            try:
                texts[name] = data.decode("utf-8")
            except UnicodeDecodeError:
                texts[name] = data.decode("utf-8", errors="replace")
                hints.append(f"{name}: invalid or cut UTF-8 bytes replaced.")
        if any(capture.omitted.values()):
            hints.append(
                "Capture truncated; redirect large output to a file and read selected portions."
            )
        if status == "detached":
            hints.append(
                "Detached: capture pipes close on return. Further writes may raise SIGPIPE/BrokenPipeError, fail, or end the program. Redirect stdin/stdout/stderr or use tmux/screen yourself."
            )
            hints.append(
                "PID is a startup identifier, not a tracked task. No final status or output is collected; containers/services can still terminate the process."
            )
        elif not capture.complete:
            hints.append(
                "Output capture closed before EOF; descendants holding pipes may encounter SIGPIPE or write errors. Redirect output for background work."
            )
        result = CommandResult(
            status=status,
            pid=process.pid,
            exit_code=code if code is not None and code >= 0 else None,
            termination_signal=-code if code is not None and code < 0 else None,
            timed_out=status == "killed",
            duration_seconds=time.monotonic() - started,
            stdout=texts["stdout"],
            stderr=texts["stderr"],
            output_complete=capture.complete,
            stdout_omitted_bytes=capture.omitted["stdout"],
            stderr_omitted_bytes=capture.omitted["stderr"],
            hints=tuple(hints),
        )
        return ClassInstance(
            result,
            eager_attrs=tuple(CommandResult.__dataclass_fields__),
            allowed_methods={"to_dict", "render"},
        )
    except BaseException:
        primary_failure = True
        raise
    finally:
        # Always close our pipe ends, including when capture construction fails.
        # Kill mode owns the child until settlement; detach mode deliberately
        # does not bind its lifetime to either this call or the Python process.
        try:
            if process is not None and timeout_action == "kill" and process.returncode is None:
                _kill_and_reap(process)
        except BaseException:
            if not primary_failure:
                raise
        finally:
            if capture is not None:
                capture.close()
            if process is not None:
                process.stdout.close()
                process.stderr.close()
                if process.returncode is None:
                    _reap_later(process)


def register_tools(registry) -> None:
    """Opt in using --module simplex_ptc.tools.process; no spawn/control API."""
    from ..extensions import ToolDescriptor

    registry.add(
        ToolDescriptor(
            name="run_command",
            description="Execute a shell command; kill or detach when waiting expires.",
            signature='run_command(command: str, *, working_directory: str | None = None, timeout_seconds: float = 60.0, timeout_action: str = "kill") -> CommandResult',
            result="CommandResult wrapped in ClassInstance. Nonzero exits and kill timeouts are returned as data; detach is not a failure.",
            effects="Executes arbitrary host shell code with inherited environment and host permissions. Effects are not rolled back by Python failures.",
            version="1",
            details="Uses /bin/bash, then /usr/bin/bash, then /bin/sh with -c. No TTY; stdin is closed (/dev/null). The shell supports pipes, expansion and redirection; syntax depends on the selected shell. Each launch starts a new POSIX session.",
            inputs=(
                "command: str — nonempty shell source without NUL; use shell syntax to configure environment, redirect files or manage sessions.",
                "working_directory: str | None = None — child's directory; relative paths use the host working directory. Not a sandbox boundary.",
                "timeout_seconds: float = 60.0 — finite wait deadline, at most 300 seconds. kill requires >0; detach permits 0 for immediate return. Process creation and bounded cleanup add latency.",
                'timeout_action: str = "kill" — kill: SIGKILL the running process group on expiry, then reap the direct child. detach: close capture and return without terminating the command.',
            ),
            return_attributes=(
                'status: str — "exited", "killed", or "detached"; a snapshot, not a live status.',
                "pid: int — shell/direct-child PID at launch; reusable by the OS and not safe as a durable control identity.",
                "exit_code: int | None — ordinary exit status; nonzero is not an exception.",
                "termination_signal: int | None — terminating signal, separate from exit_code.",
                "timed_out: bool — true only when kill policy expired; detached is not failure.",
                "duration_seconds: float — elapsed launch/wait/capture time.",
                "stdout: str; stderr: str — separate UTF-8 previews, invalid/cut bytes replaced; no invented inter-stream order.",
                "output_complete: bool — EOF observed on both streams; distinct from truncation. Never promises future detached output.",
                "stdout_omitted_bytes: int; stderr_omitted_bytes: int — observed discarded bytes only; future output is unknown.",
                "hints: tuple[str, ...] — truncation, decoding and pipe/lifecycle caveats.",
            ),
            return_methods=(
                "to_dict() -> dict: detached plain data for persistence.",
                "render() -> str: readable status and separated stdout/stderr.",
            ),
            raises=(
                "ValueError: invalid command, directory or timeout arguments.",
                "OSError: shell launch, pipe IO, signalling or cleanup failed; side effects may already exist.",
                "TimeoutExpired: a killed direct child could not be reaped within the cleanup window.",
            ),
            notes=(
                "64 KiB aggregate capture; readers continue draining/discarding after the limit. Redirect large output to a file yourself.",
                "Detach closes capture pipes: later writes may receive SIGPIPE, BrokenPipeError/EPIPE, fail, or terminate the program. Redirect all stdio before relying on survival.",
                "No task files, logs, supervisor, process registry, final status collection or restoration. Use shell commands or tmux/screen to inspect/control your own work.",
                "Detach-mode commands are not killed on interruption of the call or later Python failure. Host exit does not intentionally kill them; container shutdown, cgroups, OS shutdown and program behavior still apply.",
                "Kill mode cleans up on handled interruption. SIGKILL of the Python host cannot run cleanup. Descendants that escape the group are not contained.",
                "Exited shells may leave descendants holding pipes: bounded draining then closes capture with output_complete=false. No indefinite EOF wait.",
                "Result methods are host-backed and invocation-local. Persist to_dict(); final fresh results convert automatically to plain data.",
            ),
            examples=(
                'result = run_command("printf hello; printf warning >&2")\nprint(result.stdout)\nresult.to_dict()',
                'run_command("sleep 10", timeout_seconds=0.1, timeout_action="kill")',
                'run_command("python3 job.py </dev/null >job.log 2>&1", timeout_seconds=0, timeout_action="detach")',
                'run_command("tail -n 40 -- job.log")',
            ),
        ),
        run_command,
    )
