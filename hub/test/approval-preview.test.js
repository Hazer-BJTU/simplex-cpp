import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { APPROVAL_ARGUMENT_BYTES, approvalArgumentPreview } from '../src/protocol/approval-preview.ts';

const bytes = value => Buffer.byteLength(JSON.stringify(value));

describe('approval argument display budget', () => {
    it('keeps ten long fields and nested values exact when the JSON fits', () => {
        const source = Object.fromEntries(Array.from({ length: 10 }, (_, index) =>
            [`field_${index}`, 'x'.repeat(4096)]));
        source.nested = { arguments: Array.from({ length: 100 }, (_, index) => `arg-${index}`), enabled: true };
        const result = approvalArgumentPreview(source);
        assert.equal(result.truncated, false);
        assert.equal(result.originalBytes, bytes(source));
        assert.deepEqual(result.value, source);
    });

    it('preserves exact encoded-budget boundaries and missing/null arguments', () => {
        for (const extra of [0, 1]) {
            const source = { command: 'x'.repeat(APPROVAL_ARGUMENT_BYTES - bytes({ command: '' }) + extra) };
            const result = approvalArgumentPreview(source);
            assert.equal(result.truncated, extra > 0);
            assert.ok(bytes(result.value) <= APPROVAL_ARGUMENT_BYTES);
            if (extra === 0) assert.deepEqual(result.value, source);
        }
        assert.deepEqual(approvalArgumentPreview(undefined).value, {});
        assert.equal(approvalArgumentPreview(null).value, null);
    });

    it('never aliases executable input, including nested values that fit unchanged', () => {
        for (const command of ['echo safe', 'x'.repeat(100000)]) {
            const source = { command, nested: { arguments: ['original'] } };
            const result = approvalArgumentPreview(source);
            result.value.nested.arguments[0] = 'changed display';
            assert.equal(source.nested.arguments[0], 'original');
        }
    });

    it('shortens the largest fields first and marks every changed value without mutating input', () => {
        const source = Object.fromEntries(Array.from({ length: 10 }, (_, index) =>
            [`field_${index}`, 'x'.repeat(20000)]));
        source.path = '/workspace/file.txt';
        source.enabled = true;
        const original = JSON.stringify(source);
        const result = approvalArgumentPreview(source);
        assert.equal(result.truncated, true);
        assert.ok(bytes(result.value) <= APPROVAL_ARGUMENT_BYTES);
        assert.deepEqual(Object.keys(result.value), Object.keys(source));
        assert.equal(result.value.path, source.path);
        assert.equal(result.value.enabled, true);
        for (let index = 0; index < 10; index += 1) {
            const field = result.value[`field_${index}`];
            assert.equal(field.display_truncated, true);
            assert.equal(field.bytes, 20000);
            assert.ok(field.preview.length > 5000);
            assert.ok(source[`field_${index}`].startsWith(field.preview));
        }
        assert.equal(JSON.stringify(source), original);
    });

    it('accounts for escaped keys/strings and keeps valid UTF-8 prefixes in nested arguments', () => {
        const source = {
            'quoted"key\n': { before: '😀漢字\u0000"\\\n'.repeat(20000), after: 'short replacement' },
            arguments: ['keep me', 'é😀'.repeat(20000)],
            count: 42,
        };
        const result = approvalArgumentPreview(source);
        assert.ok(bytes(result.value) <= APPROVAL_ARGUMENT_BYTES);
        assert.equal(result.value.count, 42);
        assert.equal(result.value['quoted"key\n'].after, source['quoted"key\n'].after);
        for (const [original, preview] of [
            [source['quoted"key\n'].before, result.value['quoted"key\n'].before],
            [source.arguments[1], result.value.arguments[1]],
        ]) {
            assert.equal(preview.display_truncated, true);
            assert.equal(preview.bytes, Buffer.byteLength(original));
            assert.ok(original.startsWith(preview.preview));
            assert.equal(Buffer.from(preview.preview).toString('utf8'), preview.preview);
            assert.ok(!preview.preview.includes('\ufffd'));
        }
    });

    it('marks array tails explicitly while retaining small sibling fields', () => {
        const source = { paths: Array.from({ length: 10000 }, (_, index) => `/path/${index}/${'x'.repeat(400)}`), cwd: '/workspace' };
        const result = approvalArgumentPreview(source);
        assert.ok(bytes(result.value) <= APPROVAL_ARGUMENT_BYTES);
        assert.equal(result.value.cwd, source.cwd);
        const marker = result.value.paths.at(-1);
        assert.equal(marker.display_omitted, true);
        assert.equal(marker.omitted_items, source.paths.length - (result.value.paths.length - 1));
        assert.ok(result.value.paths.length > 1);
        assert.ok(result.value.paths[0].preview.startsWith('/path/0/'));
    });

    it('uses whole-object omission only when the labelled field structure cannot fit', () => {
        const source = { ['key'.repeat(APPROVAL_ARGUMENT_BYTES)]: 'value' };
        assert.deepEqual(approvalArgumentPreview(source).value, { display_omitted: true });
    });

    it('keeps unusual JSON property names without changing the output prototype', () => {
        const source = JSON.parse(`{"__proto__":"safe","constructor":"also safe","command":"${'x'.repeat(100000)}"}`);
        const result = approvalArgumentPreview(source);
        assert.ok(bytes(result.value) <= APPROVAL_ARGUMENT_BYTES);
        assert.equal(Object.getPrototypeOf(result.value), Object.prototype);
        assert.equal(Object.hasOwn(result.value, '__proto__'), true);
        assert.equal(result.value.__proto__, 'safe');
        assert.equal(result.value.constructor, 'also safe');
    });

    it('preserves JSON-escaped lone surrogates and bounds oversized deep traversal', () => {
        const source = { command: '\ud800😀\udc00'.repeat(30000) };
        const preview = approvalArgumentPreview(source).value.command;
        assert.ok(source.command.startsWith(preview.preview));
        assert.ok(!preview.preview.includes('\ufffd'));
        assert.ok(bytes({ command: preview }) <= APPROVAL_ARGUMENT_BYTES);

        let nested = 'x'.repeat(100000);
        for (let depth = 0; depth < 40; depth++) nested = { child: nested };
        const result = approvalArgumentPreview({ nested, path: '/workspace' });
        assert.equal(result.value.path, '/workspace');
        let item = result.value.nested;
        while (item.child) item = item.child;
        assert.deepEqual(item, { display_omitted: true });
        assert.ok(bytes(result.value) <= APPROVAL_ARGUMENT_BYTES);
    });
});
