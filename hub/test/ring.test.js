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

    it('enforces the byte ceiling even for the final oversized entry', () => {
        const ring = new RingBuffer({ limit: 10, byteLimit: 2 });
        ring.push('oversized');
        assert.deepEqual(ring.toArray(), []);
        assert.equal(ring.bytes, 0);
        ring.push('another');
        assert.deepEqual(ring.toArray(), []);
        assert.equal(ring.dropped, 2);
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

    it('keeps interleaved pipe decoders and EOF fragments independent', () => {
        const lines = [];
        const stdout = new LineSplitter((line) => lines.push(['stdout', line]));
        const stderr = new LineSplitter((line) => lines.push(['stderr', line]));
        stdout.push(Buffer.from([0xe4]));
        stderr.push(Buffer.from('error\nerr-tail'));
        stdout.push(Buffer.from([0xb8, 0xad, 0x0a]));
        stdout.push(Buffer.from('out-tail'));
        stdout.push(Buffer.from([0xe4]));
        stdout.flush();
        stderr.flush();
        stdout.flush();
        stderr.flush();
        assert.deepEqual(lines, [
            ['stderr', 'error'], ['stdout', '中'],
            ['stdout', 'out-tail�'], ['stderr', 'err-tail'],
        ]);
    });

    it('flushes an incomplete UTF-8 character even without decoded text', () => {
        const lines = [];
        const splitter = new LineSplitter((line) => lines.push(line));
        splitter.push(Buffer.from([0xe4]));
        splitter.flush();
        splitter.flush();
        assert.deepEqual(lines, ['�']);
    });

    it('bounds a long newline-free stream and reports the omitted bytes', () => {
        const lines = [];
        const splitter = new LineSplitter((line) => lines.push(line), 64);
        for (let index = 0; index < 256; index += 1) {
            splitter.push('a'.repeat(65536));
            assert.equal(splitter.pendingBytes, 64);
            assert.equal(splitter.pending.length, 64);
        }
        assert.equal(lines.length, 0);
        const omitted = 16 * 1024 * 1024 - 64;
        assert.equal(splitter.truncatedBytes, omitted);
        splitter.push('\nnext\n');
        assert.deepEqual(lines, [
            'a'.repeat(64) + ` [hub: truncated ${omitted} UTF-8 bytes]`, 'next',
        ]);
        assert.equal(splitter.pendingBytes, 0);
    });

    it('never cuts through a UTF-8 code point at the prefix boundary', () => {
        const lines = [];
        const splitter = new LineSplitter((line) => lines.push(line), 5);
        splitter.push(Buffer.from('中中文\n'));
        splitter.push('abcde\r\n');
        splitter.push('next');
        splitter.flush();
        assert.deepEqual(lines, [
            '中 [hub: truncated 6 UTF-8 bytes]',
            'abcde', 'next',
        ]);
        assert.equal(splitter.truncatedBytes, 6);
    });

    it('excludes CRLF from an exact content limit, including split terminators', () => {
        for (const chunks of [
            ['abcde\r\n'],
            ['abcde\r', '\n'],
            ['abcde', '\r', '\n'],
        ]) {
            const lines = [];
            const splitter = new LineSplitter((line) => lines.push(line), 5);
            for (const chunk of chunks) {
                splitter.push(Buffer.from(chunk));
                assert.equal(splitter.truncatedBytes, 0);
                assert.ok(splitter.pendingBytes <= 5);
            }
            splitter.flush();
            assert.deepEqual(lines, ['abcde']);
        }
    });

    it('excludes split CRLF after real truncation without hiding omitted content', () => {
        const lines = [];
        const splitter = new LineSplitter((line) => lines.push(line), 6);
        splitter.push(Buffer.from('中中文\r'));
        assert.equal(splitter.pendingBytes, 6);
        assert.equal(splitter.truncatedBytes, 3);
        splitter.push(Buffer.from('\nnext\r'));
        assert.equal(splitter.truncatedBytes, 3);
        splitter.push(Buffer.from('\n'));
        splitter.flush();
        assert.deepEqual(lines, ['中中 [hub: truncated 3 UTF-8 bytes]', 'next']);
        assert.equal(splitter.truncatedBytes, 3);
    });

    it('counts a deferred CR as content when it is not followed by LF', () => {
        const lines = [];
        const splitter = new LineSplitter((line) => lines.push(line), 5);
        splitter.push('abcd\r');
        assert.equal(splitter.pendingBytes, 4);
        splitter.push('x\r');
        assert.equal(splitter.truncatedBytes, 1);
        splitter.push('\n\r');
        splitter.push('\r\n');
        splitter.flush();
        assert.deepEqual(lines, ['abcd\r [hub: truncated 1 UTF-8 bytes]', '\r']);
        assert.equal(splitter.truncatedBytes, 1);
    });

    it('rejects invalid line limits', () => {
        for (const value of [0, -1, 1.5, NaN, Infinity]) {
            assert.throws(() => new LineSplitter(() => {}, value), RangeError);
        }
    });
});
