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

test('musl validation stays manual for development and is reusable for releases', () => {
    const musl = workflows.find((workflow) => workflow.name === 'musl-portable.yml').text;
    const triggers = musl.slice(musl.indexOf('\non:\n'), musl.indexOf('\npermissions:\n'));
    assert.match(triggers, /workflow_dispatch:/);
    assert.match(triggers, /workflow_call:/);
    assert.doesNotMatch(triggers, /push:|pull_request:|schedule:/);
    assert.match(musl, /build-musl:experimental@sha256:[a-f0-9]{64}/);
    const dockerfile = readFileSync(new URL('../../docker/Dockerfile.musl-test', import.meta.url), 'utf8');
    assert.match(dockerfile, /CMAKE_BUILD_TYPE=Release/);
    assert.match(dockerfile, /SIMPLEX_ENABLE_LTO=OFF/);
    assert.doesNotMatch(musl, /Debug|contents: write|packages: write/);
    assert.match(musl, /ctest .*--no-tests=error .*--timeout 120/);
    assert.match(musl, /image: \[ubuntu:22\.04, almalinux:9\]/);
    assert.match(musl, /SIMPLEX_E2E_REQUIRED: '1'/);
    for (const name of ['ci.yml']) {
        const text = workflows.find((workflow) => workflow.name === name).text;
        assert.doesNotMatch(text, /musl-portable|build-musl|simplex-musl-ci/);
    }
});

test('musl validates kernel-sized Boost.Test signal stacks before the full suite', () => {
    const musl = workflows.find(workflow => workflow.name === 'musl-portable.yml').text;
    const regression = musl.indexOf('bash /src/docker/test-musl-boost-monitor.sh');
    assert.ok(regression > 0 && regression < musl.indexOf('ctest --test-dir /build'));
    const recipe = readFileSync(new URL('../../docker/Dockerfile.musl-test', import.meta.url), 'utf8');
    assert.match(recipe, /patch -p1 < \/tmp\/boost-test-musl-alt-stack\.patch/);
    assert.match(recipe, /cp -a \/usr\/local\/lib\/libboost_unit_test_framework\.so\* \/opt\/simplex-runtime\//);
    assert.doesNotMatch(musl + recipe, /BOOST_TEST_USE_ALT_STACK=no|BOOST_TEST_CATCH_SYSTEM_ERRORS=no/);
});

test('tag releases use the patched musl build and probe its staged Boost.Test', () => {
    const release = workflows.find(workflow => workflow.name === 'release-worker.yml').text;
    assert.match(release, /uses: \.\/.github\/workflows\/musl-portable.yml/);
    const musl = workflows.find(workflow => workflow.name === 'musl-portable.yml').text;
    assert.match(musl, /bash \/src\/docker\/test-musl-boost-monitor.sh \/opt\/simplex/);
    const probe = readFileSync(new URL('../../docker/test-musl-boost-monitor.sh', import.meta.url), 'utf8');
    assert.match(probe, /for mode in build bundled/);
    assert.match(probe, /"\$loader" --library-path "\$stage\/lib"/);
    assert.match(probe, /dl_iterate_phdr/);
    assert.match(probe, /SIMPLEX_EXPECT_BOOST_DIRECTORY="\$stage\/lib"/);
});
