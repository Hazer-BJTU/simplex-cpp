/** Same tool-fold fixture against either checkout; scan counts, not timings, are deterministic. */
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { toolTranscript, withToolScanCounts } from '../helpers/tool-rounds.js';

const root = process.argv[2] ? resolve(process.argv[2])
    : fileURLToPath(new URL('../../..', import.meta.url));
const { buildRounds } = await import(pathToFileURL(`${root}/hub/web/src/app/rounds.ts`).href);
const items = toolTranscript();
buildRounds(items, new Map()); // Warm the same pure fold before measurement.
const start = performance.now();
const { value: rounds, counts } = withToolScanCounts(() => buildRounds(items, new Map()));
const milliseconds = Number((performance.now() - start).toFixed(2));
const run = rounds[0];
assert.equal(run.calls.length, 1024);
assert.equal(run.calls.filter(call => call.status === 'ok').length, 1024);
assert.equal(run.assistant.length, 32);
console.log(JSON.stringify({ exchanges: 32, callsPerExchange: 32, events: items.length,
    retainedCalls: run.calls.length, matchedResults: 1024,
    ...counts, milliseconds,
    presentationSha256: createHash('sha256').update(JSON.stringify(rounds)).digest('hex'),
}, null, 2));
// The hash covers complete presentation output. Compare on the same fixture;
// elapsed time includes test-only counters and is neither a CI threshold nor
// a measurement of browser/DOM responsiveness.
