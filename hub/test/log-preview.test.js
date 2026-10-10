/** Bounded complete log records, real content accounting and independent disk copies. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { formatLogLine, logPreview, MIN_LOG_RING_BYTES } from '../src/util/log-preview.ts';
import { LineSplitter, RingBuffer } from '../src/util/ring.ts';
import { ProcessRecord } from '../src/launch/supervisor.ts';

function annotation(text) {
    const match = text.match(/^(.*) \[hub: truncated (\d+) UTF-8 bytes\]$/s);
    assert.ok(match, 'a clipped record retains its full annotation');
    return { prefix: match[1], omitted: Number(match[2]) };
}

it('preserves fitting records exactly, including a full-budget line and literal marker text', () => {
    for (const prefix of ['', 'a'.repeat(64), '中'.repeat(21), '[hub: truncated 999 UTF-8 bytes]']) {
        assert.deepEqual(logPreview(prefix, 0, 64), { text: prefix, truncatedBytes: 0 });
    }
    const full = formatLogLine('original prefix', 4096);
    assert.deepEqual(logPreview('original prefix', 4096, 64), { text: full, truncatedBytes: 0 });
});

it('budgets the entire UTF-8 record and counts only discarded content', () => {
    for (const budget of [64, 65, 128, 1024, 64 * 1024]) {
        for (const prefix of ['a'.repeat(128 * 1024), 'prefix: ' + '中🙂é'.repeat(16000),
            '\u0000\u0001"\\'.repeat(32768)]) {
            for (const previouslyOmitted of [0, 9, 999, Number.MAX_SAFE_INTEGER - 256 * 1024]) {
                const preview = logPreview(prefix, previouslyOmitted, budget);
                assert.ok(Buffer.byteLength(preview.text) <= budget);
                const retained = annotation(preview.text);
                assert.ok(retained.prefix.length > 0, 'even the minimum budget keeps a useful prefix');
                assert.ok(prefix.startsWith(retained.prefix));
                assert.equal(preview.truncatedBytes, Buffer.byteLength(prefix) - Buffer.byteLength(retained.prefix));
                assert.equal(retained.omitted, previouslyOmitted + preview.truncatedBytes);
                assert.equal(Buffer.from(retained.prefix).toString('utf8'), retained.prefix);
                assert.doesNotMatch(retained.prefix, /\uFFFD/);
            }
        }
    }
});

it('clips for annotation overhead even when the original content alone fits the ring', () => {
    const prefix = 'x'.repeat(64 * 1024);
    const preview = logPreview(prefix, 192 * 1024, 64 * 1024);
    assert.ok(preview.truncatedBytes > 0);
    assert.ok(Buffer.byteLength(preview.text) <= 64 * 1024);
    assert.equal(annotation(preview.text).omitted, 192 * 1024 + preview.truncatedBytes);
});

it('retains oversized newest previews under both ring limits without relaxing the generic ceiling', () => {
    const ring = new RingBuffer({ limit: 2, byteLimit: 64 });
    for (let index = 0; index < 100; index += 1) {
        ring.push(logPreview(`entry ${index}: ` + 'x'.repeat(4096), 0, ring.byteLimit).text);
        assert.equal(ring.size, 1);
        assert.ok(ring.bytes <= 64);
        assert.ok(ring.toArray()[0].startsWith(`entry ${index}: `));
    }
    assert.equal(ring.dropped, 99, 'truncation is distinct from evicting complete older records');
    ring.push('short');
    ring.push('next');
    assert.deepEqual(ring.toArray(), ['short', 'next']);
    assert.equal(ring.bytes, 9);
    assert.equal(ring.dropped, 100);
});

function record() {
    const disk = [];
    const process = new ProcessRecord({ sessionId: 'preview',
        invocation: { command: 'probe', args: [], cwd: '', pidFile: null },
        logPath: null, logStream: { write: text => disk.push(text), end() {} },
        logs: new RingBuffer({ limit: 10, byteLimit: 64 }) });
    const splitter = new LineSplitter((line, details) => process.captureLine(line, details), 96);
    process.outputSplitters.push(splitter);
    return { process, disk, splitter };
}

it('folds splitter and preview omissions once while keeping the longer disk prefix', () => {
    const { process, disk, splitter } = record();
    const source = 'heading: ' + '中🙂'.repeat(200);
    splitter.push(Buffer.from(source + '\r'));
    splitter.push(Buffer.from('\n'));
    const memory = annotation(process.logs.toArray()[0]);
    const file = annotation(disk[0].slice(0, -1));
    assert.ok(Buffer.byteLength(file.prefix) > Buffer.byteLength(memory.prefix));
    assert.equal(file.omitted, splitter.truncatedBytes);
    const expected = Buffer.byteLength(source) - Buffer.byteLength(memory.prefix);
    assert.equal(memory.omitted, expected);
    assert.equal(process.describe().log_truncated_bytes, expected);
    assert.equal(process.describe().log_dropped, 0);
    assert.ok(process.logs.bytes <= 64);
    assert.equal(process.logs.toArray()[0].match(/\[hub: truncated/g).length, 1);
});

it('excludes split CRLF and preserves exact-budget content without a false marker', () => {
    const { process, disk, splitter } = record();
    splitter.push('a'.repeat(64) + '\r');
    splitter.push('\n');
    assert.deepEqual(process.logs.toArray(), ['a'.repeat(64)]);
    assert.deepEqual(disk, ['a'.repeat(64) + '\n']);
    assert.equal(process.describe().log_truncated_bytes, 0);
});

it('accounts for independent stdout/stderr EOF previews without leaking annotations into totals', () => {
    const { process, disk, splitter: stdout } = record();
    const stderr = new LineSplitter((line, details) => process.captureLine(line, details), 96);
    process.outputSplitters.push(stderr);
    const out = 'out: ' + '中'.repeat(200);
    const err = 'err: ' + 'é'.repeat(200);
    stdout.push(Buffer.from(out));
    stderr.push(Buffer.from(err));
    stdout.flush();
    const first = annotation(process.logs.toArray()[0]);
    stderr.flush();
    const second = annotation(process.logs.toArray()[0]);
    assert.ok(second.prefix.startsWith('err: '));
    assert.equal(process.logs.dropped, 1);
    assert.equal(process.describe().log_truncated_bytes, first.omitted + second.omitted);
    assert.equal(disk.length, 2);
    stdout.flush();
    stderr.flush();
    assert.equal(disk.length, 2, 'repeated EOF flush cannot double-count previews');
});

it('rejects budgets too small for a full annotation instead of silently admitting an unusable record', () => {
    assert.equal(MIN_LOG_RING_BYTES, 64);
    for (const budget of [0, 4, 63, 64.5, Infinity, NaN, Number.MAX_SAFE_INTEGER + 1]) {
        assert.throws(() => logPreview('text', 0, budget), RangeError);
    }
});
