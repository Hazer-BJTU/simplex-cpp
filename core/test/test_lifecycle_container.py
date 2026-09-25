"""Actual worker recovery, process I/O shutdown and exclusive ownership in Docker."""
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

if not Path('/.dockerenv').exists():
    sys.exit(77)


def exact(sock, size):
    data = b''
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError()
        data += part
    return data


def receive(sock):
    head = exact(sock, 2)
    size = head[1] & 127
    if size == 126:
        size = struct.unpack('!H', exact(sock, 2))[0]
    elif size == 127:
        size = struct.unpack('!Q', exact(sock, 8))[0]
    mask = exact(sock, 4) if head[1] & 128 else None
    data = exact(sock, size)
    if mask:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return head[0] & 15, data


def send(sock, data, opcode=1):
    data = json.dumps(data).encode() if opcode == 1 else data
    size = len(data)
    header = bytes([128 | opcode])
    if size < 126:
        header += bytes([size])
    elif size < 65536:
        header += bytes([126]) + struct.pack('!H', size)
    else:
        header += bytes([127]) + struct.pack('!Q', size)
    sock.sendall(header + data)


class Peer:
    def __init__(self, sock):
        self.sock = sock
        self.events = queue.Queue()

    def wait(self, name):
        while True:
            value = self.events.get(timeout=8)
            if isinstance(value, Exception):
                raise value
            if value.get('event') == name:
                return value

    def run(self, name, arguments):
        send(self.sock, {'type': 'payload', 'data': {
            'request_id': os.urandom(8).hex(), 'operation': 'message',
            'content': [{'type': 'text',
                         'raw': json.dumps({'name': name, 'arguments': arguments})}]}})
        results = self.wait('tool_results')
        done = self.wait('run_finished')
        assert done['data']['durable'], done
        return json.dumps(results)

    def stop(self):
        send(self.sock, {'type': 'signal', 'data': {'operation': 'shutdown'}})


