"""Isolated evaluation, without checkpoint access or inherited daemon descriptors."""

import base64
import contextlib
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

import pydantic_monty as monty

from .bootstrap import module_command
from .extensions import ToolRegistry
from .protocol import MAX_CHECKPOINT_BYTES, Output, PTCError, preview
from .runtime import compatibility
from .session import evaluate, failure_code
from .transport import MAX_FRAME_BYTES, pack, receive, unpack

MAX_RUNNER_BYTES = MAX_CHECKPOINT_BYTES + MAX_FRAME_BYTES + 128 * 1024


class Runner:
    """One daemon-owned evaluation; cancel() is safe from the control-loop thread.

    The parent sends input once, then only cancellation bytes. The child sends
    one result frame. EOF on the parent channel interrupts the child; a watchdog
    bounds cooperative cleanup after owner loss. No candidate is ever committed
    by the runner itself. A separate invocation-owned socket carries synchronous
    command requests to the daemon; it is never inherited by shell children.
    """

    def __init__(self, modules: list[str], context: dict, cleanup_seconds: float = 10):
        self.modules = modules
        self.context = context
        self.cleanup_seconds = cleanup_seconds
        self.cancelled = threading.Event()
        self.command_socket = None

    def cancel(self) -> None:
        """Request cooperative interruption; do not signal a PID from cached metadata."""
        self.cancelled.set()

    def __call__(self, source, state, registry, output, response):
        """Return a converted candidate or raise, leaving publication to Session."""
        if self.cancelled.is_set():
            raise PTCError("cancelled", "execution cancelled before runner start")
        parent, child = socket.socketpair()
        process = None
        try:
            command_fd = self.command_socket.fileno() if self.command_socket is not None else -1
            process = subprocess.Popen(
                module_command("simplex_ptc.runner", str(child.fileno()), str(command_fd)),
                pass_fds=(child.fileno(),) + ((command_fd,) if command_fd >= 0 else ()),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
                cwd="/",
            )
            child.close()
            if self.command_socket is not None:
                self.command_socket.close()
            parent.settimeout(10)
            parent.sendall(
                pack(
                    {
                        "source": source,
                        "state": base64.b64encode(state).decode() if state else None,
                        "modules": self.modules,
                        "compatibility": compatibility(registry),
                        "context": self.context,
                        "response": response,
                        "cleanup_seconds": self.cleanup_seconds,
                    },
                    MAX_RUNNER_BYTES,
                )
            )
            data = bytearray()
            expected = None
            cancel_at = None
            while expected is None or len(data) < expected + 4:
                if self.cancelled.is_set() and cancel_at is None:
                    parent.sendall(b"C")
                    cancel_at = time.monotonic()
                if cancel_at is not None and time.monotonic() - cancel_at > self.cleanup_seconds:
                    raise OSError("runner cancellation deadline; effects unknown")
                ready, _, _ = select.select([parent], [], [], 0.05)
                if not ready:
                    continue
                part = parent.recv(64 * 1024)
                if not part:
                    raise OSError("runner disconnected; effects unknown")
                data.extend(part)
                if expected is None and len(data) >= 4:
                    expected = struct.unpack("!I", data[:4])[0]
                    if expected > MAX_RUNNER_BYTES:
                        raise OSError("runner response exceeds budget")
            result = unpack(bytes(data[4:]))
            process.wait(timeout=self.cleanup_seconds)
            if process.returncode != 0:
                raise OSError("runner exited without a reliable outcome")
            if result.get("unknown"):
                raise OSError("runner cleanup or host failure; effects unknown")
            output.text = result["output"]
            output.omitted_bytes = result["output_omitted_bytes"]
            if "error" in result:
                raise PTCError(result["error"]["code"], result["error"]["message"])
            if self.cancelled.is_set():
                raise PTCError("cancelled", "execution cancelled before checkpoint publication")
            return result["response"], base64.b64decode(result["state"], validate=True)
        finally:
            parent.close()
            child.close()
            if process is not None:
                if process.poll() is None:
                    process.kill()
                process.wait()


def main(fd: int, command_fd: int = -1) -> int:
    """Evaluate trusted daemon input; a monitor keeps owner loss observable during callbacks."""
    channel = socket.socket(fileno=fd)
    channel.set_inheritable(False)
    commands = socket.socket(fileno=command_fd) if command_fd >= 0 else None
    if commands is not None:
        commands.set_inheritable(False)
    from .tools.process import command_channel

    completed = threading.Event()
    output = Output()

    def interrupt(_signal, _frame):
        raise KeyboardInterrupt("execution cancelled")

    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    value = receive(channel, MAX_RUNNER_BYTES)

    def monitor():
        try:
            control = channel.recv(1)
        except OSError:
            control = b""
        if completed.is_set():
            return
        os.kill(os.getpid(), signal.SIGINT)
        if not control and not completed.wait(value["cleanup_seconds"]):
            os._exit(2)

    try:
        threading.Thread(target=monitor, daemon=True).start()
        with contextlib.redirect_stdout(sys.stderr), command_channel(commands):
            os.umask(value["context"]["umask"])
            registry = ToolRegistry.load(value["modules"])
            if compatibility(registry) != value["compatibility"]:
                raise PTCError("incompatible_state", "runner runtime or tools changed")
            os.chdir(value["context"]["cwd"])
            os.environ.clear()
            os.environ.update(value["context"]["environment"])
            response, state = evaluate(
                value["source"],
                base64.b64decode(value["state"]) if value["state"] else None,
                registry,
                output,
                value["response"],
            )
        if len(state) * 4 // 3 + 128 * 1024 > MAX_CHECKPOINT_BYTES:
            raise PTCError("storage_limit", "snapshot exceeds checkpoint budget")
        result = {"response": response, "state": base64.b64encode(state).decode()}
    except (monty.MontyError, PTCError, KeyboardInterrupt) as exc:
        result = {"error": {"code": failure_code(exc), "message": preview(str(exc), 2048)}}
    except BaseException:
        result = {"unknown": True}
    completed.set()
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    result.update(output=output.text, output_omitted_bytes=output.omitted_bytes)
    try:
        channel.sendall(pack(result, MAX_RUNNER_BYTES))
    finally:
        channel.close()
        if commands is not None:
            commands.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else -1))
