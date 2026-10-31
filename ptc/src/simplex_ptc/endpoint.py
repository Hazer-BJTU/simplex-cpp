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


def private_directory(path: Path) -> Path:
    """Create/check an operator-owned directory; never follow its final symlink."""
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise PTCError("unsafe_directory", "directory must be owned by this user and private")
    return path


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
        private_directory(self.directory)
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
