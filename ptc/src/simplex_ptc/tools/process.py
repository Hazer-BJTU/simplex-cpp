"""Invocation-local command wrappers; live processes and capture belong to the daemon."""

from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass

from pydantic_monty import ClassInstance

from ..protocol import PTCError
from ..transport import MAX_COMMAND_REPLY_BYTES, pack, receive

_channel = ContextVar("command_channel", default=None)


@contextmanager
def command_channel(channel):
    """Bind the private runner channel only for this invocation, never to a dump."""
    token = _channel.set(channel)
    try:
        yield
    finally:
        _channel.reset(token)


def _call(operation, **arguments):
    """Make one synchronous request over this invocation's private capability channel."""
    channel = _channel.get()
    if channel is None:
        raise RuntimeError("command tools require a daemon execution; use Client or the CLI")
    channel.sendall(pack(dict(arguments, operation=operation)))
    value = receive(channel, MAX_COMMAND_REPLY_BYTES)
    if "error" in value:
        raise PTCError(value["error"]["code"], value["error"]["message"])
    return value["result"]


@dataclass(frozen=True)
class CommandResult:
    """Flat immutable command observation, with separate capture/read/input outcomes."""

    id: str
    status: str
    pid: int
    exit_code: int | None
    termination_signal: int | None
    timed_out: bool
    duration_seconds: float
    stdout: str
    stderr: str
    output_complete: bool
    output_truncated: bool
    stdout_end: str
    stderr_end: str
    stdout_omitted_bytes: int
    stderr_omitted_bytes: int
    stdout_cursor: int
    stderr_cursor: int
    read_limited: bool
    input_bytes_written: int
    input_complete: bool
    released: bool
    hints: tuple[str, ...]

    def to_dict(self) -> dict:
        """Detach observation data for persistence; this does not retain a process."""
        from dataclasses import asdict

        value = asdict(self)
        value["hints"] = list(self.hints)
        return value

    def render(self) -> str:
        """Display status and separate streams without repeating command source."""
        lines = [f"id: {self.id}; status: {self.status}; exit_code: {self.exit_code}"]
        lines.extend(self.hints)
        if self.stdout:
            lines.extend(["[stdout]", self.stdout])
        if self.stderr:
            lines.extend(["[stderr]", self.stderr])
        return "\n".join(lines)


def _result(value):
    """Rebuild only the known flat observation type, never arbitrary host objects."""
    value["hints"] = tuple(value["hints"])
    return ClassInstance(
        CommandResult(**value),
        eager_attrs=tuple(CommandResult.__dataclass_fields__),
        allowed_methods={"to_dict", "render"},
    )


@dataclass(frozen=True)
class Command:
    """Invocation-local wrapper around a generation-scoped daemon ID.

    Persist only id; get_command(id) creates a fresh wrapper in the next call.
    Multiple wrappers share the daemon's consuming stdout/stderr byte cursors.
    """

    id: str

    def status(self) -> ClassInstance:
        """Observe current exit/capture state without consuming output."""
        return _result(_call("status", id=self.id))

    def read(self, *, full: bool = False, max_bytes: int | None = None) -> ClassInstance:
        """Read bounded text; full observes the retained prefix without moving cursors."""
        return _result(_call("read", id=self.id, full=full, max_bytes=max_bytes))

    def wait(self, *, timeout_seconds: float = 60.0) -> ClassInstance:
        """Wait for exit and capture settlement; timeout never kills or reads output."""
        return _result(_call("wait", id=self.id, timeout_seconds=timeout_seconds))

    def send(
        self, text: str = "", *, close_stdin: bool = False, timeout_seconds: float = 5.0
    ) -> ClassInstance:
        """Write bounded UTF-8 input; inspect input_complete/bytes before retrying."""
        return _result(
            _call(
                "send",
                id=self.id,
                text=text,
                close_stdin=close_stdin,
                timeout_seconds=timeout_seconds,
            )
        )

    def stop(self) -> ClassInstance:
        """Kill a still-running process group, settle capture, and retain its record."""
        return _result(_call("stop", id=self.id))

    def release(self) -> ClassInstance:
        """Release only an exited command; a live command returns released=False."""
        return _result(_call("release", id=self.id))


def _handle(identity):
    """Expose only id and explicit command methods on a fresh invocation-local proxy."""
    return ClassInstance(
        Command(identity),
        eager_attrs=("id",),
        allowed_methods={"status", "read", "wait", "send", "stop", "release"},
    )


def run_command(
    command: str,
    *,
    working_directory: str | None = None,
    timeout_seconds: float = 60.0,
    timeout_action: str = "keep",
) -> ClassInstance:
    """Start a new shell and initially wait; keep leaves it owned by the daemon."""
    result = _call(
        "launch",
        command=command,
        working_directory=working_directory,
        timeout_seconds=timeout_seconds,
        timeout_action=timeout_action,
    )
    return _handle(result["id"])


def get_command(command_id: str) -> ClassInstance:
    """Reacquire an existing command; never launch source or resurrect a PID."""
    result = _call("get", id=command_id)
    return _handle(result["id"])


