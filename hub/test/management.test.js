/** Management transport regressions; subprocess fixtures never access worker data. */
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';
import { test } from 'node:test';
import { ManagedLauncher } from '../src/launch/managed-launcher.ts';
import {
    encodeManagementRequest, executeManagement, ManagementError,
} from '../src/launch/management.ts';

const reference = { allocationId: 'demo-1', directory: '/target only/allocations/demo-1' };
const request = { version: 1, request_id: 'request-1', allocation_id: reference.allocationId,
    directory: reference.directory, operation: 'inspect' };
const success = { version: 1, request_id: request.request_id, ok: true, result: { status: 'ready' } };
const base = { executable: '/opt/simplex install/bin/simplex', cwd: tmpdir(), env: { FIXTURE: 'value' } };

function child(code, args = []) {
    return { command: process.execPath, args: ['-e', code, ...args], cwd: tmpdir(), env: {} };
}

function emits(value, exitCode = 0) {
    return child(`process.stdout.write(${JSON.stringify(JSON.stringify(value))}); process.exitCode = ${exitCode};`);
}

async function rejects(invocation, kind, code) {
    await assert.rejects(executeManagement(invocation, request.request_id), error => {
        assert.ok(error instanceof ManagementError);
        assert.equal(error.kind, kind);
        assert.equal(error.code, code);
        assert.equal(error.outcome, kind === 'manager' ? 'reported' : 'unknown');
        return true;
    });
}

test('management local command encodes Unicode source verbatim, without resolving target paths', () => {
    const launcher = new ManagedLauncher({ ...base, target: { kind: 'local' } });
    const input = { ...request, operation: 'prepare', configuration: '# 注释\nunknown: "秘密"\n' };
    const invocation = launcher.buildManagementInvocation(input);
    assert.equal(invocation.command, base.executable);
    assert.equal(invocation.cwd, base.cwd);
    assert.deepEqual(invocation.args.slice(0, 2), ['manage', '--request']);
    assert.deepEqual(JSON.parse(Buffer.from(invocation.args[2], 'base64').toString('utf8')), input);
    assert.deepEqual(launcher.buildWorkerInvocation(reference, `${reference.directory}/config/config.yaml`,
        ['--session', 'demo', '--threads', '2']).args,
    ['run', '--config', `${reference.directory}/config/config.yaml`, '--session', 'demo', '--threads', '2']);
});

test('SSH uses no PTY and POSIX quoting preserves each argument without executing metacharacters', () => {
    const executable = "/remote/it's a path/$(touch SHOULD_NOT_EXIST)/simplex";
    const options = { ...base, executable,
        target: { kind: 'ssh', command: 'ssh', args: ['-p', '2222', '-i', '/keys/id', '-tt'], host: 'user@remote' } };
    const launcher = new ManagedLauncher(options);
    const invocation = launcher.buildManagementInvocation(request);
    assert.deepEqual(invocation.args.slice(0, -1), ['-p', '2222', '-i', '/keys/id', '-tt', '-T', '--', 'user@remote']);
    // Parse using an actual POSIX shell, but only print the resulting argv.
    const parse = command => execFileSync('/bin/sh', ['-c', `set -- ${command}; printf '%s\\0' "$@"`]);
    assert.deepEqual(parse(invocation.args.at(-1)).toString().split('\0').slice(0, -1),
        [executable, 'manage', '--request', encodeManagementRequest(request)]);
    const run = launcher.buildWorkerInvocation(reference, `${reference.directory}/config/config.yaml`, ['--session', "a'b"]);
    assert.deepEqual(parse(run.args.at(-1)).toString().split('\0').slice(0, -1),
        [executable, 'run', '--config', `${reference.directory}/config/config.yaml`, '--session', "a'b"]);
    options.target.host = 'different-target';
    options.env.FIXTURE = 'changed';
    assert.equal(launcher.buildManagementInvocation(request).args.at(-2), 'user@remote');
    assert.equal(launcher.buildManagementInvocation(request).env.FIXTURE, 'value');
});

test('Docker manager and worker use the same explicit context and existing container, without TTY or bind mounts', () => {
    const launcher = new ManagedLauncher({ ...base,
        target: { kind: 'docker', command: '/usr/bin/docker', args: ['--context', 'remote'], container: 'worker-1' } });
    const invocation = launcher.buildManagementInvocation(request);
    assert.deepEqual(invocation.args, ['--context', 'remote', 'exec', 'worker-1', base.executable,
        'manage', '--request', encodeManagementRequest(request)]);
    const run = launcher.buildWorkerInvocation(reference, `${reference.directory}/config/config.yaml`, []);
    assert.deepEqual(run.args.slice(0, 5), invocation.args.slice(0, 5));
    assert.equal(run.command, invocation.command);
    assert.equal(run.cwd, base.cwd);
});

