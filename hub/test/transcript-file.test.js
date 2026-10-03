/** @file optional transcript-file losses must not change live replay history. */
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { SessionTranscript } from '../src/state/transcript.ts';
import { LOG_WRITE_MAX_BYTES } from '../src/util/bounded-writer.ts';
import { until } from './helpers/worker.js';

it('omits oversized file records atomically and keeps them in live replay', async (t) => {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-transcript-file-'));
    const filePath = join(directory, 'events.jsonl');
    const transcript = new SessionTranscript({ sessionId: 'logs', limit: 10, filePath });
    t.after(() => { transcript.close(); rmSync(directory, { recursive: true, force: true }); });
    const huge = { event: 'model_response', text: 'x'.repeat(LOG_WRITE_MAX_BYTES) };
    transcript.append(huge);
    assert.equal(existsSync(filePath), false);
    assert.equal(transcript.writer.droppedRecords, 1);
    transcript.append({ event: 'status', data: { active: false } });
    transcript.close();
    await until(() => existsSync(filePath) && readFileSync(filePath, 'utf8').includes('"event":"status"'));
    const rows = readFileSync(filePath, 'utf8').trim().split('\n').map((line) => JSON.parse(line));
    assert.equal(rows.length, 2);
    assert.equal(rows[0].type, 'hub_log_omission');
    assert.equal(rows[0].dropped_records, 1);
    assert.equal(rows[0].dropped_bytes, Buffer.byteLength(JSON.stringify(huge)) + 1);
    assert.equal(rows[1].hub_sequence, 2);
    assert.equal(transcript.sequence, 2);
    assert.equal(transcript.written, 1);
    assert.equal(transcript.size, 2);
    assert.equal(transcript.toArray()[0], huge);
});

it('does not reopen a transcript file after close', async (t) => {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-transcript-close-'));
    const filePath = join(directory, 'events.jsonl');
    const transcript = new SessionTranscript({ sessionId: 'logs', limit: 10, filePath });
    t.after(() => rmSync(directory, { recursive: true, force: true }));
    transcript.close();
    transcript.append({ event: 'late' });
    assert.equal(transcript.written, 0);
    assert.equal(transcript.writer.droppedRecords, 1);
    assert.equal(existsSync(filePath), false);
    assert.equal(transcript.sequence, 1);
});
