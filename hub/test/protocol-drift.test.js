/** Hub adapters against compiled-descriptor artifacts; no source/prose parsing. */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { EVENT_TABLE, KNOWN_EVENTS, parseEventEnvelope } from '../src/protocol/events.ts';
import { CONTENT_MODALITIES, CONTENT_TYPES, INPUT_OPERATIONS, SIGNAL_OPERATIONS,
    buildConfirmationResponse, buildPayload, buildSignal } from '../src/protocol/messages.ts';
import { parseToolRequest, dispatchToolRequest } from '../src/protocol/tool-requests.ts';
import { succeededTool } from '../src/worker/tool-context.ts';
import { manifest, contractValidator, validateWorkerEvent } from './helpers/worker-contract.js';

const check = contractValidator();
const source = { worker_id: 'w', turn: 0, step: 0, commit_sequence: '1', fingerprint: 'a'.repeat(64) };
const correlation = { worker_id: 'w', session_id: 's', run_id: 'r', confirmation_id: 'c' };
const call = { id: 'c', name: 'future_tool', arguments: { arbitrary: [1, null] },
    type: 'read_only', security: 'trusted' };
const marker = { display_omitted: true, omitted_items: 36 };
const envelope = (event, data) => ({ type: 'event', event, session_id: 's', worker_id: 'w',
    request_id: 'q', run_id: 'r', sequence: 1, data });

describe('worker contract conformance', () => {
    it('accepts future capabilities without weakening closed operation vocabularies', () => {
        const data = { active: false, stopping: false, storage_failed: false,
            rejected_payloads: 0, capabilities: ['session-history', 'future-feature'] };
        assert.doesNotThrow(() => check('StatusData', data));
        assert.throws(() => check('StatusData', { ...data, capabilities: [42] }));
        assert.throws(() => check('PayloadRequest', { operation: 'future', request_id: 'q' }));
    });
    it('covers Hub vocabularies and every event rendering category', () => {
        for (const [name, values] of Object.entries({ EventName: KNOWN_EVENTS,
            InputOperation: INPUT_OPERATIONS, SignalOperation: SIGNAL_OPERATIONS,
            ContentType: CONTENT_TYPES, Modality: CONTENT_MODALITIES })) {
            assert.deepEqual([...values].sort(), [...manifest.enumerations[name].spellings].sort(), name);
        }
        for (const [name, entry] of Object.entries(manifest.events)) {
            assert.equal(Boolean(EVENT_TABLE[name].array), entry.array, name);
        }
    });
    it('validates actual Hub request builders for all operations and signals', () => {
        for (const operation of INPUT_OPERATIONS) {
            const extra = operation === 'message'
                ? { content: [{ type: 'text', raw: 'hello', modality: 'text' }], options: { model: {} } }
                : operation === 'history' ? { start: 0, step: 0, limit: 10 }
                : operation === 'answer' ? { source, part: 0, offset: 0 } : {};
            check('PayloadEnvelope', buildPayload({ operation, requestId: 'q', ...extra }));
        }
        for (const operation of SIGNAL_OPERATIONS) {
            check('SignalEnvelope', buildSignal({ operation, runId: 'r' }));
        }
        for (const type of CONTENT_TYPES) for (const modality of CONTENT_MODALITIES) {
            check('PayloadEnvelope', buildPayload({ requestId: 'q', content: [{ type, modality, raw: 'x' }] }));
        }
        const answer = buildPayload({ operation: 'answer', requestId: 'q', source, part: 0, offset: 0 });
        assert.throws(() => check('PayloadEnvelope', { ...answer, data: { ...answer.data, unknown: true } }));
        assert.throws(() => check('PayloadEnvelope', { ...answer, data: { ...answer.data, source: null } }));
        const message = buildPayload({ requestId: 'q', content: [{ type: 'text', modality: 'text', raw: 'x' }] });
        check('PayloadEnvelope', { ...message, data: { ...message.data, start: 'ignored', unknown: true } });
        // Runtime policy is deliberately outside protocol compatibility.
        check('PayloadEnvelope', { type: 'payload', data: { operation: 'continue', request_id: '',
            options: { confirmation: { mode: 'future-policy' }, future: 42 } } });
    });
    it('validates confirmation and opaque remote-call envelopes', () => {
        check('ConfirmationRequestEnvelope', { type: 'confirmation_request', data: { ...correlation, call } });
        for (const decision of manifest.enumerations.ConfirmationDecision.spellings) {
            check('ConfirmationResponseEnvelope', buildConfirmationResponse(correlation, decision));
        }
        const ids = { worker_id: 'w', session_id: 's', run_id: 'r', request_id: 'q' };
        const request = { type: 'tool_request', data: { ...ids, arguments: { future: [null] } } };
        check('ToolRequestEnvelope', request);
        const parsed = parseToolRequest(request, 's');
        assert.deepEqual(parsed, request.data);
        check('ToolResponseEnvelope', dispatchToolRequest('/unknown', parsed));
        check('ToolResponseEnvelope', succeededTool({ request: parsed, route: '/unknown' }, { future: true }));
        for (const [status, fields] of [['succeeded', { result: { future: true } }],
            ['rejected', { error: { code: 'future_error', message: 'unavailable' } }]]) {
            const value = { type: 'tool_response', data: { ...ids, route: '/unknown', status, ...fields } };
            check('ToolResponseEnvelope', value);
            assert.throws(() => check('ToolResponseEnvelope', { ...value, data: { ...value.data, result: {}, error: {} } }));
        }
    });
    it('validates projected events before exercising the tolerant Hub normalizer', () => {
        const fixtures = [envelope('run_started', {}), envelope('tool_calls', [call, marker]),
            envelope('history', { request_id: 'q', revision: 1, start: 0, step: 0, next: 0,
                next_step: 0, total: 0, turns: [] }),
            envelope('model_response', { index: 0, type: 'model_response', role: 'assistant', content: [],
                tool_calls: 1, omitted_parts: 0, invokes: [call, marker] }),
            envelope('run_finished', { status: 'completed', error: '', exchanges: 1, durable: true,
                omitted_display_events: 2 })];
        for (const fixture of fixtures) {
            validateWorkerEvent(fixture, check);
            assert.equal(parseEventEnvelope(JSON.stringify(fixture)).ok, true);
        }
        assert.equal(parseEventEnvelope(JSON.stringify(envelope('future_event', { future: true }))).ok, true);
        assert.throws(() => validateWorkerEvent(envelope('tool_calls', [marker, call]), check));
        assert.throws(() => check('PayloadEnvelope', { type: 'payload', data: { operation: 'unknown' } }));
        assert.throws(() => check('PayloadEnvelope', { type: 'payload', data: { operation: 'continue', request_id: 'q', content: null } }));
    });
    it('detects deliberate required-field, enum and structural-constraint mutations', () => {
        const message = buildSignal({ operation: 'status' });
        for (const mutate of [
            schema => schema.$defs.SignalRequest.required.push('new_required'),
            schema => { schema.$defs.SignalOperation.enum = ['new_operation']; },
            schema => { schema.$defs.SignalRequest.properties.operation = { type: 'integer' }; },
        ]) {
            const changed = structuredClone(manifest);
            mutate(changed.json_schema);
            assert.throws(() => contractValidator(changed)('SignalEnvelope', message), /SignalEnvelope/);
        }
    });
});
