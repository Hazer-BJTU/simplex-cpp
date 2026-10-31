"""One session authority with responsive control and isolated synchronous execution."""

import asyncio
import contextlib
import os
import signal
import sys
import uuid
from dataclasses import asdict
from pathlib import Path

from .endpoint import Endpoint, check_peer
from .extensions import ToolRegistry
from .lifecycle import Policy, Resources
from .protocol import PTCError, Request, encode
from .runner import Runner
from .runtime import compatibility
from .session import Session, digest
from .transport import VERSION, execution_context, read_frame, unpack, write_frame


def error_response(exc: Exception) -> dict:
    """Do not leak environment values or arbitrary host diagnostics across control replies."""
    code = exc.code if isinstance(exc, PTCError) else "storage_or_interruption"
    message = (
        str(exc)
        if isinstance(exc, PTCError)
        else "inspect session before retrying; effects unknown"
    )
    return {
        "protocol": 1,
        "status": "failed",
        "commit_status": "unknown",
        "error": {"code": code, "message": message},
    }


class Daemon:
    """Event-loop-affine admission and resource owner; only evaluation uses a thread.

    Session.execute runs on one coordinator thread and is the only storage writer
    while active. Queries use its previous committed metadata, never the long-held
    storage lock. Admission/mutation decisions have no intervening await.
    """

    def __init__(self, endpoint: Endpoint, modules: list[str], policy: Policy):
        self.endpoint = endpoint
        self.modules = modules
        self.policy = policy
        self.registry = ToolRegistry.load(modules)
        self.session = Session(endpoint.directory, self.registry, daemon_owned=True)
        self.generation = str(uuid.uuid4())
        self.directory_identity = None
        self.resources = Resources(self.generation, policy)
        self.state = "ready"
        self.active = None
        self.runner = None
        self.task = None
        self.last = None
        self.info = None
        self.handlers = set()
        self.shutdown_task = None
        self.finished = asyncio.Event()

    def status(self) -> dict:
        """Expose lifecycle metadata without resetting idle deadlines or reading the heap."""
        return {
            "running": True,
            "state": self.state,
            "generation": self.generation,
            "policy": asdict(self.policy),
            "active": self.active
            and {
                "execution_id": self.active["request"].execution_id,
                "epoch": self.active["request"].epoch,
            },
        }

    def refresh(self) -> dict:
        """Read durable metadata only while no execution owns the session lock."""
        self.info = self.session.inspect()
        return self.info

    def query(self, identity: str, epoch) -> dict:
        """Resolve only the active/latest outcome; unknown IDs are never executed here."""
        if self.active and self.active["request"].execution_id == identity:
            if self.active["request"].epoch != epoch:
                raise PTCError("stale_epoch", "active execution epoch does not match")
            return {"status": "running", "execution_id": identity, "generation": self.generation}
        if self.last and self.last[0] == identity and self.last[1] == epoch:
            return {"status": "terminal", "response": self.last[2]}
        if self.active:
            raise PTCError("unknown_execution", "only the active or latest receipt is retained")
        # An observational query never repairs pending state or claims durability
        # after a failed fence. Such outcomes require exact-request receipt retry.
        with self.session.storage.locked():
            checkpoint = self.session._load()
            if self.session.storage.read("execution.pending") is not None:
                raise PTCError("recovery_required", "inspect pending execution before retrying")
            receipt = checkpoint and checkpoint.get("receipt")
            if receipt and receipt["execution_id"] == identity:
                if epoch is not None and checkpoint["epoch"] != epoch:
                    raise PTCError("stale_epoch", "execution epoch does not match")
                self.session.storage.sync_directory()
                return {"status": "terminal", "response": receipt["response"]}
        raise PTCError("unknown_execution", "only the active or latest receipt is retained")

    async def execute(self, request: Request, context: dict, runner: Runner) -> None:
        """Settle execution even if its submitting socket has already disconnected."""
        try:
            response = await asyncio.to_thread(
                self.session.execute, request, evaluator=runner, context=context
            )
        except Exception as exc:
            response = error_response(exc)
            response["boundary_failure"] = True
        self.last = (request.execution_id, request.epoch, response)
        self.active = None
        self.runner = None
        # Completion starts the unattended interval; queries do not extend it.
        self.resources.touch()
        try:
            self.refresh()
        except Exception:
            self.info = None

    def admit(self, value: dict) -> dict:
        """Validate identity/context before reserving the sole execution slot."""
        request = Request.parse(value.get("request"))
        context = execution_context(value.get("context"))
        key = digest(encode({"source": request.source, "epoch": request.epoch, "context": context}))
        if self.state != "ready":
            raise PTCError("stopping", "daemon is stopping; request was not admitted")
        if self.active:
            if self.active["request"].execution_id == request.execution_id:
                if self.active["digest"] != key:
                    raise PTCError("identity_conflict", "execution identity has different content")
                return self.query(request.execution_id, request.epoch)
            raise PTCError("busy", "session is already executing")
        self.refresh()
        runner = Runner(self.modules, context, self.policy.cleanup_seconds)
        self.active = {"request": request, "digest": key}
        self.runner = runner
        self.resources.touch()
        self.task = asyncio.create_task(self.execute(request, context, runner))
        return self.query(request.execution_id, request.epoch)

    async def shutdown(self) -> dict:
        """Best-effort lifetime fence, with one owner and no new admission."""
        failure = None
        if self.runner:
            self.runner.cancel()
        if self.task:
            try:
                await self.task
            except Exception as exc:
                failure = exc
        try:
            await self.resources.close()
        except Exception as exc:
            failure = failure or exc
        self.state = "stopped"
        return (
            error_response(failure) if failure else {"stopped": True, "generation": self.generation}
        )

    def begin_shutdown(self):
        """Close admission synchronously before scheduling any asynchronous cleanup."""
        if self.shutdown_task is None:
            self.state = "stopping"
            self.shutdown_task = asyncio.create_task(self.shutdown())
        return self.shutdown_task

    async def dispatch(self, value: dict) -> dict:
        """Handle one handshaken operation; only stop waits for lifetime cleanup."""
        operation = value.get("operation")
        if operation == "status":
            return self.status()
        if operation == "stop":
            return await asyncio.shield(self.begin_shutdown())
        if operation == "execute":
            return self.admit(value)
        if operation == "query":
            return self.query(value.get("execution_id"), value.get("epoch"))
        if operation == "cancel":
            if (
                value.get("generation") != self.generation
                or not self.active
                or value.get("execution_id") != self.active["request"].execution_id
                or value.get("epoch") != self.active["request"].epoch
            ):
                raise PTCError(
                    "stale_execution", "cancellation does not match the active execution"
                )
            self.runner.cancel()
            return {"cancellation_requested": True}
        if operation == "inspect":
            return (
                dict(self.info or {}, active=self.status()["active"])
                if self.active
                else self.refresh()
            )
        if self.state != "ready":
            raise PTCError("stopping", "daemon is stopping")
        if self.active:
            raise PTCError("busy", "session is already executing")
        if operation == "recover":
            self.session.recover(value.get("epoch"), value.get("execution_id"))
        elif operation == "reset":
            self.session.reset(value.get("epoch"))
        else:
            raise PTCError("invalid_operation", "unknown daemon operation")
        self.last = None
        return self.refresh()

    async def connection(self, reader, writer):
        """Bound connection count, handshake, framing and writes independently of execution."""
        task = asyncio.current_task()
        if len(self.handlers) >= 32:
            writer.close()
            return
        self.handlers.add(task)
        stopped = False
        try:
            check_peer(writer.get_extra_info("socket"))
            hello = await read_frame(reader)
            management = hello.get("management") is True
            if (
                hello.get("version") != VERSION
                or hello.get("directory") != str(self.endpoint.directory)
                or hello.get("identity") != self.directory_identity
                or self.endpoint.identity() != self.directory_identity
            ):
                raise PTCError("daemon_incompatible", "daemon identity or protocol differ")
            if not management and (
                hello.get("compatibility") != compatibility(self.registry)
                or hello.get("modules") != self.modules
                or hello.get("policy") != asdict(self.policy)
            ):
                raise PTCError("daemon_incompatible", "daemon runtime, modules or policy differ")
            await write_frame(writer, {"version": VERSION, "generation": self.generation})
            value = await read_frame(reader)
            if not isinstance(value.get("id"), str) or not 1 <= len(value["id"]) <= 128:
                raise PTCError("invalid_request", "transport request ID is required")
            try:
                if management and value.get("operation") not in (
                    "status",
                    "stop",
                    "query",
                    "cancel",
                ):
                    raise PTCError(
                        "invalid_operation", "management connection cannot mutate session state"
                    )
                result = await self.dispatch(value)
            except Exception as exc:
                result = {"boundary_error": error_response(exc)}
            stopped = value.get("operation") == "stop"
            await write_frame(writer, {"id": value["id"], "result": result})
        except PTCError as exc:
            with contextlib.suppress(Exception):
                await write_frame(writer, {"boundary_error": error_response(exc)})
        except (OSError, EOFError, asyncio.IncompleteReadError, TimeoutError):
            pass
        finally:
            writer.close()
            try:
                async with asyncio.timeout(0.25):
                    await writer.wait_closed()
            except (Exception, asyncio.CancelledError):
                writer.transport.abort()
            self.handlers.discard(task)
            if stopped:
                self.finished.set()

    async def serve(self):
        """Keep the lifetime lock until resources, handlers, and endpoint are settled."""
        with self.endpoint.locked():
            self.directory_identity = self.endpoint.identity()
            try:
                self.endpoint.check_socket()
            except FileNotFoundError:
                pass
            else:
                # A replaced durable directory may have a different lock inode
                # but the same socket pathname. Never unlink a live endpoint.
                try:
                    connection = self.endpoint.connect()
                except ConnectionRefusedError:
                    self.endpoint.socket.unlink()
                else:
                    connection.close()
                    raise PTCError("endpoint_busy", "a live endpoint already uses this pathname")
            server = await asyncio.start_unix_server(
                self.connection, path=str(self.endpoint.socket)
            )
            os.chmod(self.endpoint.socket, 0o600)
            inode = self.endpoint.socket.stat().st_ino
            loop = asyncio.get_running_loop()
            for sig in (signal.SIGINT, signal.SIGTERM):
                loop.add_signal_handler(sig, self.begin_shutdown)
            try:
                shutdown_finished_at = None
                while not self.finished.is_set():
                    if self.resources.expired(self.active is not None):
                        self.begin_shutdown()
                    if self.shutdown_task and self.shutdown_task.done():
                        # Give an explicit stop handler time to send its cleanup result.
                        shutdown_finished_at = shutdown_finished_at or loop.time()
                        if not self.handlers or loop.time() - shutdown_finished_at >= 0.25:
                            break
                    try:
                        await asyncio.wait_for(self.finished.wait(), 0.05)
                    except TimeoutError:
                        pass
            finally:
                server.close()
                await server.wait_closed()
                await asyncio.shield(self.begin_shutdown())
                for task in list(self.handlers):
                    task.cancel()
                await asyncio.gather(*self.handlers, return_exceptions=True)
                if self.endpoint.socket.exists() and self.endpoint.socket.stat().st_ino == inode:
                    self.endpoint.socket.unlink()


def main():
    """Internal foreground entry; boot input is trusted operator configuration only."""
    os.umask(0o077)
    value = unpack(sys.stdin.buffer.read(64 * 1024 + 1))
    daemon = Daemon(Endpoint(Path(sys.argv[1])), value["modules"], Policy(**value["policy"]))
    asyncio.run(daemon.serve())


if __name__ == "__main__":
    main()
