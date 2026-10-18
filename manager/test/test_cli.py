"""Exercise the real argv/stdout protocol, including output beyond exec defaults."""
import base64
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, router = sys.argv[1:]


def invoke(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=15)


for args in ([binary, '--help'], [router, 'manage', '--help']):
    result = invoke(args)
    assert result.returncode == 0, result.stderr
    assert '--request' in result.stdout
for args in ([binary], [binary, '--request', 'not base64 secret'], [binary, '--unknown=secret']):
    result = invoke(args)
    assert result.returncode != 0
    error = json.loads(result.stdout)
    assert error['ok'] is False
    assert 'secret' not in result.stdout + result.stderr

with tempfile.TemporaryDirectory(prefix='simplex manage cli ') as tmp:
    root = Path(tmp) / 'worker'
    request = dict(version=1, request_id='cli-test', allocation_id='cli-allocation',
                   directory=str(root), operation='prepare',
                   configuration='# 中文 comment\npersistence: {}\n')

    def call(op, expected=True):
        request['operation'] = op
        encoded = base64.b64encode(json.dumps(request).encode()).decode()
        result = invoke([router, 'manage', '--request', encoded])
        response = json.loads(result.stdout)
        assert response['request_id'] == request['request_id']
        assert response['ok'] == expected, response
        assert (result.returncode == 0) == expected
        return response

    prepared = call('prepare')['result']
    assert prepared['configuration'] == request['configuration']
    assert call('inspect')['result'] == prepared
    empty = call('read_state')['result']
    assert empty['backend'] == 'sqlite' and empty['state'] is None
    assert not (root / 'state/state.sqlite3').exists()
    (root / 'state').mkdir()
    # A complete real model_io snapshot uses optional defaults for unused fields.
    state = dict(meta={}, system_prompt={}, tools=[], turns=[],
                 extras={'large': 'x' * (10 * 1024 * 1024)})
    (root / 'state/state.json').write_text(json.dumps(state))
    assert call('read_state')['result']['state'] == state
    (root / 'state/readable.md').write_bytes(b'\xff damaged UTF-8')
    assert call('read_state', expected=False)['error']['code'] == 'invalid_data'
    (root / 'state/readable.md').unlink()
    call('remove')
    assert not root.exists()
    call('read_state', expected=False)
print('manager CLI: structured errors, routing and complete 10 MiB stdout passed')
