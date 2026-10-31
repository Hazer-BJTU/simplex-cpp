"""Bounded private UDS frames, separate from the public CLI JSON envelope."""

import asyncio
import json
import socket
import struct

from .protocol import MAX_REQUEST_BYTES, MAX_RESPONSE_BYTES, PTCError, encode

VERSION = 1
MAX_CONTEXT_BYTES = 1024 * 1024
MAX_FRAME_BYTES = MAX_REQUEST_BYTES + MAX_CONTEXT_BYTES + 64 * 1024
MAX_REPLY_BYTES = MAX_RESPONSE_BYTES + 64 * 1024
IO_TIMEOUT = 10.0


def pack(value: dict, limit: int = MAX_FRAME_BYTES) -> bytes:
    """Validate encoded size before allocating a transport write."""
    data = encode(value)
    if len(data) > limit:
        raise PTCError("transport_limit", "frame exceeds byte limit")
    return struct.pack("!I", len(data)) + data


def unpack(data: bytes) -> dict:
    """Require one strict JSON object; reject invalid/non-finite JSON values."""
    try:
        value = json.loads(data)
        if not isinstance(value, dict):
            raise ValueError("not an object")
        encode(value)
        return value
    except (ValueError, TypeError, UnicodeError, RecursionError) as exc:
        raise PTCError("invalid_frame", "expected a strict JSON object") from exc


def receive(sock: socket.socket, limit: int = MAX_REPLY_BYTES) -> dict:
    """Read exactly one frame, detecting EOF and rejecting oversized prefixes."""

    def exact(count):
        result = bytearray()
        while len(result) < count:
            part = sock.recv(count - len(result))
            if not part:
                raise EOFError("peer disconnected")
            result.extend(part)
        return bytes(result)

    size = struct.unpack("!I", exact(4))[0]
    if size > limit:
        raise PTCError("transport_limit", "frame exceeds byte limit")
    return unpack(exact(size))


async def read_frame(reader: asyncio.StreamReader) -> dict:
    """Bound partial-frame waiting as well as allocation on the server."""
    async with asyncio.timeout(IO_TIMEOUT):
        size = struct.unpack("!I", await reader.readexactly(4))[0]
        if size > MAX_FRAME_BYTES:
            raise PTCError("transport_limit", "frame exceeds byte limit")
        return unpack(await reader.readexactly(size))


async def write_frame(writer: asyncio.StreamWriter, value: dict) -> None:
    """A slow reader cannot hold a server handler indefinitely."""
    writer.write(pack(value, MAX_REPLY_BYTES))
    async with asyncio.timeout(IO_TIMEOUT):
        await writer.drain()


def execution_context(value: object) -> dict:
    """Validate trusted caller cwd/environment without exposing values in errors."""
    if not isinstance(value, dict) or set(value) != {"cwd", "environment"}:
        raise PTCError("invalid_context", "expected cwd and environment")
    cwd, environment = value["cwd"], value["environment"]
    if not isinstance(cwd, str) or not cwd.startswith("/") or "\0" in cwd:
        raise PTCError("invalid_context", "cwd must be an absolute path")
    if not isinstance(environment, dict) or any(
        not isinstance(k, str)
        or not k
        or "=" in k
        or "\0" in k
        or not isinstance(v, str)
        or "\0" in v
        for k, v in environment.items()
    ):
        raise PTCError("invalid_context", "invalid environment")
    try:
        size = len(encode(value))
        cwd.encode()
        for k, v in environment.items():
            k.encode()
            v.encode()
    except (ValueError, UnicodeError) as exc:
        raise PTCError("invalid_context", "context must be UTF-8") from exc
    if size > MAX_CONTEXT_BYTES:
        raise PTCError("invalid_context", "context exceeds byte limit")
    return {"cwd": cwd, "environment": dict(sorted(environment.items()))}
