import assert from 'node:assert/strict';
import { it } from 'node:test';
import { approvalSnapshot } from '../src/protocol/approval-snapshot.ts';
import { approvalArgumentPreview } from '../src/protocol/approval-preview.ts';

const bytes = value => Buffer.byteLength(JSON.stringify(value));

it('allocates against the complete encoded frame, not just its argument total', () => {
    const argumentsValue = { command: '漢字😀\n"'.repeat(20000), cwd: '/workspace' };
    const initial = approvalArgumentPreview(argumentsValue);
    const prompt = { confirmation_id: 'prompt', session_id: 'session', worker_id: 'worker', run_id: 'run',
        state: 'awaiting-decision', verified: true, arguments_truncated: true,
        arguments_bytes: initial.originalBytes, call: { arguments: initial.value } };
    const source = { v: 1, type: 'subscribed', session: { session_id: 'session', confirmations: [prompt] },
        transcript: [{ message: 'x'.repeat(60000) }], replay_more: false };
    let regenerationBudget;
    const result = approvalSnapshot(source, 64 * 1024, (original, maximum) => {
        regenerationBudget = maximum;
        const preview = approvalArgumentPreview(argumentsValue, maximum);
        return { ...original, arguments_bytes: preview.originalBytes, call: { arguments: preview.value } };
    });
    assert.ok(regenerationBudget < 6000);
    assert.ok(bytes(result) <= 64 * 1024);
    const shortened = result.session.confirmations[0];
    assert.equal(shortened.arguments_bytes, initial.originalBytes);
    assert.equal(shortened.call.arguments.command.display_truncated, true);
    assert.equal(shortened.call.arguments.command.bytes, Buffer.byteLength(argumentsValue.command));
    assert.equal(typeof shortened.call.arguments.command.preview, 'string', 'never nest preview markers');
    assert.equal(shortened.call.arguments.cwd, '/workspace');
    assert.deepEqual(result.transcript, source.transcript);
    assert.deepEqual(source.session.confirmations[0].call.arguments, initial.value);
});

it('retains approval identity and state when metadata alone exceeds the frame ceiling', () => {
    const prompt = { confirmation_id: 'prompt', session_id: 'session', worker_id: 'worker', run_id: 'run',
        state: 'awaiting-decision', verified: true, call: { arguments: { command: 'x'.repeat(10000) } } };
    const source = { type: 'sessions', sessions: [{ session_id: 'session', spec: { note: 'x'.repeat(10000) },
        confirmations: [prompt] }] };
    const result = approvalSnapshot(source, 1000, () => { throw new Error('no regeneration should fit'); });
    const retained = result.sessions[0].confirmations[0];
    assert.equal(retained.confirmation_id, prompt.confirmation_id);
    assert.equal(retained.state, prompt.state);
    assert.equal(retained.verified, true);
    assert.deepEqual(retained.call.arguments, { display_omitted: true });
    assert.ok(bytes(result) > 1000, 'the caller must keep its hard frame guard');
});

it('preserves small snapshots and single-prompt broadcasts without regeneration', () => {
    const prompt = { call: { arguments: { command: 'echo ok' } } };
    for (const source of [
        { type: 'welcome', hub: {}, sessions: [{ confirmations: [prompt] }] },
        { type: 'confirmation', confirmation: prompt },
    ]) {
        const result = approvalSnapshot(source, 2 * 1024 * 1024,
            () => { throw new Error('fitting arguments must stay unchanged'); });
        assert.deepEqual(result, source);
    }
});
