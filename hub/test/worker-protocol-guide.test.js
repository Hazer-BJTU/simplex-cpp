import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { contractValidator, manifest, validateWorkerEvent } from './helpers/worker-contract.js';

const guide = readFileSync(new URL('../../docs/core/worker-protocol.md', import.meta.url), 'utf8');

test('worker guide identifies the manifest and lists every operation and event', () => {
    for (const value of [manifest.protocol, manifest.manifest_format, manifest.fingerprint.slice(0, 12)]) {
        assert.ok(guide.includes(`\`${value}\``), value);
    }
    for (const name of ['InputOperation', 'SignalOperation', 'EventName']) {
        for (const value of manifest.enumerations[name].spellings) {
            assert.ok(guide.includes(`\`${value}\``), value);
        }
    }
});

test('worker guide illustrates a correlated run with a matching approval', () => {
    const frames = [...guide.matchAll(/^```json\n([\s\S]*?)^```$/gm)]
        .map(match => JSON.parse(match[1]));
    const [payload, admitted, prompt, reply, finished] = frames;
    assert.equal(frames.length, 5);
    const check = contractValidator();
    check('PayloadEnvelope', payload);
    check('ConfirmationRequestEnvelope', prompt);
    check('ConfirmationResponseEnvelope', reply);
    for (const event of [admitted, finished]) {
        validateWorkerEvent(event, check);
        assert.equal(event.request_id, payload.data.request_id);
        for (const field of ['session_id', 'worker_id', 'run_id']) {
            assert.equal(event[field], prompt.data[field]);
            assert.equal(event[field], reply.data[field]);
        }
    }
    assert.equal(admitted.event, 'input_admitted');
    assert.equal(finished.event, 'run_finished');
    assert.equal(prompt.data.confirmation_id, reply.data.confirmation_id);
});
