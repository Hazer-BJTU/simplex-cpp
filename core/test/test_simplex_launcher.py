"""Check public routing, relocation, symlinks and exact exec argument forwarding."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
assert source.is_file() and os.access(source, os.X_OK), source

with tempfile.TemporaryDirectory(prefix="simplex launcher ") as temporary:
    root = Path(temporary)
    prefix = root / "original prefix"
    binary_dir = prefix / "bin"
    binary_dir.mkdir(parents=True)
    launcher = binary_dir / "simplex"
    shutil.copy2(source, launcher)

    # Top-level help needs no worker binary or model configuration.
    for arguments in ([], ["--help"], ["-h"]):
        result = subprocess.run([str(launcher), *arguments], capture_output=True,
                                text=True, timeout=5)
        assert result.returncode == 0, result.stderr
        assert "simplex <command>" in result.stdout
        assert "run" in result.stdout
        assert "shell" not in result.stdout
    for command in ("unknown", "shell", "--unknown"):
        result = subprocess.run([str(launcher), command], capture_output=True,
                                text=True, timeout=5)
        assert result.returncode == 2, result
        assert "unknown command" in result.stderr
    for arguments in (["run"], ["--version"]):
        missing = subprocess.run([str(launcher), *arguments], capture_output=True,
                                 text=True, timeout=5)
        assert missing.returncode == 127, missing
        assert "worker executable" in missing.stderr

    worker = binary_dir / "simplex_worker"
    worker.write_text('''#!/usr/bin/env python3
import json, os, sys
print(json.dumps({"arguments": sys.argv[1:], "pid": os.getpid(),
                  "cwd": os.getcwd(), "environment": os.environ["SIMPLEX_LAUNCH_TEST"]}))
sys.exit(int(os.environ["SIMPLEX_LAUNCH_EXIT"]))
''')
    worker.chmod(0o755)

    # A relocated tree must not retain its old prefix, or change the caller's cwd.
    moved = root / "relocated prefix"
    prefix.rename(moved)
    launcher = moved / "bin/simplex"
    caller = root / "caller workspace"
    caller.mkdir()
    links = root / "links"
    links.mkdir()
    (links / "first").symlink_to(os.path.relpath(launcher, links))
    (links / "simplex").symlink_to("first")
    environment = {**os.environ, "PATH": str(links) + os.pathsep + os.environ["PATH"],
                   "SIMPLEX_LAUNCH_TEST": "preserved value", "SIMPLEX_LAUNCH_EXIT": "37"}
    arguments = ["--config", "a config.yaml", "", "line one\nline two",
                 "$(must_not_execute)", "*.cpp", "unicode-中文", "--threads=4"]
    for entry in (str(launcher), str(links / "simplex"), "simplex"):
        process = subprocess.Popen([entry, "run", *arguments], cwd=caller, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 37, (stdout, stderr)
        observed = json.loads(stdout)
        assert observed["arguments"] == arguments, observed
        assert observed["pid"] == process.pid, "router must exec, not leave a parent shell"
        assert observed["cwd"] == str(caller), observed
        assert observed["environment"] == "preserved value", observed

        # Top-level version queries use the same relocatable exec path, and
        # preserve the worker's output, exit code and remaining arguments.
        result = subprocess.run([entry, "--version", "--config", "a config.yaml"],
                                cwd=caller, env=environment, capture_output=True,
                                text=True, timeout=5)
        assert result.returncode == 37, result.stderr
        assert json.loads(result.stdout)["arguments"] == [
            "--version", "--config", "a config.yaml"
        ], result.stdout

print("simplex launcher: routing, relocation, symlinks and exec forwarding passed")