test('invalid allocation identities, non-normalized paths and unsafe destinations fail before spawning', () => {
    for (const directory of ['relative', '/', '/x/../y', '/x/./y', '/x//y', '/x/', '/x\0y']) {
        assert.throws(() => encodeManagementRequest({ ...request, directory }), /normalized absolute/);
    }
    for (const allocation_id of ['', '../other', 'a'.repeat(129)]) {
        assert.throws(() => encodeManagementRequest({ ...request, allocation_id }), /identity/);
    }
    assert.throws(() => encodeManagementRequest({ ...request, request_id: 'with spaces' }), /identity/);
    assert.throws(() => new ManagedLauncher({ ...base,
        target: { kind: 'ssh', command: 'ssh', args: [], host: '-oProxyCommand=bad' } }), /transport/);
    const launcher = new ManagedLauncher({ ...base, target: { kind: 'local' } });
    assert.throws(() => launcher.buildWorkerInvocation(reference, '/unrelated/config.yaml', []), /allocation/);
    for (const flag of ['--config', '--config=/other', '-c', '-c/other', '--conf=/other']) {
        assert.throws(() => launcher.buildWorkerInvocation(reference,
            `${reference.directory}/config/config.yaml`, [flag]), /override/);
    }
});

test('large stdout and stderr are drained concurrently, without exec maxBuffer limits', async () => {
    const result = await executeManagement(child(`
        const { once } = require('node:events');
        async function write(stream, text) {
            for (let i = 0; i < 160; ++i) {
                if (!stream.write(text)) await once(stream, 'drain');
            }
        }
        (async () => {
            process.stdout.write('{"version":1,"request_id":"request-1","ok":true,"result":{"text":"');
            await Promise.all([write(process.stdout, 'x'.repeat(65536)), write(process.stderr, 'y'.repeat(65536))]);
            process.stdout.write('"}}');
        })();
    `), request.request_id);
    assert.equal(result.result.text.length, 10 * 1024 * 1024);
    assert.equal(result.diagnostics.length, 10 * 1024 * 1024);
});

test('UTF-8 split across pipe chunks is decoded only after complete collection', async () => {
    const result = await executeManagement(child(`
        const bytes = Buffer.from(JSON.stringify({version:1,request_id:'request-1',ok:true,result:{text:'🌍你好'}}));
        let i = 0;
        const timer = setInterval(() => {
            if (i === bytes.length) clearInterval(timer);
            else process.stdout.write(bytes.subarray(i, ++i));
        }, 1);
    `), request.request_id);
    assert.equal(result.result.text, '🌍你好');
});

test('success is not accepted until process exit, including success followed by a failed exit', async () => {
    await rejects(emits(success, 7), 'transport', 'nonzero_exit');
    await rejects(child(`process.stdout.write(${JSON.stringify(JSON.stringify(success))}); process.kill(process.pid, 'SIGTERM');`),
        'transport', 'terminated');
});

test('partial, additional and malformed responses cannot turn into empty state or successful deletion', async () => {
    for (const response of ['', '{', JSON.stringify(success) + '\n{}', 'diagnostic\n' + JSON.stringify(success)]) {
        await rejects(child(`process.stdout.write(${JSON.stringify(response)});`), 'protocol', 'invalid_response');
    }
    await rejects(child("process.stdout.write(Buffer.from([0xff]));"), 'protocol', 'invalid_response');
    for (const invalid of [{ ...success, request_id: 'other' }, { ...success, version: 2 }, [], null]) {
        await rejects(emits(invalid), 'protocol', 'invalid_envelope');
    }
    for (const result of [null, [], 'ready']) {
        await rejects(emits({ ...success, result }), 'protocol', 'invalid_result');
    }
});

test('manager errors preserve private details and publication uncertainty but sanitize public messages', async () => {
    const error = { code: 'io_error', message: 'SECRET_CONFIGURATION', published: true };
    await assert.rejects(executeManagement(emits({ version: 1, request_id: request.request_id, ok: false, error }, 1),
        request.request_id), failure => {
        assert.equal(failure.kind, 'manager');
        assert.equal(failure.published, true);
        assert.equal(failure.detail, 'SECRET_CONFIGURATION');
        assert.doesNotMatch(failure.message, /SECRET_CONFIGURATION/);
        return true;
    });
    await rejects(emits({ version: 1, request_id: request.request_id, ok: false, error }), 'protocol', 'invalid_error');
    await rejects(emits({ version: 1, request_id: request.request_id, ok: false,
        error: { code: 'SECRET with spaces', message: 'detail' } }, 1), 'protocol', 'invalid_error');
});

