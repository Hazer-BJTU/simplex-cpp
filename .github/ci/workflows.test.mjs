import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { test } from 'node:test';

const directory = new URL('../workflows/', import.meta.url);
const workflows = readdirSync(directory)
    .filter((name) => /\.ya?ml$/.test(name))
    .sort()
    .map((name) => ({ name, text: readFileSync(new URL(name, directory), 'utf8') }));

for (const action of ['actions/checkout', 'actions/setup-node']) {
    test(`${action} uses one reference across all workflows`, () => {
        const references = new Set();
        const locations = [];
        for (const { name, text } of workflows) {
            // Read the scalar uses declarations in our workflow files, including
            // quoted references. This checks consistency, not full YAML syntax.
            const declarations = text.matchAll(
                /^\s*(?:-\s*)?uses:\s*["']?(actions\/(?:checkout|setup-node))@([^\s"'#]+)/gm);
            for (const [, usedAction, reference] of declarations) {
                if (usedAction !== action) continue;
                references.add(reference);
                locations.push(`${name}: ${usedAction}@${reference}`);
            }
        }
        assert.ok(locations.length > 0, `No ${action} declarations found`);
        assert.equal(references.size, 1,
            `${action} references differ:\n${locations.join('\n')}`);
    });
}
