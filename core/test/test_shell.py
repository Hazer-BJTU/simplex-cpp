"""Plain terminal router integration, using only Python's standard library."""
import base64
import json
import os
import queue
import re
import socket
import struct
import subprocess
import sys
import threading
import time


def read_exact(sock, count):
    result = b""
    while len(result) < count:
        part = sock.recv(count - len(result))
        if not part:
            raise EOFError("connection closed")
        result += part
    return result


def connect(port, path, status=101):
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f"GET {path} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                  f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                  "Sec-WebSocket-Version: 13\r\n\r\n").encode())
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        header += read_exact(sock, 1)
    assert int(header.split()[1]) == status, header
    return sock


def send(sock, value, opcode=1):
    payload = json.dumps(value).encode() if opcode == 1 else value
    mask = os.urandom(4)
    size = len(payload)
    header = bytes([0x80 | opcode])
    if size < 126:
        header += bytes([0x80 | size])
    elif size < 65536:
        header += bytes([0x80 | 126]) + struct.pack("!H", size)
    else:
        header += bytes([0x80 | 127]) + struct.pack("!Q", size)
    masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    sock.sendall(header + mask + masked)


def receive(sock):
    head = read_exact(sock, 2)
    opcode = head[0] & 15
    size = head[1] & 127
    if size == 126:
        size = struct.unpack("!H", read_exact(sock, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", read_exact(sock, 8))[0]
    payload = read_exact(sock, size)
    return opcode, json.loads(payload) if opcode == 1 else payload


def close(sock):
    send(sock, b"\x03\xe8", opcode=8)
    assert receive(sock)[0] == 8
    sock.close()


with socket.socket() as reserve:
    reserve.bind(("127.0.0.1", 0))
    port = reserve.getsockname()[1]

process = subprocess.Popen([sys.argv[1], "--listen", f"127.0.0.1:{port}"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
output = queue.Queue()
transcript = ""
connections = []


def pump():
    for line in process.stdout:
        output.put(line)


threading.Thread(target=pump, daemon=True).start()


def wait_for(text, start=0):
    global transcript
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if text in transcript[start:]:
            return
        try:
            transcript += output.get(timeout=0.1)
        except queue.Empty:
            pass
    raise AssertionError(text + "\n" + transcript)


def command(line):
    process.stdin.write(line + "\n")
    process.stdin.flush()


def event(sock, worker="worker"):
    send(sock, {"type": "event", "event": "ready", "session_id": "test",
                "worker_id": worker, "run_id": "run-1", "request_id": "one",
                "sequence": 1, "data": {"marker": "fixture-ready"}})


try:
    wait_for("Waiting for one worker")
    connect(port, "/unknown", 404).close()
    worker = connect(port, "/agent/events")
    connections.append(worker)
    event(worker)
    wait_for("fixture-ready")
    connect(port, "/agent/events", 409).close()

    confirmations = []
    for identifier in ["first", "second"]:
        connection = connect(port, "/agent/confirm")
        connections.append(connection)
        confirmations.append(connection)
        send(connection, {"type": "confirmation_request", "data": {
            "session_id": "test", "run_id": "run-1", "confirmation_id": identifier,
            "call": {"name": "fixture", "arguments": {}}}})
        wait_for("=== Confirm " + identifier)
    send(worker, {"type": "event", "event": "status", "session_id": "test",
                  "worker_id": "worker", "run_id": "older-run", "request_id": "",
                  "sequence": 2, "data": {"marker": "delayed-status"}})
    wait_for("delayed-status")
    command("/cancel")
    cancellation = receive(worker)[1]
    assert cancellation["data"]["run_id"] == "run-1", cancellation
    command("/status")
    assert receive(worker)[1]["data"]["operation"] == "status"
    command("/approve first")
    command("/deny second")
    for connection, decision in zip(confirmations, ["approved", "denied"]):
        reply = receive(connection)[1]
        assert reply["data"]["decision"] == decision, reply
        close(connection)

    pending = connect(port, "/agent/confirm")
    connections.append(pending)
    send(pending, {"type": "confirmation_request", "data": {
        "session_id": "test", "run_id": "run-1", "confirmation_id": "expired",
        "call": {"name": "fixture"}}})
    wait_for("=== Confirm expired")
    offset = len(transcript)
    pending.close()
    wait_for("Confirmation ended", offset)
    command("/approve expired")
    wait_for("Expired prompt", offset)
    command("/status")
    assert receive(worker)[1]["data"]["operation"] == "status"

    offset = len(transcript)
    close(worker)
    wait_for("Worker disconnected", offset)
    command("offline input must be discarded")
    wait_for("Not sent", offset)
    reconnect = connect(port, "/agent/events")
    connections.append(reconnect)
    event(reconnect)
    command("/status")
    # No automatic replay of the offline message.
    assert receive(reconnect)[1]["data"]["operation"] == "status"
    command("/quit")
    process.wait(timeout=5)
    assert process.returncode == 0
    print("shell routes, one-worker admission, concurrent prompts, expiry, reconnect: passed")
finally:
    for connection in connections:
        connection.close()
    if process.poll() is None:
        process.kill()
        process.wait()