test('missing executable and transport exit are explicit failures with no embedded argv secrets', async () => {
    const invocation = { ...child(''), command: '/missing/simplex-manager-SECRET', args: ['SECRET'] };
    await rejects(invocation, 'transport', 'spawn_failed');
    await rejects(child("process.stderr.write('ssh: connection lost'); process.exitCode=255;"), 'transport', 'invalid_response');
    await assert.rejects(executeManagement(invocation, request.request_id), error => !error.message.includes('SECRET'));
});

test('lost mutation reply is uncertain, retains published data, and is never retried automatically', async t => {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-management-'));
    t.after(() => rmSync(directory, { recursive: true, force: true }));
    const marker = join(directory, 'mutation');
    await rejects(child(`require('node:fs').appendFileSync(process.argv[1], 'published\\n');
        process.stdout.write('{'); process.exitCode=255;`, [marker]), 'transport', 'invalid_response');
    assert.equal(readFileSync(marker, 'utf8'), 'published\n');
});

test('manage facade invokes simplex manage exactly once with complete operation and reference', async t => {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-management-'));
    t.after(() => rmSync(directory, { recursive: true, force: true }));
    const executable = join(directory, 'simplex');
    writeFileSync(executable, `#!${process.execPath}\n
        const assert = require('node:assert/strict');
        assert.deepEqual(process.argv.slice(2,4), ['manage','--request']);
        const request = JSON.parse(Buffer.from(process.argv[4], 'base64').toString('utf8'));
        process.stdout.write(JSON.stringify({version:1,request_id:request.request_id,ok:true,result:request}));
    `, { mode: 0o700 });
    const launcher = new ManagedLauncher({ ...base, executable, target: { kind: 'local' } });
    const reply = await launcher.manage(reference, 'capture-1', {
        operation: 'capture_startup', lifecycle_id: 'life-1', expected_revision: 3,
    });
    assert.deepEqual(reply.result, { version: 1, request_id: 'capture-1',
        allocation_id: reference.allocationId, directory: reference.directory,
        operation: 'capture_startup', lifecycle_id: 'life-1', expected_revision: 3 });
});

test('SSH and Docker adapters execute through configured transport, retaining full target-only state', async t => {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-management-'));
    t.after(() => rmSync(directory, { recursive: true, force: true }));
    const executable = join(directory, 'simplex');
    const transport = join(directory, 'transport');
    writeFileSync(executable, `#!${process.execPath}\n
        const request = JSON.parse(Buffer.from(process.argv.at(-1), 'base64').toString('utf8'));
        process.stderr.write('manager diagnostics');
        process.stdout.write(JSON.stringify({version:1,request_id:request.request_id,ok:true,
            result:{backend:'json',state:{directory:request.directory, text:'x'.repeat(2*1024*1024)},readable:null}}));
    `, { mode: 0o700 });
    writeFileSync(transport, `#!${process.execPath}\n
        const { spawn } = require('node:child_process');
        const assert = require('node:assert/strict');
        const args = process.argv.slice(2);
        let child;
        if (args[0] === '-T') {
            assert.deepEqual(args.slice(0,3), ['-T','--','test-host']);
            child = spawn('/bin/sh', ['-c', args[3]], {stdio:'inherit'});
        } else {
            assert.deepEqual(args.slice(0,2), ['exec','test-container']);
            child = spawn(args[2], args.slice(3), {stdio:'inherit'});
        }
        child.on('error', () => { process.exitCode=127; });
        child.on('exit', code => { process.exitCode=code ?? 1; });
    `, { mode: 0o700 });
    for (const target of [
        { kind: 'ssh', command: transport, args: [], host: 'test-host' },
        { kind: 'docker', command: transport, args: [], container: 'test-container' },
    ]) {
        const launcher = new ManagedLauncher({ ...base, executable, target });
        const reply = await launcher.manage(reference, 'read-1', { operation: 'read_state' });
        assert.equal(reply.result.state.directory, reference.directory);
        assert.equal(reply.result.state.text.length, 2 * 1024 * 1024);
        assert.equal(reply.diagnostics, 'manager diagnostics');
    }
});
