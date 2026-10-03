/** @file optional logging admission, delayed sinks and failure containment. */
import assert from 'node:assert/strict';
import { Writable } from 'node:stream';
import { setTimeout as delay } from 'node:timers/promises';
import { it } from 'node:test';
import { BoundedWriter } from '../src/util/bounded-writer.ts';
import { until } from './helpers/worker.js';

/** A real Writable whose write completion is controlled by the test. */
function delayedSink(highWaterMark = 64, decodeStrings = true) {
    const records = [];
    const callbacks = [];
    const stream = new Writable({
        highWaterMark,
        decodeStrings,
        write(chunk, _encoding, callback) {
            records.push(chunk.toString('utf8'));
            callbacks.push(callback);
        },
    });
    return { stream, records, callbacks };
}

function writerFor(sink, options = {}) {
    return new BoundedWriter({
        open: () => sink.stream,
        omission: ({ records, bytes }) => JSON.stringify({
            type: 'hub_log_omission', dropped_records: records, dropped_bytes: bytes,
        }) + '\n',
        maxBytes: 256,
        ...options,
    });
}

it('admits the write that returns false, then drops until drain and emits one marker', async () => {
    const sink = delayedSink();
    let warnings = 0;
    const writer = writerFor(sink, { onDrop: () => { warnings += 1; } });
    const admitted = '中'.repeat(22) + '\n';
    assert.equal(writer.write(admitted), true);
    for (let index = 0; index < 10000; index += 1) {
        assert.equal(writer.write('🙂\n'), false);
        assert.equal(writer.pendingBytes, Buffer.byteLength(admitted));
    }
    assert.equal(writer.droppedRecords, 10000);
    assert.equal(writer.droppedBytes, 50000);
    assert.equal(warnings, 1);
    sink.callbacks.shift()();
    await until(() => sink.records.length === 2);
    assert.deepEqual(JSON.parse(sink.records[1]), {
        type: 'hub_log_omission', dropped_records: 10000, dropped_bytes: 50000,
    });
    sink.callbacks.shift()();
    await until(() => writer.pendingBytes === 0);
    assert.equal(writer.write('after\n'), true);
    writer.end();
    sink.callbacks.shift()();
    await until(() => sink.stream.writableFinished);
    assert.deepEqual(sink.records.map((line) => line.startsWith('{') ? 'marker' : line),
        [admitted, 'marker', 'after\n']);
});

it('checks exact UTF-8 queued bytes even before the Writable reaches its water mark', async () => {
    const sink = delayedSink(4096, false);
    const writer = writerFor(sink);
    assert.equal(writer.write('🙂'.repeat(32)), true);
    assert.equal(writer.write('a'.repeat(128)), true);
    assert.equal(writer.pendingBytes, 256);
    assert.equal(writer.write('x'), false);
    assert.equal(writer.pendingBytes, 256);
    sink.callbacks.shift()();
    sink.callbacks.shift()();
    assert.equal(writer.write('after\n'), true);
    assert.deepEqual(JSON.parse(sink.records[2]), {
        type: 'hub_log_omission', dropped_records: 1, dropped_bytes: 1,
    });
    writer.end();
    while (sink.callbacks.length) sink.callbacks.shift()();
    await until(() => sink.stream.writableFinished);
});

it('rejects an oversized record without opening a file and preserves JSONL boundaries', async () => {
    const sink = delayedSink(4096);
    let opens = 0;
    const writer = writerFor(sink, { open: () => { opens += 1; return sink.stream; } });
    assert.equal(writer.write('x'.repeat(257)), false);
    assert.equal(opens, 0);
    assert.equal(writer.write('{"event":"status"}\n'), true);
    assert.equal(opens, 1);
    writer.end();
    while (sink.callbacks.length) sink.callbacks.shift()();
    await until(() => sink.stream.writableFinished);
    assert.equal(sink.records.map((line) => JSON.parse(line)).length, 2);
    assert.equal(writer.droppedRecords, 1);
});

it('flushes a final omission marker on drain after end and rejects later writes', async () => {
    const sink = delayedSink();
    const writer = writerFor(sink);
    writer.write('a'.repeat(64));
    writer.write('lost\n');
    writer.end();
    writer.end();
    writer.write('late\n');
    sink.callbacks.shift()();
    await until(() => sink.records.length === 2);
    assert.equal(JSON.parse(sink.records[1]).dropped_records, 2);
    sink.callbacks.shift()();
    await until(() => sink.stream.writableFinished);
    assert.equal(writer.failed, false);
});

it('disables a failing sink once, without retrying open or throwing into its caller', async () => {
    const sink = delayedSink();
    const errors = [];
    let opens = 0;
    const writer = writerFor(sink, {
        open: () => { opens += 1; return sink.stream; },
        onError: (error) => errors.push(error.message),
    });
    writer.write('first\n');
    sink.callbacks.shift()(new Error('disk full'));
    await until(() => writer.failed);
    for (let index = 0; index < 100; index += 1) assert.equal(writer.write('next\n'), false);
    writer.end();
    assert.equal(opens, 1);
    assert.deepEqual(errors, ['disk full']);
    assert.equal(writer.droppedRecords, 100);
    assert.equal(sink.stream.destroyed, true);
});

it('contains synchronous open/write and diagnostic failures', () => {
    for (const mode of ['open', 'write']) {
        let opens = 0;
        const sink = delayedSink();
        if (mode === 'write') sink.stream.write = () => { throw new Error('write failed'); };
        const writer = writerFor(sink, {
            open: () => {
                opens += 1;
                if (mode === 'open') throw new Error('open failed');
                return sink.stream;
            },
            onError: () => { throw new Error('observer failed'); },
            onDrop: () => { throw new Error('observer failed'); },
        });
        assert.equal(writer.write('first\n'), false);
        assert.equal(writer.write('second\n'), false);
        assert.equal(writer.failed, true);
        assert.equal(opens, 1);
        writer.end();
    }
});

it('destroys a permanently stalled sink after the finite close deadline', async () => {
    const sink = delayedSink();
    const errors = [];
    const writer = writerFor(sink, {
        closeTimeoutMs: 25,
        onError: (error) => errors.push(error.message),
    });
    writer.write('a'.repeat(64));
    writer.write('lost\n');
    writer.end();
    await delay(60);
    assert.equal(writer.failed, true);
    assert.equal(writer.abandonedBytes, 64);
    assert.equal(sink.stream.destroyed, true);
    assert.match(errors[0], /close deadline/);
    sink.callbacks.shift()();
});
