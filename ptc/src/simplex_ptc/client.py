"""Transparent local daemon client; uncertain submissions are never replayed."""

import os
import subprocess
import threading
import time
import uuid
from dataclasses import asdict
from pathlib import Path

from .bootstrap import module_command
from .endpoint import Endpoint, durable_directory
from .extensions import ToolRegistry
from .lifecycle import Policy
from .protocol import PTCError, Request, encode
from .runtime import compatibility
from .transport import VERSION, caller_umask, execution_context, pack, receive


class Client:
    """A session selector, not a persistent connection or a resource ownership lease."""

    def __init__(
        self, directory: Path, modules: list[str] | None = None, policy: Policy | None = None
    ):
        self.endpoint = Endpoint(directory)
        self.modules = modules or []
        self.policy = policy or Policy()
        self.compatibility = None
        self.generation = None

    @staticmethod
    def check(value: dict) -> dict:
        """Raise structured boundary failures while leaving execution failures as data."""
        if "boundary_error" in value:
            response = value["boundary_error"]
            error = response["error"]
            failure = PTCError(error["code"], error["message"])
            failure.response = response
            raise failure
        return value

    def connect(self, *, management: bool = False):
        """Handshake binds the transport to this directory, runtime and policy."""
        if not management and self.compatibility is None:
            self.compatibility = compatibility(ToolRegistry.load(self.modules))
        sock = self.endpoint.connect()
        try:
            sock.sendall(
                pack(
                    {
                        "version": VERSION,
                        "management": management,
                        "directory": str(self.endpoint.directory),
                        "identity": self.endpoint.identity(),
                        "modules": self.modules,
                        "compatibility": self.compatibility,
                        "policy": asdict(self.policy),
                    }
                )
            )
            value = self.check(receive(sock))
            if value.get("version") != VERSION or not isinstance(value.get("generation"), str):
                raise PTCError("daemon_incompatible", "invalid daemon handshake")
            self.generation = value["generation"]
            return sock
        except BaseException:
            sock.close()
            raise

    def ensure(self):
        """Start only before submission. Losing simultaneous starters never steal ownership."""
        try:
            return self.connect()
        except (FileNotFoundError, ConnectionRefusedError):
            pass
        durable_directory(self.endpoint.directory)
        process = subprocess.Popen(
            module_command("simplex_ptc.daemon", str(self.endpoint.directory)),
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
            cwd="/",
        )
        try:
            process.stdin.write(encode({"modules": self.modules, "policy": asdict(self.policy)}))
            process.stdin.close()
        finally:
            # Reap a child if this Python client stays alive; a short-lived CLI
            # does not keep itself alive merely to wait for its daemon.
            threading.Thread(target=process.wait, daemon=True).start()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                return self.connect()
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.025)
        raise PTCError(
            "daemon_startup",
            "daemon did not become ready; check directory ownership, runtime and existing owner",
        )

    def call(self, operation: str, *, start: bool = True, **fields) -> dict:
        """One framed operation; no transport retry after send starts."""
        try:
            sock = (
                self.ensure()
                if start
                else self.connect(management=operation in ("status", "stop", "query", "cancel"))
            )
        except (FileNotFoundError, ConnectionRefusedError):
            if operation in ("status", "stop"):
                # A missing endpoint is not proof of owner exit: a daemon might
                # be initializing or using a different runtime-directory setting.
                if self.endpoint.directory.exists():
                    with self.endpoint.locked():
                        pass
                return {"running": False}
            raise PTCError(
                "daemon_unavailable", "daemon unavailable; inspect before retrying"
            ) from None
        except (OSError, EOFError) as exc:
            raise PTCError(
                "daemon_unavailable", "daemon handshake failed; request was not submitted"
            ) from exc
        with sock:
            identity = str(uuid.uuid4())
            try:
                sock.sendall(pack(dict(fields, operation=operation, id=identity)))
                if operation == "stop":
                    sock.settimeout(self.policy.cleanup_seconds * 2 + 15)
                value = self.check(receive(sock))
                if value.get("id") != identity:
                    raise PTCError("invalid_frame", "response correlation failed")
                return self.check(value["result"])
            except (OSError, EOFError) as exc:
                raise PTCError(
                    "outcome_unknown", "connection lost; query execution before retrying source"
                ) from exc

    def inspect(self) -> dict:
        """Observe session metadata; this may start a daemon, never an execution."""
        return self.call("inspect")

    def execute(self, request: Request, *, context: dict | None = None) -> dict:
        """Submit once, then poll the correlated outcome without resending source."""
        context = execution_context(
            context
            if context is not None
            else {
                "cwd": os.getcwd(),
                "environment": dict(os.environ),
                "umask": caller_umask(),
            }
        )
        request = Request.parse(asdict(request))
        generation = None
        try:
            self.call("execute", request=asdict(request), context=context)
            generation = self.generation
            return self.wait(request)
        except KeyboardInterrupt:
            try:
                self.call(
                    "cancel",
                    start=False,
                    generation=generation or self.generation,
                    execution_id=request.execution_id,
                    epoch=request.epoch,
                )
            except PTCError as exc:
                if exc.code != "stale_execution":
                    raise
                # Completion may have won the cancellation race. Query the exact
                # request and report its real receipt; never claim rollback.
            return self.wait(request)

    def wait(self, request: Request) -> dict:
        """Poll only, never auto-start a replacement daemon after uncertain execution."""
        while True:
            value = self.call(
                "query", start=False, execution_id=request.execution_id, epoch=request.epoch
            )
            if value["status"] == "terminal":
                response = value["response"]
                if response.get("boundary_failure"):
                    error = response["error"]
                    failure = PTCError(error["code"], error["message"])
                    failure.response = response
                    raise failure
                return response
            time.sleep(0.05)

    def recover(self, epoch: str, execution_id: str) -> None:
        """Explicitly acknowledge interrupted effects, without rerunning source."""
        self.call("recover", epoch=epoch, execution_id=execution_id)

    def reset(self, epoch: str) -> None:
        """Start a new durable epoch only when the daemon is idle and recovered."""
        self.call("reset", epoch=epoch)

    def stop(self) -> dict:
        """Wait for cleanup and lifetime-lock release before callers delete their directory."""
        result = self.call("stop", start=False)
        deadline = time.monotonic() + self.policy.cleanup_seconds * 2 + 15
        while self.endpoint.directory.exists():
            try:
                with self.endpoint.locked():
                    return result
            except PTCError as exc:
                if exc.code != "daemon_busy":
                    raise
            if time.monotonic() >= deadline:
                raise PTCError(
                    "shutdown_timeout", "daemon ownership not released; preserve state directory"
                )
            time.sleep(0.025)
        return result