def register_tools(registry) -> None:
    """Expose launch and reacquisition separately; do not expose spawn_process."""
    from ..extensions import ToolDescriptor

    methods = (
        "status() -> CommandResult: latest state, no output consumption.",
        "read(*, full: bool = False, max_bytes: int | None = None) -> CommandResult: incremental text advances shared raw-byte cursors; full reads observe the retained prefix without moving cursors. max_bytes is an optional aggregate 1..65536 byte budget; default 16384 for delta reads and all retained bytes for full reads.",
        "wait(*, timeout_seconds: float = 60.0) -> CommandResult: wait for child exit AND capture settlement, at most 300 seconds; does not consume output or kill on timeout.",
        'send(text: str = "", *, close_stdin: bool = False, timeout_seconds: float = 5.0) -> CommandResult: send up to 64 KiB UTF-8, optionally close stdin after all bytes are written. Check input_complete and input_bytes_written; partial writes are effects, not safe to replay blindly.',
        "stop() -> CommandResult: SIGKILL a running process group and settle/reap; keep output/record for later read.",
        "release() -> CommandResult: exited commands are closed and removed, even if capture was incomplete; running commands return released=False. Unread/later output is lost.",
    )
    notes = (
        "Each launch is a fresh shell (/bin/bash, /usr/bin/bash, /bin/sh); no TTY or persistent cd/export environment. stdin remains a writable pipe until explicitly closed or the command exits.",
        "64 KiB shared stdout/stderr PREFIX retention, not a rolling tail. Capture continues draining/discarding after capacity is exhausted. Reads do not refund capture capacity. Redirect large/full output to files.",
        "read() consumes shared byte cursors across all wrappers; full=True does not consume. Invalid UTF-8 is replaced, incomplete growing suffixes wait for more bytes, EOF or truncation. Streams remain separate without invented inter-stream ordering.",
        "output_complete means both captures ended (EOF, stopped or failed), not lossless output. output_truncated also covers forced/failed closure. read_limited indicates this read's byte budget, separately from capture loss and final PTC JSON preview limits.",
        "A child can exit while descendants hold pipes. wait may expire with status=exited and output_complete=False. stop/release/shutdown use bounded drain then close pipes; descendants may get SIGPIPE/EPIPE.",
        "Command is not a persistent ClassInstance and cannot be a final result: return command.id or a CommandResult/to_dict(). Reacquire by get_command(id) next invocation. IDs are session/daemon-generation scoped; no recovery after daemon restart.",
        "keep commands survive PTC completion, errors and cancellation of waits. kill applies to initial timeout or interruption of the initial wait. No unmanaged detach mode. Explicit stop, daemon shutdown or retention expiry ends owned commands. Escaped descendants are not contained.",
        "commands_created in execution results/queries identifies launches even when Python state fails to commit. Never replay source just to reacquire a handle. At most 16 unsettled commands, 64 retained records and 64 launches per invocation; release exited records to free capacity.",
    )
    attributes = (
        "Command.id: str — persist this ID, not the wrapper or a PID.",
        "CommandResult.id/status/pid/exit_code/termination_signal: identity and current running/exited/killed snapshot.",
        "CommandResult.timed_out/duration_seconds: wait expiry and elapsed lifetime; keep expiry is not failure.",
        "CommandResult.stdout/stderr: selected decoded text; empty on non-read operations.",
        "CommandResult.output_complete/output_truncated, stdout_end/stderr_end (open/eof/stopped/failed), stdout_omitted_bytes/stderr_omitted_bytes: capture outcomes; omitted counts only cover observed discarded bytes.",
        "CommandResult.stdout_cursor/stderr_cursor: shared consuming raw-byte offsets; read_limited: requested bytes remain or a growing UTF-8 suffix was deferred.",
        "CommandResult.input_bytes_written/input_complete: this send's progress; released: record removal succeeded; hints: tuple[str, ...].",
        "CommandResult.to_dict() -> dict and render() -> str: detached data/readable result. Result methods are invocation-local; fresh final results convert to plain data.",
    )
    for name, handler, signature, description, inputs, examples in (
        (
            "run_command",
            run_command,
            'run_command(command: str, *, working_directory: str | None = None, timeout_seconds: float = 60.0, timeout_action: str = "keep") -> Command',
            "Start a daemon-owned shell command; keep it running when initial waiting expires.",
            (
                "command: nonempty shell source without NUL.",
                "working_directory: optional path relative to this invocation's cwd; caller environment and umask apply.",
                "timeout_seconds: finite initial wait in [0, 300]; kill requires >0. Process creation/cleanup adds latency.",
                'timeout_action: "keep" (default) or "kill". Neither initial wait nor subsequent wait consumes output.',
            ),
            (
                'c = run_command("python task.py", timeout_seconds=1)\ncommand_id = c.id\nprint(c.read().render())\ncommand_id',
            ),
        ),
        (
            "get_command",
            get_command,
            "get_command(command_id: str) -> Command",
            "Reacquire a command from this daemon without launching it again.",
            (
                "command_id: previously returned generation-scoped ID; released/foreign/old IDs fail.",
            ),
            ("c = get_command(command_id)\nprint(c.read().render())\nc.status().to_dict()",),
        ),
    ):
        registry.add(
            ToolDescriptor(
                name=name,
                description=description,
                signature=signature,
                result="Invocation-local Command ClassInstance; methods return flat CommandResult ClassInstances.",
                effects="Commands execute arbitrary host code; input, consumed cursors and process effects are not rolled back by Python errors.",
                version="2",
                details="Processes and buffers belong to the daemon, independent of Monty checkout lifetime.",
                inputs=inputs,
                return_attributes=attributes,
                return_methods=methods,
                raises=(
                    "ValueError/PTCError: invalid arguments, unavailable command, capacity or IO error. Partial effects may already exist.",
                ),
                notes=notes,
                examples=examples,
            ),
            handler,
        )
