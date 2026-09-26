/**
 * @file outbound message builders: the rules the hub enforces before a payload
 * or signal can reach a worker.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import {
    ProtocolError,
    buildConfirmationResponse,
    buildPayload,
    buildSignal,
    newRequestId,
    normalizeOptions,
    utf8Length,
} from '../src/protocol/messages.ts';

describe('buildPayload', () => {
    it('builds a text message envelope', () => {
        const payload = buildPayload({
            requestId: 'req-1',
            content: [{ type: 'text', raw: 'Hello' }],
        });
        assert.deepEqual(payload, {
            type: 'payload',
            data: {
                operation: 'message',
                request_id: 'req-1',
                content: [{ type: 'text', raw: 'Hello' }],
            },
        });
    });

    it('keeps extras on an external reference and does not invent fields', () => {
        const payload = buildPayload({
            requestId: 'req-2',
            content: [{
                type: 'external_ref',
                raw: 'https://example.com/photo.png',
                extras: { detail: 'low' },
                ignored: true,
            }],
        });
        assert.deepEqual(payload.data.content, [{
            type: 'external_ref',
            raw: 'https://example.com/photo.png',
            extras: { detail: 'low' },
        }]);
    });

    it('explains the image type mistake', () => {
        assert.throws(
            () => buildPayload({ requestId: 'r', content: [{ type: 'image', raw: 'x' }] }),
            /external_ref/);
    });

    it('rejects empty content and non-string raw', () => {
        assert.throws(() => buildPayload({ requestId: 'r', content: [] }), ProtocolError);
        assert.throws(() => buildPayload({ requestId: 'r', content: [{ type: 'text', raw: '' }] }),
            ProtocolError);
        assert.throws(() => buildPayload({ requestId: 'r', content: ['text'] }), ProtocolError);
    });

    it('rejects missing, empty, and oversized request ids', () => {
        assert.throws(() => buildPayload({ requestId: '', content: [{ type: 'text', raw: 'x' }] }),
            ProtocolError);
        assert.throws(() => buildPayload({ content: [{ type: 'text', raw: 'x' }] }), ProtocolError);
        assert.throws(() => buildPayload({
            requestId: 'x'.repeat(129),
            content: [{ type: 'text', raw: 'x' }],
        }), /128/);
    });

    it('counts the request id limit in UTF-8 bytes, not characters', () => {
        assert.equal(utf8Length('é'), 2);
        assert.throws(() => buildPayload({
            requestId: 'é'.repeat(65),
            content: [{ type: 'text', raw: 'x' }],
        }), /128/);
    });

    it('builds a continuation without content', () => {
        const payload = buildPayload({ operation: 'continue', requestId: 'req-3' });
        assert.deepEqual(payload.data, { operation: 'continue', request_id: 'req-3' });
    });

    it('builds a bounded read-only history query', () => {
        assert.deepEqual(buildPayload({ operation: 'history', requestId: 'h-1',
            start: 10, step: 3, limit: 5 }).data,
        { operation: 'history', request_id: 'h-1', start: 10, step: 3, limit: 5 });
        assert.throws(() => buildPayload({ operation: 'history', requestId: 'h-2',
            content: [{ type: 'text', raw: 'x' }] }), /must not carry/);
        assert.throws(() => buildPayload({ operation: 'history', requestId: 'h-3',
            limit: 11 }), /between 1 and 10/);
    });

    it('refuses content on a continuation', () => {
        assert.throws(() => buildPayload({
            operation: 'continue',
            requestId: 'req-4',
            content: [{ type: 'text', raw: 'x' }],
        }), ProtocolError);
    });

    it('rejects unknown operations', () => {
        assert.throws(() => buildPayload({ operation: 'cancel', requestId: 'r' }), ProtocolError);
    });
});

describe('normalizeOptions', () => {
    it('accepts model and confirmation categories', () => {
        assert.deepEqual(
            normalizeOptions({
                model: { model: 'deepseek-v4-pro', reasoning_effort: 'max' },
                confirmation: { mode: 'approve' },
            }),
            {
                model: { model: 'deepseek-v4-pro', reasoning_effort: 'max' },
                confirmation: { mode: 'approve' },
            });
    });

    it('treats options as absent when omitted or null', () => {
        assert.equal(normalizeOptions(undefined), undefined);
        assert.equal(normalizeOptions(null), undefined);
    });

    it('keeps an empty confirmation object a no-op', () => {
        assert.deepEqual(normalizeOptions({ confirmation: {} }), { confirmation: {} });
    });

    it('rejects unknown categories, non-objects, and reserved tool settings', () => {
        assert.throws(() => normalizeOptions({ temperature: 1 }), /unknown options category/);
        assert.throws(() => normalizeOptions({ model: 'deepseek-flash' }), /must be an object/);
        assert.throws(() => normalizeOptions({ tools: { enable: ['x'] } }), /reserved/);
        assert.throws(() => normalizeOptions({ confirmation: { mode: 'maybe' } }), /mode/);
    });

    it('accepts the reserved empty tools object', () => {
        assert.deepEqual(normalizeOptions({ tools: {} }), { tools: {} });
    });
});

describe('buildSignal', () => {
    it('builds the three field-free signals', () => {
        for (const operation of ['status', 'options', 'shutdown']) {
            assert.deepEqual(buildSignal({ operation }), {
                type: 'signal',
                data: { operation },
            });
        }
    });

    it('requires a run id for cancel', () => {
        assert.deepEqual(buildSignal({ operation: 'cancel', runId: 'run-1' }), {
            type: 'signal',
            data: { operation: 'cancel', run_id: 'run-1' },
        });
        assert.throws(() => buildSignal({ operation: 'cancel' }), /run_id/);
        assert.throws(() => buildSignal({ operation: 'cancel', runId: '' }), /run_id/);
    });

    it('rejects unknown operations', () => {
        assert.throws(() => buildSignal({ operation: 'restart' }), ProtocolError);
    });
});

describe('buildConfirmationResponse', () => {
    const request = {
        worker_id: 'w-1',
        session_id: 'demo',
        run_id: 'run-1',
        confirmation_id: 'c-1',
        call: { name: 'run_command' },
    };

    it('echoes all four identifiers', () => {
        const response = buildConfirmationResponse(request, 'approved', 'operator decision');
        assert.deepEqual(response, {
            type: 'confirmation_response',
            data: {
                worker_id: 'w-1',
                session_id: 'demo',
                run_id: 'run-1',
                confirmation_id: 'c-1',
                decision: 'approved',
                reason: 'operator decision',
            },
        });
    });

    it('never sends the call back', () => {
        const response = buildConfirmationResponse(request, 'denied');
        assert.equal(response.data.call, undefined);
        assert.equal(response.data.decision, 'denied');
    });

    it('rejects an unknown decision or a non-string reason', () => {
        assert.throws(() => buildConfirmationResponse(request, 'maybe'), ProtocolError);
        assert.throws(() => buildConfirmationResponse(request, 'denied', 7), ProtocolError);
    });
});

describe('newRequestId', () => {
    it('produces distinct identifiers', () => {
        const ids = new Set(Array.from({ length: 50 }, () => newRequestId()));
        assert.equal(ids.size, 50);
        assert.ok([...ids].every((id) => id.length <= 128));
    });
});