class Router:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.peers = queue.Queue()
        self.sockets = []
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        try:
            while True:
                sock, _ = self.listener.accept()
                self.sockets.append(sock)
                threading.Thread(target=self.serve, args=(sock,), daemon=True).start()
        except OSError:
            pass

    def serve(self, sock):
        peer = None
        try:
            header = b''
            while not header.endswith(b'\r\n\r\n'):
                header += exact(sock, 1)
            key = re.search(br'Sec-WebSocket-Key: ([^\r]+)', header, re.I).group(1)
            accept = base64.b64encode(hashlib.sha1(
                key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
            sock.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                         b'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
            if b'/confirm ' in header.split(b'\r\n')[0]:
                _, raw = receive(sock)
                data = json.loads(raw)['data']
                assert data['worker_id']
                data['decision'] = 'approved'
                send(sock, {'type': 'confirmation_response', 'data': data})
            else:
                peer = Peer(sock)
                self.peers.put(peer)
            while True:
                opcode, data = receive(sock)
                if opcode == 8:
                    send(sock, data, 8)
                    return
                if opcode == 9:
                    send(sock, data, 10)
                elif opcode == 1 and peer:
                    peer.events.put(json.loads(data))
        except (EOFError, OSError) as error:
            if peer:
                peer.events.put(error)
        finally:
            sock.close()


class Provider(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if request['messages'][-1]['role'] == 'tool':
            delta, finish = {'role': 'assistant', 'content': 'done'}, 'stop'
        else:
            content = next(m['content'] for m in reversed(request['messages']) if m['role'] == 'user')
            if isinstance(content, list):
                content = ''.join(c.get('text', '') for c in content)
            call = json.loads(content)
            delta = {'role': 'assistant', 'tool_calls': [{
                'index': 0, 'id': os.urandom(8).hex(), 'type': 'function',
                'function': {'name': call['name'], 'arguments': json.dumps(call['arguments'])}}]}
            finish = 'tool_calls'
        frames = [{'id': 'fixture', 'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]},
                  {'id': 'fixture', 'choices': [{'index': 0, 'delta': {}, 'finish_reason': finish}]}]
        body = (''.join('data: ' + json.dumps(f) + '\n\n' for f in frames) + 'data: [DONE]\n\n').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


router = Router()
provider = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
threading.Thread(target=provider.serve_forever, daemon=True).start()
workers = []
children = []
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    config = {
        'providers': {'fixture': {'plugin': 'deepseek', 'model': 'fixture',
            'endpoint': {'base_url': f'http://127.0.0.1:{provider.server_port}', 'auth': {'scheme': 'none'}},
            'retry': {'max_attempts': 0}}},
        'driver_model': 'fixture',
        'client': {'endpoint': f'ws://127.0.0.1:{router.port}/events'},
        'security': {'confirmation': {'endpoint': f'ws://127.0.0.1:{router.port}/confirm'}},
        'persistence': {'directory': str(root / 'sessions')},
    }
    path = root / 'config.yaml'
    path.write_text(json.dumps(config))

    def start(session='same', ready=True):
        log = (root / f'worker-{len(workers)}.log').open('w+')
        process = subprocess.Popen([sys.argv[1], '--config', str(path), '--session', session],
                                   stdout=log, stderr=log)
        workers.append((process, log))
        if not ready:
            return process, log
        peer = router.peers.get(timeout=8)
        peer.wait('ready')
        return process, peer

    def failed(process, log):
        assert process.wait(timeout=5) != 0
        log.seek(0)
        return log.read()

    def shutdown(process, peer):
        peer.stop()
        assert process.wait(timeout=3) == 0

    try:
        # Keep quick-command sessions for the cross-worker stale-ID checks.
        first, peer = start()
        result = peer.run('spawn_process', {'executable': '/bin/sh', 'arguments': ['-c', 'printf FIRST'], 'expected_runtime_milliseconds': 1000, 'auto_release': False})
        old = re.search(r'proc_[0-9a-f-]+_\d+', result).group()
        snapshot = root / 'sessions/same/state.json'
        original = snapshot.read_bytes()
        duplicate, log = start(ready=False)
        assert 'exclusive session ownership' in failed(duplicate, log)
        assert snapshot.read_bytes() == original
        independent, other = start('different')
        shutdown(independent, other)
        shutdown(first, peer)

        second, peer = start()
        result = peer.run('spawn_process', {'executable': '/bin/sh', 'arguments': ['-c', 'printf SECOND'], 'expected_runtime_milliseconds': 1000, 'auto_release': False})
        new = re.search(r'proc_[0-9a-f-]+_\d+', result).group()
        assert old != new
        stale = peer.run('read_process', {'session_id': old, 'full': True})
        assert 'SECOND' not in stale and ('not found' in stale or 'unknown' in stale or 'not_found' in stale or 'no such' in stale.lower() or 'no live process session' in stale), stale
        current = peer.run('read_process', {'session_id': new, 'full': True})
        assert 'SECOND' in current, current
        assert old in snapshot.read_text() and new in snapshot.read_text()
        pidfile = root / 'descendant.pid'
        peer.run('run_command', {'command': f'sleep 30 & echo $! > {pidfile}',
                                'expected_runtime_milliseconds': 1000})
        child = int(pidfile.read_text())
        children.append(child)
        shutdown(second, peer) # Must exit before the inherited pipe reaches EOF.
        os.kill(child, 0)

        # Crash with an executed child still alive: CLOEXEC must prevent that
        # child from retaining ownership of the parent's session lock.
        owner, peer = start()
        peer.run('run_command', {'command': f'sleep 30 & echo $! > {pidfile}',
                                'expected_runtime_milliseconds': 1000})
        children.append(int(pidfile.read_text()))
        owner.kill()
        owner.wait(timeout=3)
        successor, peer = start()
        shutdown(successor, peer)

        # Startup failure releases the lock before a repaired session retries.
        bad = root / 'sessions/broken'
        bad.mkdir()
        (bad / 'state.json').write_text('invalid json')
        broken, log = start('broken', ready=False)
        failed(broken, log)
        (bad / 'state.json').unlink()
        repaired, peer = start('broken')
        shutdown(repaired, peer)
        print('worker ownership, recovery non-aliasing, and pipe lifetime: passed')
    finally:
        for process, log in workers:
            if process.poll() is None:
                process.kill()
                process.wait()
            log.close()
        for child in children:
            try:
                os.kill(child, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for sock in router.sockets:
            sock.close()
        router.listener.close()
        provider.shutdown()
        provider.server_close()
