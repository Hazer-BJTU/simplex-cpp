/** Test-only contract reader. Never used to negotiate or reject runtime peers. */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import Ajv2020 from 'ajv/dist/2020.js';

export const manifestPath = new URL('../../../protocol/schemas/worker-protocol.json', import.meta.url);
// Missing/malformed artifacts are errors, including in Node-only CI.
export const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));

/** Compile the emitted schema, including the documented trailing-marker rule. */
export function contractValidator(contract = manifest) {
    const ajv = new Ajv2020({ strict: false, allErrors: true });
    ajv.addKeyword({ keyword: 'x-simplex-trailing-marker', type: 'array',
        validate: (key, items) => items.slice(0, -1).every(item =>
            !item || typeof item !== 'object' || !Object.hasOwn(item, key)) });
    ajv.addSchema(contract.json_schema, 'worker');
    const cache = new Map();
    return (record, value) => {
        if (!cache.has(record)) cache.set(record, ajv.compile({ $ref: `worker#/$defs/${record}` }));
        const validate = cache.get(record);
        assert.ok(validate(value), `${record}: ${ajv.errorsText(validate.errors)}`);
    };
}

/** Dispatch event data through the manifest's event registry, not a copied list. */
export function validateWorkerEvent(event, check = contractValidator()) {
    check('EventEnvelope', event);
    const shape = manifest.events[event.event];
    if (shape.array) {
        assert.ok(Array.isArray(event.data));
        event.data.forEach((item, index) => {
            if (Object.hasOwn(item, 'display_omitted')) {
                assert.equal(index, event.data.length - 1, 'omission notice must be last');
                check('OmissionMarker', item);
            } else check(shape.data, item);
        });
    } else check(shape.data, event.data);
}
