"""POSIX-only trusted-directory storage, with an exclusive lifetime lock.

Files are private interpreter data, not an import format. A checksum detects
accidental damage; it does not authenticate a checkpoint from an attacker.
"""

import fcntl
import json
import os
import tempfile
from contextlib import contextmanager
from pathlib import Path

from .protocol import MAX_CHECKPOINT_BYTES, PTCError, encode


class Storage:
    """Own only named files in the caller-selected PTC directory."""

    def __init__(self, directory: Path):
        self.directory = directory

    @contextmanager
    def locked(self):
        """Fail immediately on overlap. Never unlink the lock's inode."""
        missing = []
        parent = self.directory.absolute()
        while not parent.exists():
            missing.append(parent)
            parent = parent.parent
        for directory in reversed(missing):
            directory.mkdir(mode=0o700, exist_ok=True)
            self._sync_path(directory.parent)
        with (self.directory / "session.lock").open("a+b") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise PTCError("busy", "session is already in use") from exc
            try:
                # No other invocation can own these staging files while locked.
                # Never recurse or remove unrelated files in the chosen directory.
                for pattern in (".checkpoint-*", ".execution.pending-*"):
                    for temporary in self.directory.glob(pattern):
                        if temporary.is_file() and not temporary.is_symlink():
                            temporary.unlink()
                yield self
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def read(self, name: str) -> dict | None:
        """Read bounded strict JSON; never interpret missing data as corruption."""
        try:
            with (self.directory / name).open("rb") as stream:
                data = stream.read(MAX_CHECKPOINT_BYTES + 1)
        except FileNotFoundError:
            return None
        if len(data) > MAX_CHECKPOINT_BYTES:
            raise PTCError("corrupt_state", "stored file exceeds size limit")
        try:
            value = json.loads(data)
            if not isinstance(value, dict):
                raise ValueError("not an object")
            return value
        except (ValueError, UnicodeError, RecursionError) as exc:
            raise PTCError("corrupt_state", f"invalid {name}") from exc

    def sync_directory(self) -> None:
        """Make publication/unlink durable on supported local POSIX filesystems."""
        self._sync_path(self.directory)

    @staticmethod
    def _sync_path(path: Path) -> None:
        """Sync a directory entry, including newly created ancestor directories."""
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def write(self, name: str, value: dict) -> None:
        """Publish a single consistent generation; failures may be post-replace.

        Callers must not assume an exception means publication did not happen.
        A pending marker and receipt reconcile that case on the next invocation.
        """
        data = encode(value)
        if len(data) > MAX_CHECKPOINT_BYTES:
            raise PTCError("storage_limit", "checkpoint exceeds size limit")
        descriptor, temporary = tempfile.mkstemp(prefix=f".{name}-", dir=self.directory)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.directory / name)
            self.sync_directory()
        finally:
            Path(temporary).unlink(missing_ok=True)

    def clear_pending(self) -> None:
        """Settle the pending marker only after checkpoint publication."""
        (self.directory / "execution.pending").unlink(missing_ok=True)
        self.sync_directory()
