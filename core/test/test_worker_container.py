"""Offline provider + actual worker/shell/process tools, restricted to Docker."""
import json
from pathlib import Path
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

if not Path("/.dockerenv").exists():
    sys.exit(77)

worker_binary, shell_binary = sys.argv[1:3]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def scenario(cancel):
    failures = []
    requests = []
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        marker = root / "executed"

        class Provider(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                try:
                    request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    requests.append(request)
                    results = [m for m in request["messages"] if m["role"] == "tool"]
                    if results:
                        assert "stdout-marker" in str(results), results
                        assert "stderr-marker" in str(results), results
                        delta = {"role": "assistant", "content": "fixture-finished"}
                        finish = "stop"
                    else:
                        delta = {"role": "assistant", "tool_calls": [{
                            "index": 0, "id": "call-1", "type": "function", "function": {
                                "name": "run_command",
                                "arguments": json.dumps({
                                    "command": "printf 'stdout-marker\\n'; printf 'stderr-marker\\n' >&2; "
                                               + "touch " + str(marker),
                                    "expected_runtime_milliseconds": 1000,
                                })}}]}
                        finish = "tool_calls"
                    frames = [
                        {"id": "fixture", "choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                        {"id": "fixture", "choices": [{"index": 0, "delta": {}, "finish_reason": finish}]},
                    ]
                    body = ("".join("data: " + json.dumps(frame) + "\n\n" for frame in frames)
                            + "data: [DONE]\n\n").encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except Exception as error:
                    failures.append(error)

        provider = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        port = free_port()
        config = {
            "providers": {"fixture": {"plugin": "deepseek", "model": "fixture",
                "endpoint": {"base_url": f"http://127.0.0.1:{provider.server_port}",
                             "auth": {"scheme": "none"}}, "retry": {"max_attempts": 0}}},
            "driver_model": "fixture",
            "client": {"endpoint": f"ws://127.0.0.1:{port}/agent/events"},
            "security": {"confirmation": {"endpoint": f"ws://127.0.0.1:{port}/agent/confirm",
                                            "timeout_ms": 5000}},
            "persistence": {"directory": str(root / "sessions"), "readable": True},
        }
        path = root / "config.yaml"
        path.write_text(json.dumps(config))
        shell = worker = None
        lines = queue.Queue()
        transcript = ""

        def pump(stream):
            for line in stream:
                lines.put(line)

        def wait_for(pattern):
            nonlocal transcript
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                match = re.search(pattern, transcript)
                if match:
                    return match
                try:
                    transcript += lines.get(timeout=0.2)
                except queue.Empty:
                    pass
            raise AssertionError("Timed out waiting for " + pattern + "\n" + transcript)

        def command(text):
            shell.stdin.write(text + "\n")
            shell.stdin.flush()

        try:
            with (root / "worker-errors.log").open("w+") as errors:
                shell = subprocess.Popen([shell_binary, "--listen", f"127.0.0.1:{port}"],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                reader = threading.Thread(target=pump, args=(shell.stdout,), daemon=True)
                reader.start()
                wait_for("Waiting for one worker")
                worker = subprocess.Popen([worker_binary, "--config", str(path), "--session", "integration"],
                                          stdout=errors, stderr=errors)
                wait_for("=== ready ===")
                command("Run the fixture command")
                match = wait_for(r"=== Confirm ([0-9a-f-]+) ===")
                command("/cancel" if cancel else "/approve " + match.group(1))
                wait_for(r'"status": "' + ("cancelled" if cancel else "completed") + '"')
                assert marker.exists() != cancel
                snapshot = root / "sessions/integration/state.json"
                state = json.loads(snapshot.read_text())
                assert state["loop"]["status"] == ("cancelled" if cancel else "completed")
                assert state["loop"]["phase"] == "ready"
                assert (root / "sessions/integration/readable.md").exists()
                if not cancel:
                    assert len(requests) == 2
                    assert "stdout (" in transcript and "stderr (" in transcript
                    assert "fixture-finished" in transcript
                else:
                    assert len(requests) == 1
                command("/shutdown")
                worker.wait(timeout=10)
                errors.seek(0)
                assert worker.returncode == 0, errors.read()
                command("/quit")
                shell.wait(timeout=5)
                assert shell.returncode == 0
                assert not failures, failures
                print(("cancel pending confirmation" if cancel else "approve and execute") + ": passed")
        finally:
            for process in (worker, shell):
                if process and process.poll() is None:
                    process.kill()
                    process.wait()
            provider.shutdown()
            provider.server_close()
            thread.join()


scenario(False)
scenario(True)
