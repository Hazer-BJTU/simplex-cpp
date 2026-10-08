/** Offline data only. Generates before/after display copies without touching model state. */
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { normalizeDisplay } from '../../src/protocol/display.ts';
const output = process.argv[2];
if (!output) throw new Error('usage: node test/browser/large-output-cases.mjs <output-directory>');
mkdirSync(output, { recursive: true });
const before = [], after = [];
for (const bytes of [128 * 1024, 1024 * 1024, 3 * 1024 * 1024, 8 * 1024 * 1024]) {
    const raw = 'Reasoning completed. '.repeat(Math.ceil(bytes / 21)).slice(0, bytes);
    const data = { type: 'model_response', role: 'assistant',
        content: [{ type: 'text', modality: 'text', raw: 'Final benchmark answer' }],
        reasoning: { type: 'text', modality: 'text', raw } };
    const projected = normalizeDisplay('model_response', data);
    before.push({ bytes, data, envelopeBytes: Buffer.byteLength(JSON.stringify({ data, raw: { data } })) });
    after.push({ bytes, data: projected, envelopeBytes: Buffer.byteLength(JSON.stringify({ data: projected })) });
}
writeFileSync(join(output, 'before.json'), JSON.stringify(before));
writeFileSync(join(output, 'after.json'), JSON.stringify(after));
