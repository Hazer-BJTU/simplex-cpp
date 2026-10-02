/** Validate actual job outcomes against the reviewed selection contract. */
import { pathToFileURL } from 'node:url';
import { validateDecisions } from './selection.mjs';

export function checkResults(needs) {
    if (needs?.changes?.result !== 'success') {
        throw new Error('change classification did not succeed');
    }
    const outputs = needs.changes.outputs;
    const decisions = { category: outputs?.category };
    for (const key of ['cpp_validation', 'portable_build', 'staged_validation', 'worker_integration']) {
        if (!['true', 'false'].includes(outputs?.[key])) {
            throw new Error(`missing or invalid classifier output: ${key}`);
        }
        decisions[key] = outputs[key] === 'true';
    }
    validateDecisions(decisions);
    const selected = {
        'build-test': decisions.cpp_validation,
        'portable-release': decisions.portable_build,
        'staged-runtime': decisions.staged_validation,
        'hub-e2e': decisions.worker_integration,
        'hub-test': true,
        'hub-panel': true,
    };
    for (const [job, required] of Object.entries(selected)) {
        const expected = required ? 'success' : 'skipped';
        if (needs[job]?.result !== expected) {
            throw new Error(`${job}: expected ${expected}, received ${needs[job]?.result ?? 'missing'}`);
        }
    }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    checkResults(JSON.parse(process.env.NEEDS_JSON));
    console.log('All selected CI jobs succeeded; omitted jobs match the selection.');
}
