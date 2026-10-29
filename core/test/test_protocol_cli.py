"""Inspect a build/installed worker without configuration, from unrelated paths."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

worker = Path(sys.argv[1]).absolute()
launcher = worker.with_name('simplex')
artifact = Path(__file__).resolve().parents[2] / 'protocol/schemas/worker-protocol.json'
expected = json.loads(artifact.read_text())
with tempfile.TemporaryDirectory(prefix='simplex protocol ') as directory:
    cwd = Path(directory)
    link = cwd / 'linked simplex'
    link.symlink_to(launcher)
    # An external artifact in the caller's directory must not alter inspection.
    (cwd / 'worker-protocol.json').write_text('{"fingerprint":"not-the-contract"}')
    commands = [(worker, '--protocol'), (launcher, 'protocol'), (link, 'protocol')]
    for command in commands:
        result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stderr
        assert result.stderr == ''
        assert json.loads(result.stdout) == expected
        assert result.stdout.endswith('\n')
        args = (worker, '--protocol-hash') if command[0] == worker else (*command, '--hash')
        result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stderr
        assert result.stdout == expected['fingerprint'] + '\n'
        assert result.stderr == ''
    for args in [(launcher, 'protocol', ''), (launcher, 'protocol', '--unknown'), (launcher, 'protocol', '--hash', '--hash'),
                 (worker, '--protocol', '--session', 'x'), (worker, '--protocol', '--config', 'missing'),
                 (worker, '--protocol', '--protocol-hash'), (worker, '--protocol-hash', '--version')]:
        result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=15)
        assert result.returncode != 0, args
        assert not result.stdout, args
    if os.name == 'posix':
        for command in commands:
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                result = subprocess.run(command, cwd=cwd, stdout=write_fd,
                                        stderr=subprocess.PIPE, timeout=15)
                assert result.returncode != 0
            finally:
                os.close(write_fd)
