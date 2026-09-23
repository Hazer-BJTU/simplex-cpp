"""Keyless end-to-end checks; real shell calls are permitted only in Docker.

The local SSE fixture tests the plugin, host prompts, loop continuation, tool
results and terminal rendering together. No request reaches a public provider.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

if not Path('/.dockerenv').exists():
    print('SKIP: this example executes real process tools; run inside Docker')
    sys.exit(77)

binary = sys.argv[1]


def scenario(mode):
    received = threading.Event()
    release = threading.Event()
    requests = []
    failures = []

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                requests.append(request)
                received.set()
                if mode == 'cancel':
                    # Keep the HTTP header wait suspended until the client stops.
                    release.wait(10)
                    return
                messages = request['messages']
                results = [item for item in messages if item['role'] == 'tool']
                if not results:
                    delta = {'role': 'assistant', 'tool_calls': [{
                        'index': 0, 'id': 'call_demo', 'type': 'function',
                        'function': {'name': 'run_command', 'arguments': json.dumps({
                            'command': "printf 'stdout-marker\\n'; printf '\\033[31mstderr-marker\\n' >&2",
                            'expected_runtime_milliseconds': 1000,
                        })},
                    }]}
                    finish = 'tool_calls'
                else:
                    content = str(results[0]['content'])
                    if mode == 'deny':
                        assert 'denied' in content.lower(), content
                    else:
                        assert 'stdout-marker' in content and 'stderr-marker' in content, content
                        assert len([m for m in messages if m['role'] == 'user']) == 1
                    delta = {'role': 'assistant', 'content': 'fixture-final-answer',
                             'reasoning_content': 'fixture-reasoning'}
                    finish = 'stop'
                frames = [
                    {'id': 'fixture', 'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]},
                    {'id': 'fixture', 'choices': [{'index': 0, 'delta': {}, 'finish_reason': finish}]},
                ]
                body = ''.join('data: ' + json.dumps(frame) + '\n\n' for frame in frames)
                body = (body + 'data: [DONE]\n\n').encode()
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as error:
                failures.append(error)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    process = None
    try:
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, DEEPSEEK_API_KEY='local-fixture-only',
                       DEEPSEEK_BASE_URL=f'http://127.0.0.1:{server.server_port}')
            args = [binary, '--log', str(Path(directory) / 'errors.log'), '--reasoning']
            if mode == 'continue':
                args += ['--yes', '--max-steps', '1']
            process = subprocess.Popen(args, env=env, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if mode == 'cancel':
                process.stdin.write('hello\n')
                process.stdin.flush()
                assert received.wait(5), 'model request never arrived'
                process.send_signal(signal.SIGINT)
                output, errors = process.communicate('/quit\n', timeout=10)
                assert 'Run cancelled' in output, output
                assert len(requests) == 1, 'cancelled request retried'
            else:
                lines = 'hello\n/continue\n/sessions\n/quit\n' if mode == 'continue' else 'hello\nn\n/quit\n'
                output, errors = process.communicate(lines, timeout=15)
                assert 'fixture-final-answer' in output, output
                assert len(requests) == 2, len(requests)
                if mode == 'continue':
                    assert 'Run step limit' in output and 'Run completed' in output, output
                    assert 'stdout (' in output and 'stderr (' in output, output
                    assert '\\x1b[31mstderr-marker' in output and '\x1b' not in output, output
                else:
                    assert 'Tool FAILED' in output, output
            assert process.returncode == 0, (process.returncode, output, errors)
            assert not errors, errors
            assert not failures, failures
            print(f'{mode}: passed')
    finally:
        release.set()
        if process is not None and process.poll() is None:
            process.kill()
            process.communicate()
        server.shutdown()
        server.server_close()
        worker.join()


for mode in ('continue', 'deny', 'cancel'):
    scenario(mode)
