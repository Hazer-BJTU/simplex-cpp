"""Deterministically interleave allocation publication across separate processes."""
import base64
import fcntl
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile

binary, interposer = sys.argv[1:]


def command(root, operation='prepare'):
    request = dict(version=1, request_id='concurrent-test', operation=operation,
                   allocation_id='allocation', directory=str(root), configuration='persistence: {}')
    return [binary, '--request', base64.b64encode(json.dumps(request).encode()).decode()]


def invoke(root, operation='prepare'):
    result = subprocess.run(command(root, operation), capture_output=True, text=True, timeout=5)
    response = json.loads(result.stdout)
    assert (result.returncode == 0) == response['ok'], response
    return response


with tempfile.TemporaryDirectory(prefix='simplex-namespace-') as tmp:
    base = Path(tmp)
    outer = base / 'outer'
    inner = outer / 'inner'
    ready_r, ready_w = os.pipe()
    resume_r, resume_w = os.pipe()
    env = dict(os.environ, LD_PRELOAD=interposer,
               SIMPLEX_TEST_RENAME_TARGET=str(base / '.simplex-manager/outer.json'),
               SIMPLEX_TEST_READY_FD=str(ready_w), SIMPLEX_TEST_RESUME_FD=str(resume_r))
    parent = subprocess.Popen(command(outer), env=env, pass_fds=(ready_w, resume_r),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    lease = None
    try:
        assert select.select([ready_r], [], [], 10)[0], 'parent did not reach publication barrier'
        assert os.read(ready_r, 1) == b'r'
        assert not (base / '.simplex-manager/outer.json').exists()
        child = invoke(inner)
        if child['ok']:
            # If the bug regresses, hold the child's genuine shared lease before
            # parent removal and independently verify that data cannot be erased.
            lease = open(outer / '.simplex-manager/inner.lock', 'r+')
            fcntl.flock(lease, fcntl.LOCK_SH | fcntl.LOCK_NB)
            (inner / 'active-data').write_text('must survive')
        os.write(resume_w, b'g')
        stdout, stderr = parent.communicate(timeout=10)
        assert parent.returncode == 0, (stdout, stderr)
        assert json.loads(stdout)['ok']
        removed = invoke(outer, 'remove')
        if lease:
            assert not removed['ok'] and (inner / 'active-data').exists(), (
                'parent deletion erased an independently leased child', removed)
        assert not child['ok'] and child['error']['code'] == 'busy', child
        assert removed['ok']
    finally:
        if parent.poll() is None:
            parent.kill()
        parent.communicate()
        if lease:
            lease.close()
        for fd in (ready_r, ready_w, resume_r, resume_w):
            os.close(fd)

    # Opposite ordering: a committed child with a live lease makes the parent
    # directory unadoptable. Failed parent removal must leave all child data.
    outer = base / 'child-first'
    inner = outer / 'inner'
    assert invoke(inner)['ok']
    with open(outer / '.simplex-manager/inner.lock', 'r+') as lease:
        fcntl.flock(lease, fcntl.LOCK_SH | fcntl.LOCK_NB)
        (inner / 'active-data').write_text('keep')
        assert invoke(outer)['error']['code'] == 'unmanaged_directory'
        assert not invoke(outer, 'remove')['ok']
        assert (inner / 'active-data').read_text() == 'keep'
    assert invoke(inner, 'remove')['ok']
print('manager concurrency: both publication orders preserve allocation boundaries')
