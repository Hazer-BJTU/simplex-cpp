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

function ciJob(name) {
    const ci = workflows.find((workflow) => workflow.name === 'ci.yml').text;
    const start = ci.indexOf(`\n  ${name}:\n`);
    assert.ok(start !== -1, `missing CI job: ${name}`);
    const rest = ci.slice(start + 1);
    const next = rest.slice(1).search(/^  [a-z][a-z-]*:\s*$/m);
    return next === -1 ? rest : rest.slice(0, next + 1);
}

test('ordinary CI retains unconditional version, selection, Hub and gate checks', () => {
    const changes = ciJob('changes');
    assert.doesNotMatch(changes, /^\s+if:/m);
    assert.match(changes, /run: node --test \.github\/ci\/\*\.test\.mjs/);
    assert.match(changes, /run: node versioning\/sync_version\.mjs --check/);
    assert.match(changes, /fetch-depth: 0/);
    for (const output of ['category', 'cpp_validation', 'portable_build',
        'staged_validation', 'worker_integration', 'version_from', 'version_to']) {
        assert.ok(changes.includes(`${output}: \${{ steps.selection.outputs.${output} }}`));
    }
    for (const name of ['hub-test', 'hub-panel']) {
        assert.doesNotMatch(ciJob(name), /^    if:/m);
    }
    const gate = ciJob('ci-gate');
    assert.match(gate, /if: always\(\)/);
    assert.match(gate, /needs: \[changes, build-test, portable-release, staged-runtime, hub-e2e, hub-test, hub-panel\]/);
    assert.match(gate, /run: node \.github\/ci\/gate\.mjs/);
});

test('worker consumers follow centralized selection and require a successful current producer', () => {
    for (const [job, output] of [
        ['build-test', 'cpp_validation'], ['portable-release', 'portable_build'],
        ['staged-runtime', 'staged_validation'], ['hub-e2e', 'worker_integration'],
    ]) {
        const text = ciJob(job);
        assert.ok(text.includes(`if: needs.changes.outputs.${output} == 'true'`));
        if (job === 'staged-runtime' || job === 'hub-e2e') {
            assert.match(text, /needs: \[changes, portable-release\]/);
            assert.match(text, /&& needs\.portable-release\.result == 'success'/);
        }
    }
});

test('the version bypass is confined to ordinary CI, not release or documentation workflows', () => {
    for (const name of ['release-worker.yml', 'docs.yml']) {
        const text = workflows.find((workflow) => workflow.name === name).text;
        assert.doesNotMatch(text, /version-only|ci\/classify\.mjs|outputs\.cpp_validation|outputs\.portable_build/);
    }
});
