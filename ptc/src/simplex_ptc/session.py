"""Persistent idle sessions, with explicit unknown-outcome recovery.

A checkpoint and a worker conversation are not a distributed transaction.
Only the latest execution receipt is retained; historical exactly-once execution
is deliberately not promised. IDs must not be recycled by callers.
"""

import base64
import hashlib
import uuid
from pathlib import Path

import pydantic_monty as monty

from . import runtime
from .extensions import ToolRegistry
from .protocol import (
    MAX_CHECKPOINT_BYTES,
    Output,
    PTCError,
    Request,
    bound_response,
    encode,
    preview,
)
from .storage import Storage


def digest(data: bytes) -> str:
    """Content digest for request correlation and accidental corruption detection."""
    return hashlib.sha256(data).hexdigest()


class Session:
    """Manage one trusted POSIX directory; all operations hold its exclusive lock."""

    def __init__(self, directory: Path, registry: ToolRegistry | None = None):
        self.storage = Storage(directory)
        self.registry = registry if registry is not None else ToolRegistry()

    def _new(self) -> dict:
        """An empty state is represented by no dump, not replayable source text."""
        return {
            "format": 1,
            "session_id": str(uuid.uuid4()),
            "epoch": str(uuid.uuid4()),
            "revision": 0,
            "compatibility": runtime.compatibility(self.registry),
            "dump": None,
            "dump_sha256": digest(b""),
            "receipt": None,
        }

    def _load(self) -> dict | None:
        """Validate metadata before passing any opaque bytes to Monty."""
        value = self.storage.read("checkpoint")
        if value is None:
            return None
        try:
            if value["format"] != 1 or value["compatibility"] != runtime.compatibility(
                self.registry
            ):
                raise PTCError("incompatible_state", "runtime, format, tools or limits changed")
            uuid.UUID(value["session_id"])
            uuid.UUID(value["epoch"])
            if type(value["revision"]) is not int or value["revision"] < 0:
                raise ValueError("revision")
            raw = self._dump(value)
            if digest(raw or b"") != value["dump_sha256"]:
                raise ValueError("checksum")
            receipt = value["receipt"]
            if receipt is not None and (
                not isinstance(receipt, dict)
                or not isinstance(receipt["execution_id"], str)
                or not isinstance(receipt["request_digest"], str)
                or not isinstance(receipt["response"], dict)
            ):
                raise ValueError("receipt")
        except (KeyError, ValueError, TypeError) as exc:
            raise PTCError("corrupt_state", "invalid checkpoint metadata or dump") from exc
        return value

    @staticmethod
    def _dump(value: dict) -> bytes | None:
        """Decode only the validated checkpoint's opaque idle-session payload."""
        return None if value["dump"] is None else base64.b64decode(value["dump"], validate=True)

    def _pending(self, checkpoint: dict | None) -> dict | None:
        """Recognize a committed receipt left behind by a crash before unlink."""
        pending = self.storage.read("execution.pending")
        if pending is None:
            return None
        if (
            not isinstance(pending.get("execution_id"), str)
            or not isinstance(pending.get("epoch"), str)
            or type(pending.get("base_revision")) is not int
            or not isinstance(pending.get("request_digest"), str)
        ):
            raise PTCError("corrupt_state", "invalid pending marker")
        receipt = checkpoint.get("receipt") if checkpoint else None
        if (
            receipt
            and pending["epoch"] == checkpoint["epoch"]
            and pending["execution_id"] == receipt["execution_id"]
            and pending["request_digest"] == receipt["request_digest"]
        ):
            return None
        if (
            checkpoint is None
            or pending["epoch"] != checkpoint["epoch"]
            or pending["base_revision"] != checkpoint["revision"]
        ):
            raise PTCError("corrupt_state", "pending marker does not match checkpoint")
        return pending

    def inspect(self) -> dict:
        """Expose metadata only. Inspection never resumes, repairs or clears state."""
        with self.storage.locked():
            checkpoint = self._load()
            pending = self._pending(checkpoint)
            return {
                "exists": checkpoint is not None,
                "session_id": checkpoint["session_id"] if checkpoint else None,
                "epoch": checkpoint["epoch"] if checkpoint else None,
                "revision": checkpoint["revision"] if checkpoint else 0,
                "recovery_required": pending is not None,
                "pending": pending,
                "last_execution_id": (checkpoint or {}).get("receipt", {}).get("execution_id")
                if (checkpoint or {}).get("receipt")
                else None,
            }

    def execute(self, request: Request) -> dict:
        """Commit success or a failure receipt without rolling back external effects."""
        request = Request.parse(
            {"execution_id": request.execution_id, "source": request.source, "epoch": request.epoch}
        )
        request_digest = digest(encode({"source": request.source, "epoch": request.epoch}))
        with self.storage.locked():
            checkpoint = self._load()
            if self._pending(checkpoint) is not None:
                raise PTCError(
                    "recovery_required", "interrupted execution must be acknowledged first"
                )
            if checkpoint is None:
                if request.epoch is not None:
                    raise PTCError("stale_epoch", "cannot resume a missing session")
                checkpoint = self._new()
                self.storage.write("checkpoint", checkpoint)
            receipt = checkpoint["receipt"]
            if receipt and receipt["execution_id"] == request.execution_id:
                if receipt["request_digest"] != request_digest:
                    raise PTCError(
                        "identity_conflict", "execution_id was used with different source"
                    )
                # Visibility after rename is not proof that the directory
                # durability fence succeeded. Retry the fence even if an earlier
                # unlink succeeded but its sync failed and no marker is visible.
                # Never re-run source while reconciling a published receipt.
                self.storage.sync_directory()
                self.storage.clear_pending()
                return receipt["response"]
            if self.storage.read("execution.pending") is not None:
                # _pending already established this marker matches the receipt.
                # Settle that publication before admitting a different execution.
                self.storage.sync_directory()
                self.storage.clear_pending()
            initial_request = (
                request.epoch is None
                and checkpoint["receipt"] is None
                and not checkpoint.get("requires_epoch", False)
            )
            if request.epoch != checkpoint["epoch"] and not initial_request:
                raise PTCError("stale_epoch", "supply the current session epoch")
            self.storage.write(
                "execution.pending",
                {
                    "epoch": checkpoint["epoch"],
                    "base_revision": checkpoint["revision"],
                    "execution_id": request.execution_id,
                    "request_digest": request_digest,
                },
            )
            output = Output()
            response = {
                "protocol": 1,
                "execution_id": request.execution_id,
                "session_id": checkpoint["session_id"],
                "epoch": checkpoint["epoch"],
                "revision": checkpoint["revision"],
                "status": "completed",
                "committed": False,
                "result": None,
                "output": "",
                "output_omitted_bytes": 0,
            }
            try:
                result, state = runtime.run(
                    request.source, self._dump(checkpoint), self.registry, output
                )
                response.update(
                    result=result, output=output.text, output_omitted_bytes=output.omitted_bytes
                )
                response = bound_response(response)
                candidate = dict(checkpoint)
                candidate["dump"] = base64.b64encode(state).decode()
                candidate["dump_sha256"] = digest(state)
                candidate["revision"] += 1
                # Leave room for the bounded receipt before accepting a dump.
                if len(encode(candidate)) + 64 * 1024 > MAX_CHECKPOINT_BYTES:
                    raise PTCError("storage_limit", "snapshot exceeds checkpoint budget")
                response.update(committed=True, revision=candidate["revision"])
                response = bound_response(response)
            except (monty.MontyError, PTCError, KeyboardInterrupt) as exc:
                code = (
                    exc.code
                    if isinstance(exc, PTCError)
                    else ("cancelled" if isinstance(exc, KeyboardInterrupt) else "runtime_error")
                )
                if isinstance(exc, monty.MontySyntaxError):
                    code = "compile_error"
                elif isinstance(exc, monty.MontyRuntimeError) and isinstance(
                    exc.exception(), (TimeoutError, MemoryError, RecursionError)
                ):
                    code = "resource_limit"
                elif isinstance(exc, monty.MontyCrashedError):
                    code = "runtime_crashed"
                response.update(
                    status="failed",
                    committed=False,
                    revision=checkpoint["revision"],
                    result=None,
                    error={"code": code, "message": preview(str(exc), 2048)},
                    output=output.text,
                    output_omitted_bytes=output.omitted_bytes,
                )
                response.pop("result_preview", None)
                response.pop("result_truncated", None)
                response = bound_response(response)
            else:
                # Outside the failure handler: interruption from here onward
                # leaves a pending outcome instead of committing a failed heap.
                checkpoint = candidate
            checkpoint["receipt"] = {
                "execution_id": request.execution_id,
                "request_digest": request_digest,
                "response": response,
            }
            # Deliberately outside the runtime-error handler: failed publication
            # leaves pending intact; it must not overwrite a possibly new state.
            self.storage.write("checkpoint", checkpoint)
            self.storage.clear_pending()
            return response

    def recover(self, epoch: str, execution_id: str) -> None:
        """Acknowledge unknown effects without re-running the interrupted source."""
        with self.storage.locked():
            checkpoint = self._load()
            pending = self._pending(checkpoint)
            if (
                not checkpoint
                or not pending
                or pending["epoch"] != epoch
                or pending["execution_id"] != execution_id
            ):
                raise PTCError("stale_recovery", "pending execution does not match")
            response = {
                "protocol": 1,
                "execution_id": execution_id,
                "epoch": epoch,
                "session_id": checkpoint["session_id"],
                "revision": checkpoint["revision"],
                "status": "failed",
                "committed": False,
                "error": {
                    "code": "interrupted",
                    "message": "external effects are unknown; source was not replayed",
                },
            }
            checkpoint["receipt"] = {
                "execution_id": execution_id,
                "request_digest": pending["request_digest"],
                "response": response,
            }
            self.storage.write("checkpoint", checkpoint)
            self.storage.clear_pending()

    def reset(self, epoch: str) -> None:
        """Replace an idle compatible session with an empty epoch; keep external files."""
        with self.storage.locked():
            checkpoint = self._load()
            if not checkpoint or checkpoint["epoch"] != epoch:
                raise PTCError("stale_reset", "session epoch does not match")
            if self._pending(checkpoint):
                raise PTCError(
                    "recovery_required", "acknowledge interrupted execution before reset"
                )
            # Remove a matching old receipt marker before changing epochs.
            # A crash after publication must not resurrect the old pending ID.
            self.storage.clear_pending()
            replacement = self._new()
            replacement["session_id"] = checkpoint["session_id"]
            replacement["requires_epoch"] = True
            self.storage.write("checkpoint", replacement)
