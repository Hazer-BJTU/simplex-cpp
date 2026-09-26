/**
 * @file bounded buffers: the memory ceilings behind the log view and the
 * transcript, and the chunk splitting that keeps log lines intact.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { LineSplitter, RingBuffer } from '../src/util/ring.ts';

describe('RingBuffer', () => {
    it('keeps the most recent entries up to the count limit', () => {
        const ring = new RingBuffer({ limit: 3 });
        for (const value of ['a', 'b', 'c', 'd']) ring.push(value);
        assert.deepEqual(ring.toArray(), ['b', 'c', 'd']);
        assert.equal(ring.dropped, 1);
        assert.equal(ring.size, 3);
    });

    it('counts string length against the byte limit', () => {
        const ring = new RingBuffer({ limit: 100, byteLimit: 5 });
        ring.push('abc');
        ring.push('def');
        assert.deepEqual(ring.toArray(), ['def']);
        assert.equal(ring.bytes, 3);
    });

    it('always keeps at least one entry, however large', () => {
        const ring = new RingBuffer({ limit: 10, byteLimit: 2 });
        ring.push('oversized');
        assert.deepEqual(ring.toArray(), ['oversized']);
        ring.push('another');
        assert.deepEqual(ring.toArray(), ['another']);
    });

    it('uses a custom size function for structured entries', () => {
        const ring = new RingBuffer({
            limit: 10,
            byteLimit: 100,
            sizeOf: (item) => item.bytes,
        });
        ring.push({ bytes: 60, name: 'first' });
        ring.push({ bytes: 60, name: 'second' });
        assert.deepEqual(ring.toArray().map((item) => item.name), ['second']);
        assert.equal(ring.bytes, 60);
    });

    it('clears without touching the drop counter', () => {
        const ring = new RingBuffer({ limit: 2 });
        ring.push('a');
        ring.clear();
        assert.equal(ring.size, 0);
        assert.equal(ring.bytes, 0);
    });
});

describe('LineSplitter', () => {
    it('emits complete lines and holds a partial one back', () => {
        const lines = [];
        const splitter = new LineSplitter((line) => lines.push(line));
        splitter.push('first\nsec');
        assert.deepEqual(lines, ['first']);
        splitter.push('ond\nthird\n');
        assert.deepEqual(lines, ['first', 'second', 'third']);
    });

    it('decodes a multi-byte character split across chunks', () => {
        const lines = [];
        const splitter = new LineSplitter((line) => lines.push(line));
        const bytes = Buffer.from('héllo\n', 'utf8');
        splitter.push(bytes.subarray(0, 2));
        splitter.push(bytes.subarray(2));
        assert.deepEqual(lines, ['héllo']);
    });

    it('strips a carriage return and flushes a trailing fragment', () => {
        const lines = [];
        const splitter = new LineSplitter((line) => lines.push(line));
        splitter.push('windows\r\npartial');
        assert.deepEqual(lines, ['windows']);
        splitter.flush();
        assert.deepEqual(lines, ['windows', 'partial']);
        splitter.flush();
        assert.equal(lines.length, 2);
    });
});
