import test from 'node:test';
import assert from 'node:assert/strict';
import { setTimeout as delay } from 'node:timers/promises';
import { serializeMutation, retryManagerBusy, mutationNamespace } from '../src/launch/managed-mutations.ts';
import { ManagementError } from '../src/launch/management.ts';

test('namespace sequences serialize; unrelated namespaces remain independent', async () => {
    const order = [];
    await Promise.all([1, 2, 3, 4].map(id => serializeMutation('same', 1000, async () => {
        order.push(`start${id}`);
        await delay(10);
        order.push(`end${id}`);
    })));
    assert.deepEqual(order, [1, 2, 3, 4].flatMap(id => [`start${id}`, `end${id}`]));
    let release;
    const held = serializeMutation('held', 1000, () => new Promise(resolve => { release = resolve; }));
    await delay(0);
    assert.equal(await serializeMutation('other', 100, async () => 42), 42);
    release(); await held;
});

test('expired queued mutations never execute and a failed sequence releases its queue', async () => {
    let release;
    const held = serializeMutation('expiry', 1000, () => new Promise(resolve => { release = resolve; }));
    await delay(0);
    await assert.rejects(serializeMutation('expiry', 15, async () => assert.fail('expired work ran')), /cancelled/);
    release(); await held;
    await assert.rejects(serializeMutation('expiry', 100, async () => { throw new Error('failure'); }), /failure/);
    assert.equal(await serializeMutation('expiry', 100, async () => 'recovered'), 'recovered');
});

test('busy retry is bounded and excludes published, transport, protocol and other manager errors', async () => {
    let attempts = 0;
    assert.equal(await retryManagerBusy(async () => {
        if (++attempts < 3) throw new ManagementError('manager', 'busy');
        return 'ok';
    }, new AbortController().signal), 'ok');
    assert.equal(attempts, 3);
    for (const error of [new ManagementError('transport', 'busy'), new ManagementError('protocol', 'busy'),
        new ManagementError('manager', 'conflict'), new ManagementError('manager', 'busy', '', null, true)]) {
        let calls = 0;
        await assert.rejects(retryManagerBusy(async () => { ++calls; throw error; }, new AbortController().signal), e => e === error);
        assert.equal(calls, 1);
    }
    attempts = 0;
    await assert.rejects(retryManagerBusy(async () => {
        ++attempts; throw new ManagementError('manager', 'busy');
    }, new AbortController().signal), /busy/);
    assert.equal(attempts, 4);
});

test('local namespace identity ignores binary/env while remote targets remain distinct', () => {
    const launcher = { target: { kind: 'local' }, executable: 'simplex', cwd: '/', env: {} };
    assert.equal(mutationNamespace(launcher, '/tmp/root/a'), mutationNamespace({ ...launcher, executable: '/opt/simplex', env: { A: 'x' } }, '/tmp/root/b'));
    const remote = { ...launcher, target: { kind: 'docker', command: 'docker', args: [], container: 'one' } };
    assert.notEqual(mutationNamespace(remote, '/root/a'), mutationNamespace({ ...remote, target: { ...remote.target, container: 'two' } }, '/root/a'));
});

test('active deadline reaches transport and holds the queue through abort settlement', async () => {
    let settled = false;
    const first = serializeMutation('active-deadline', 10, signal => new Promise((resolve, reject) => {
        signal.addEventListener('abort', () => {
            setTimeout(() => { settled = true; reject(new ManagementError('transport', 'cancelled')); }, 20);
        }, { once: true });
    }));
    const failure = assert.rejects(first, /cancelled/);
    const second = serializeMutation('active-deadline', 1000, async () => assert.equal(settled, true));
    await Promise.all([failure, second]);
});
