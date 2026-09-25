/**
 * @file worker event envelope parsing: tolerant where the protocol says to be
 * tolerant, strict about the fields the hub correlates on.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import {
    EVENT_TABLE,
    KNOWN_EVENTS,
    isKnownEvent,
    parseEventEnvelope,
    readUnsignedInteger,
} from '../src/protocol/events.js';
import { workerEvent } from './helpers/worker.js';

describe('event vocabulary', () => {
    it('covers every event core documents', () => {
        // Mirrors the "Worker events" table in core/docs/worker-protocol.md.
        const documented = [
            'ready', 'status', 'options', 'input_admitted', 'input_rejected',
            'run_started', 'input_committed', 'model_response', 'tool_calls',
            'tool_results', 'persisted', 'export_error', 'error', 'run_finished',
        ];
        for (const name of documented) {
            assert.ok(KNOWN_EVENTS.includes(name), `missing event: ${name}`);
            assert.ok(isKnownEvent(name));
        }
        assert.equal(KNOWN_EVENTS.length, documented.length);
    });

    it('marks array-carrying events', () => {
        assert.equal(EVENT_TABLE.tool_calls.array, true);
        assert.equal(EVENT_TABLE.tool_results.array, true);
        assert.equal(EVENT_TABLE.model_response.array, undefined);
    });
});

describe('parseEventEnvelope', () => {
    it('accepts a complete envelope and preserves the raw document', () => {
        const raw = workerEvent({
            event: 'model_response',
            requestId: 'req-1',
            runId: 'run-1',
            data: { type: 'model_response', content: [{ type: 'text', raw: 'hi' }] },
            extra: { future_field: { nested: true } },
        });
        const parsed = parseEventEnvelope(JSON.stringify(raw));
        assert.equal(parsed.ok, true);
        assert.deepEqual(parsed.issues, []);
        assert.equal(parsed.envelope.event, 'model_response');
        assert.equal(parsed.envelope.session_id, 'demo');
        assert.equal(parsed.envelope.request_id, 'req-1');
        assert.equal(parsed.envelope.run_id, 'run-1');
        assert.equal(parsed.envelope.sequence, 1);
        assert.equal(parsed.envelope.known, true);
        assert.deepEqual(parsed.envelope.raw, raw);
        assert.deepEqual(parsed.envelope.data.content, [{ type: 'text', raw: 'hi' }]);
    });

    it('accepts an empty object and an array as data', () => {
        const empty = parseEventEnvelope(JSON.stringify(workerEvent({ event: 'run_started' })));
        assert.deepEqual(empty.envelope.data, {});
        const array = parseEventEnvelope(JSON.stringify(workerEvent({
            event: 'tool_calls',
            data: [{ id: 'call-1', name: 'run_command' }],
        })));
        assert.ok(Array.isArray(array.envelope.data));
    });

    it('keeps an unknown event instead of rejecting it', () => {
        const parsed = parseEventEnvelope(JSON.stringify(workerEvent({
            event: 'hypothetical_future_event',
            data: { anything: true },
        })));
        assert.equal(parsed.ok, true);
        assert.equal(parsed.envelope.known, false);
        assert.deepEqual(parsed.envelope.data, { anything: true });
    });

    it('reports a missing correlation field as an issue, not as a parse failure', () => {
        const parsed = parseEventEnvelope(JSON.stringify({
            type: 'event',
            event: 'status',
            session_id: 'demo',
            worker_id: 'worker-1',
            data: {},
        }));
        assert.equal(parsed.ok, true);
        assert.ok(parsed.issues.some((issue) => issue.includes('sequence')));
        assert.ok(parsed.issues.some((issue) => issue.includes('request_id')));
        assert.ok(parsed.issues.some((issue) => issue.includes('run_id')));
        assert.equal(parsed.envelope.request_id, '');
        assert.equal(parsed.envelope.sequence, null);
        assert.deepEqual(parsed.envelope.data, {});
    });

    it('rejects an envelope that carries no identity', () => {
        // Identity is not optional: a confirmation may only be judged against a
        // worker the event connection has actually named.
        const parsed = parseEventEnvelope('{"type":"event","event":"status","data":{}}');
        assert.equal(parsed.ok, false);
        assert.match(parsed.error, /worker_id/);
    });

    it('rejects a non-event envelope, non-JSON, and arrays', () => {
        assert.equal(parseEventEnvelope('{"type":"payload","data":{}}').ok, false);
        assert.equal(parseEventEnvelope('not json').ok, false);
        assert.equal(parseEventEnvelope(JSON.stringify([1, 2])).ok, false);
        assert.match(parseEventEnvelope('{').error, /invalid JSON/);
    });

    it('rejects an empty event name and an invalid session id', () => {
        assert.match(
            parseEventEnvelope(JSON.stringify(workerEvent({ event: '' }))).error,
            /event name/);
        assert.match(
            parseEventEnvelope(JSON.stringify(workerEvent({ session: '../etc' }))).error,
            /session_id/);
        assert.match(
            parseEventEnvelope(JSON.stringify(workerEvent({ session: 'x'.repeat(129) }))).error,
            /session_id/);
    });

    it('rejects an empty worker id', () => {
        assert.match(
            parseEventEnvelope(JSON.stringify(workerEvent({ worker: '' }))).error,
            /worker_id/);
    });

    it('normalizes non-string correlation fields to empty strings', () => {
        const parsed = parseEventEnvelope(JSON.stringify(workerEvent({
            requestId: null,
            runId: 42,
        })));
        assert.equal(parsed.envelope.request_id, '');
        assert.equal(parsed.envelope.run_id, '');
    });
});

describe('readUnsignedInteger', () => {
    it('accepts a plain integer', () => {
        assert.deepEqual(readUnsignedInteger(7, '{"sequence":7}', 'sequence'), {
            value: 7n, safe: true, display: 7,
        });
    });

    it('recovers a 64-bit value that JSON.parse rounded', () => {
        const text = '{"sequence":18446744073709551615}';
        const parsed = JSON.parse(text).sequence;
        assert.equal(Number.isSafeInteger(parsed), false);
        const read = readUnsignedInteger(parsed, text, 'sequence');
        assert.equal(read.value, 18446744073709551615n);
        assert.equal(read.safe, false);
        assert.equal(read.display, '18446744073709551615');
    });

    it('accepts a decimal string as a fallback', () => {
        const read = readUnsignedInteger('9007199254740993', '{}', 'sequence');
        assert.equal(read.value, 9007199254740993n);
        assert.equal(read.display, '9007199254740993');
    });

    it('reports an unusable value instead of inventing one', () => {
        assert.equal(readUnsignedInteger(-1, '{}', 'sequence').value, null);
        assert.equal(readUnsignedInteger('abc', '{}', 'sequence').value, null);
        assert.equal(readUnsignedInteger(undefined, '{"sequence":"x"}', 'sequence').value, null);
    });
});
