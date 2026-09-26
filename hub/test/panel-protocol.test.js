/**
 * @file the panel protocol envelope, and the one module that checks it.
 *
 * `shared/guards.ts` runs at both ends: the hub uses it to decide what to do
 * with an inbound frame, and a panel can use it to tell a malformed reply from
 * a message it simply does not know. These tests pin the decisions that matter
 * — which failures are reported, and which are deliberately not.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { checkEnvelope, isPanelMessageType } from '../shared/guards.ts';
import {
    CAPABILITIES,
    ERROR_CODES,
    HUB_MESSAGE_TYPES,
    PANEL_MESSAGE_TYPES,
    PANEL_PROTOCOL,
    PANEL_VERSION,
} from '../shared/protocol.ts';

describe('panel envelope guard', () => {
    it('accepts a well-formed message', () => {
        const check = checkEnvelope(JSON.stringify({
            v: PANEL_VERSION, type: 'subscribe', session: 'demo',
        }));
        assert.equal(check.kind, 'ok');
        assert.equal(check.message.type, 'subscribe');
    });

    it('accepts a message that omits the version', () => {
        // `v` is optional so a minimal client can talk to a hub without knowing
        // which version it speaks.
        const check = checkEnvelope(JSON.stringify({ type: 'ping' }));
        assert.equal(check.kind, 'ok');
    });

    it('reports text that is not JSON', () => {
        const check = checkEnvelope('{not json');
        assert.equal(check.kind, 'rejected');
        assert.equal(check.code, 'bad_json');
    });

    it('reports a value that is not an object', () => {
        for (const text of ['"a string"', '42', 'null']) {
            const check = checkEnvelope(text);
            assert.equal(check.kind, 'rejected', `${text} was accepted`);
            assert.equal(check.code, 'bad_message');
        }
    });

    it('reports a version it does not speak, naming its own', () => {
        const check = checkEnvelope(JSON.stringify({ v: 99, type: 'ping' }));
        assert.equal(check.kind, 'rejected');
        assert.equal(check.code, 'unsupported_version');
        assert.match(check.detail, new RegExp(`version ${PANEL_VERSION}`));
    });

    it('reports an unknown type instead of rejecting it', () => {
        // Forward compatibility depends on this being distinguishable from a
        // malformed frame: the caller ignores it rather than answering an error.
        const check = checkEnvelope(JSON.stringify({ v: PANEL_VERSION, type: 'invented' }));
        assert.equal(check.kind, 'unknown_type');
        assert.equal(check.type, 'invented');
    });

    it('reports a missing type as unknown rather than as a broken envelope', () => {
        const check = checkEnvelope(JSON.stringify({ v: PANEL_VERSION }));
        assert.equal(check.kind, 'unknown_type');
    });

    it('recognises exactly the types the protocol lists', () => {
        for (const type of PANEL_MESSAGE_TYPES) {
            assert.ok(isPanelMessageType(type), `${type} is listed but not recognised`);
        }
        assert.equal(isPanelMessageType('invented'), false);
        assert.equal(isPanelMessageType(42), false);
    });
});

describe('panel protocol vocabulary', () => {
    it('derives its version from the protocol it names', () => {
        assert.equal(PANEL_VERSION, PANEL_PROTOCOL.version);
        assert.equal(PANEL_PROTOCOL.name, 'simplex-hub-panel');
    });

    it('lists no duplicate message types', () => {
        for (const [label, list] of [
            ['panel', PANEL_MESSAGE_TYPES], ['hub', HUB_MESSAGE_TYPES], ['error', ERROR_CODES],
        ]) {
            assert.equal(new Set(list).size, list.length, `${label} types contain a duplicate`);
        }
    });

    it('lists no duplicate capabilities', () => {
        assert.equal(new Set(CAPABILITIES).size, CAPABILITIES.length);
    });

    it('names the capabilities this hub actually gained', () => {
        // These two are the reason the list stopped being decorative: the panel
        // reads it, and each one corresponds to behaviour added alongside it.
        assert.ok(CAPABILITIES.includes('transcript-epoch'));
        assert.ok(CAPABILITIES.includes('global-confirmations'));
    });
});
