/** @file real worker control must not wait for either optional disk sink. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { basename, join } from 'node:path';
import { Writable } from 'node:stream';
import { it } from 'node:test';
import { startTestHub } from './helpers/hub.js';
import { until } from './helpers/worker.js';

it('keeps status and shutdown routing responsive while both disk sinks remain stalled', async (t) => {
    const originalOpen = fs.createWriteStream;
    const sinks = [];
    // Node's built-in named ESM exports follow the mocked fs object after sync.
    // This test file runs in its own process; the child fixture remains real.
    t.mock.method(fs, 'createWriteStream', (path, options) => {
        if (!['worker.log', 'events.jsonl'].includes(basename(String(path)))) {
            return originalOpen(path, options);
        }
        const stream = new Writable({
            highWaterMark: 1,
            write(_chunk, _encoding, _callback) { /* intentionally never drains */ },
        });
        sinks.push(stream);
        return stream;
    });
    syncBuiltinESMExports();
    const ctx = await startTestHub({ worker: {
        bin: join(import.meta.dirname, 'fixtures', 'fake-worker.js'),
        stopTimeoutMs: 100, sigtermGraceMs: 200, sigkillGraceMs: 1000,
    } });
    t.after(async () => {
        try { await ctx.hub.stop(); }
        finally {
            for (const sink of sinks) sink.destroy();
            fs.rmSync(ctx.config.dataDir, { recursive: true, force: true });
            t.mock.restoreAll();
            syncBuiltinESMExports();
        }
    });
    const session = ctx.hub.registry.create('stalled-log-worker');
    const started = await ctx.hub.supervisor.start(session);
    assert.equal(started.ok, true, started.error);
    await until(() => session.latest.status !== null);
    const transcript = ctx.hub.transcripts.get(session.id);
    await until(() => transcript.writer.droppedRecords > 0
        && session.process.logStream.droppedRecords > 0);
    const previous = session.latest.status;
    session.connection.send({ type: 'signal', data: { operation: 'status' } });
    await until(() => session.latest.status !== previous);
    assert.equal(sinks.length, 2);
    const retainedStatus = transcript.toArray().filter((item) => item.event === 'status');
    assert.ok(retainedStatus.length >= 2);
    assert.ok(transcript.writer.pendingBytes <= transcript.writer.maxBytes);
    assert.ok(session.process.logStream.pendingBytes <= session.process.logStream.maxBytes);
    const stopped = await ctx.hub.supervisor.stop(session);
    assert.deepEqual(stopped, { ok: true, how: 'shutdown-signal', forced: false });
});
