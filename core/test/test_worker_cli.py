"""Check worker CLI parsing and local shutdown without model requests or tool invocation."""
import json
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile

worker = str(pathlib.Path(sys.argv[1]).resolve())


def invoke(*arguments):
    return subprocess.run(
        [worker, *arguments], capture_output=True, text=True, timeout=10
    )


for flag in ("--help", "-h"):
    result = invoke(flag)
    assert result.returncode == 0, result.stderr
    for option in ("--config", "--session", "--threads"):
        assert option in result.stdout, result.stdout

for value in ("0", "-1", "1.5", "abc", "999999999999999999999"):
    result = invoke("--session", "demo", "--threads", value)
    assert result.returncode != 0
    assert "threads" in result.stderr, result.stderr

for arguments in (("--unknown",), ("--threads",), ("--threads", "2")):
    result = invoke(*arguments)
    assert result.returncode != 0, arguments

# Valid options must reach configuration loading. A missing file deliberately
# stops startup before any network access, regardless of the thread count.
with tempfile.TemporaryDirectory() as directory:
    missing = str(pathlib.Path(directory) / "missing.yaml")
    for arguments in (
        ("--config", missing, "--session", "demo"),
        ("-c", missing, "-s", "demo", "-t", "4"),
        (f"--config={missing}", "--session=demo", "--threads=2"),
    ):
        result = invoke(*arguments)
        assert result.returncode != 0
        assert missing in result.stderr, result.stderr

# Exercise the real entry point with one and four executor threads. A loopback
# peer holds the HTTP upgrade open; no payload, model request or tool is run.
# SIGTERM must cancel the pending connection and join all execution threads.
for count in (1, 4):
    with tempfile.TemporaryDirectory() as directory, socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(10)
        port = listener.getsockname()[1]
        config = {
            "driver_model": "fixture",
            "providers": {"fixture": {
                "plugin": "deepseek", "model": "fixture",
                "endpoint": {"base_url": f"http://127.0.0.1:{port}",
                             "auth": {"scheme": "none"}},
            }},
            "client": {"endpoint": f"ws://127.0.0.1:{port}/events"},
            "persistence": {"enabled": False},
        }
        path = pathlib.Path(directory) / "config.yaml"
        path.write_text(json.dumps(config))
        process = subprocess.Popen(
            [worker, "--config", str(path), "--session", "cli-test",
             "--threads", str(count)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(10)
                assert connection.recv(4096), "Worker did not send an upgrade request"
                process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=10)
                assert process.returncode == 0, (stdout, stderr)
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
