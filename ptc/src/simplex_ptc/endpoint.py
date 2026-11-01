"""Private endpoint discovery and non-inheritable session lifetime ownership."""

import fcntl
import hashlib
import os
import socket
import stat
import struct
import tempfile
from contextlib import contextmanager
from pathlib import Path

from .protocol import PTCError
from .storage import Storage


def private_directory(path: Path) -> Path:
    """Create/check an ephemeral private directory; this is not a durability fence."""
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    return _check_private_directory(path)


def _check_private_directory(path: Path) -> Path:
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise PTCError("unsafe_directory", "directory must be owned by this user and private")
    return path


def _creation_intent(path: Path) -> Path:
    """Locate a fixed-size creation intent without listing the containing directory."""
    identity = hashlib.sha256(os.fsencode(path)).hexdigest()[:32]
    return path.parent / f".simplex-ptc-create-{os.getuid()}-{identity}.pending"


def durable_directory(path: Path) -> Path:
    """Fence only new directory entries and unfinished creation from earlier attempts.

    Publish an empty private intent in each parent before creating its child.
    Other starters check that intent after observing the child, so a visible
    directory is not mistaken for completed creation. Remove the intent only
    after the parent sync succeeds. Existing paths without intents need only
    search permission on ancestors, not read access or filesystem sync support.
    """
    path = path.absolute()
    for directory in reversed((path, *path.parents)):
        if directory == directory.parent:
            continue
        exists = directory.exists()
        intent = _creation_intent(directory)
        try:
            intent.lstat()
        except FileNotFoundError:
            if exists:
                continue
        fd = os.open(intent, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        try:
            info = os.fstat(fd)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != os.getuid()
                or info.st_mode & 0o077
                or info.st_nlink > 1
                or info.st_size != 0
            ):
                raise PTCError("unsafe_creation_intent", "directory creation intent is not private")
            os.fsync(fd)
        finally:
            os.close(fd)
        # Persist intent before mkdir, including retry after an earlier failure.
        Storage._sync_path(directory.parent)
        directory.mkdir(mode=0o700, exist_ok=True)
        Storage._sync_path(directory.parent)
        try:
            current = intent.lstat()
        except FileNotFoundError:
            pass
        else:
            if (current.st_dev, current.st_ino) == (info.st_dev, info.st_ino):
                intent.unlink(missing_ok=True)
        # The child entry was already fenced; failure here cannot hide required
        # creation work even when the intent unlink became visible.
        Storage._sync_path(directory.parent)
    return _check_private_directory(path)


class Endpoint:
    """Resolve aliases to one durable lock and a short internal socket address."""

    def __init__(self, directory: Path):
        self.directory = directory.resolve()
        runtime = os.environ.get("XDG_RUNTIME_DIR")
        if runtime:
            base = private_directory(Path(runtime))
            root = base / "simplex-ptc"
        else:
            root = Path(tempfile.gettempdir()) / f"simplex-ptc-{os.getuid()}"
        self.root = private_directory(root)
        identity = hashlib.sha256(os.fsencode(self.directory)).hexdigest()[:32]
        self.socket = self.root / f"{identity}.sock"
        if len(os.fsencode(self.socket)) > 100:
            raise PTCError("endpoint_path", "runtime directory is too long for a Unix socket")

    def identity(self) -> list[int]:
        """Pin handshake identity to the actual directory, not just its path hash."""
        info = self.directory.stat()
        return [info.st_dev, info.st_ino]

    def check_socket(self) -> None:
        """Reject foreign/symlink/non-socket objects without deleting them."""
        info = self.socket.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise PTCError("unsafe_endpoint", "endpoint is not a private owned socket")

    @contextmanager
    def locked(self):
        """Only this owner can bind/remove the endpoint; lock inodes are never removed."""
        durable_directory(self.directory)
        fd = os.open(self.directory / "daemon.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                raise PTCError("unsafe_lock", "lifetime lock must be a private owned file")
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise PTCError("daemon_busy", "session daemon already owns the directory") from exc
            yield
        finally:
            os.close(fd)

    def connect(self) -> socket.socket:
        """Connect with a bounded handshake; the caller owns the returned descriptor."""
        self.check_socket()
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(10)
            sock.connect(str(self.socket))
            check_peer(sock)
            return sock
        except BaseException:
            sock.close()
            raise


def check_peer(sock) -> None:
    """Linux credential checks supplement directory permissions, not a same-UID sandbox."""
    if hasattr(socket, "SO_PEERCRED"):
        _, uid, _ = struct.unpack("3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if uid != os.getuid():
            raise PTCError("foreign_peer", "Unix socket peer belongs to another user")
