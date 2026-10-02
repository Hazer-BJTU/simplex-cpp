import assert from 'node:assert/strict';
import { test } from 'node:test';
import { categoryForPath, classifyPaths, decisionsForCategory, validateDecisions } from './selection.mjs';
import { checkResults } from './gate.mjs';

const nativeTrees = [
    'core', 'dataclass', 'endpoint', 'extensions', 'intercom', 'io', 'llm', 'load',
    'loop', 'process', 'textedit', 'tools', 'utils', 'versioning', 'third_party',
];
for (const tree of nativeTrees) {
    test(`${tree}: source, docs and runtime resources select the complete chain`, () => {
        for (const file of ['src/a.cpp', 'include/a.hpp', 'test/fixture.json',
            'README.md', 'schemas/skill.yaml', 'scripts/launch', 'prompts/default.yaml']) {
            assert.deepEqual(classifyPaths([`${tree}/${file}`]), decisionsForCategory('native'));
        }
    });
}

for (const path of ['CMakeLists.txt', 'cmake/install.cmake', 'VERSION',
    'docker/Dockerfile.build-portable', 'docker/package-worker-release.sh',
    '.github/workflows/ci.yml', '.github/ci/selection.mjs',
    '.github/ci/selection.test.mjs', '.github/ci/README.md']) {
    test(`${path} forces full validation`, () => {
        assert.deepEqual(classifyPaths([path]), decisionsForCategory('native'));
    });
}

for (const path of ['hub/src/launch/config-render.ts', 'hub/shared/protocol.ts',
    'hub/bin/simplex-hub.ts', 'hub/test/e2e/e2e.test.js', 'hub/test/helpers/e2e.js',
    'hub/test/fixtures/fake-worker.js', 'hub/package-lock.json', 'hub/package.json',
    'hub/hub.config.example.jsonc', 'hub/tsconfig.json', 'hub/vite.config.ts']) {
    test(`${path} selects a fresh portable worker and integration`, () => {
        assert.deepEqual(classifyPaths([path]), decisionsForCategory('integration'));
    });
}

for (const path of ['hub/web/src/App.tsx', 'hub/test/browser/panel-shell.spec.ts',
    'docs/core/worker-protocol.md', 'docs/.vitepress/config.mts', 'README.md',
    'assets/simplex-logo.svg', '.github/workflows/docs.yml',
    '.github/workflows/release-worker.yml']) {
    test(`${path} does not require native work`, () => {
        assert.deepEqual(classifyPaths([path]), decisionsForCategory('independent'));
    });
}

for (const path of ['CONTRIBUTING.md', 'SECURITY.md',
    '.github/ISSUE_TEMPLATE/bug_report.yml', '.github/ISSUE_TEMPLATE/feature_request.yml',
    '.github/ISSUE_TEMPLATE/config.yml', '.github/PULL_REQUEST_TEMPLATE.md']) {
    test(`${path} is community documentation and omits native work`, () => {
        assert.equal(categoryForPath(path), 'documentation');
        assert.deepEqual(classifyPaths([path]), decisionsForCategory('independent'));
        assert.deepEqual(classifyPaths([path, 'core/src/application.cpp']),
            decisionsForCategory('native'));
        assert.deepEqual(classifyPaths([path, 'hub/src/hub.ts']),
            decisionsForCategory('integration'));
    });
}

test('community exceptions do not exclude other GitHub automation or similarly named paths', () => {
    for (const path of ['.github/workflows/new.yml', '.github/scripts/maintenance.sh',
        '.github/ISSUE_TEMPLATE-extra/action.yml', '.github/ISSUE_TEMPLATE.yml',
        '.github/PULL_REQUEST_TEMPLATE.md.js', 'SECURITY.md.sh', 'CONTRIBUTING.md.cmake']) {
        assert.equal(categoryForPath(path), 'native');
        assert.deepEqual(classifyPaths([path]), decisionsForCategory('native'));
    }
});

test('mixed changes take the union, unknown files default to native', () => {
    assert.deepEqual(classifyPaths(['docs/index.md', 'hub/src/hub.ts']),
        decisionsForCategory('integration'));
    assert.deepEqual(classifyPaths(['hub/web/app.ts', 'load/schemas/config.example.yaml']),
        decisionsForCategory('native'));
    for (const path of ['new-package/input.txt', 'future-build.conf', '.github/new-script.sh']) {
        assert.equal(classifyPaths([path]).category, 'native');
    }
    assert.equal(classifyPaths([]).category, 'independent');
});

test('invalid paths and inconsistent selections cannot be accepted', () => {
    for (const path of ['', '/tmp/file', '../core/file', 'docs/../core/file', 'docs//file', null]) {
        assert.throws(() => classifyPaths([path]));
    }
    assert.throws(() => classifyPaths(null));
    assert.throws(() => validateDecisions({
        ...decisionsForCategory('integration'), portable_build: false,
    }));
    assert.throws(() => validateDecisions({
        ...decisionsForCategory('native'), cpp_validation: 'true',
    }));
});

function needsFor(category) {
    const selection = decisionsForCategory(category);
    return {
        changes: { result: 'success', outputs: Object.fromEntries(
            Object.entries(selection).map(([key, value]) => [key, String(value)])) },
        'build-test': { result: selection.cpp_validation ? 'success' : 'skipped' },
        'portable-release': { result: selection.portable_build ? 'success' : 'skipped' },
        'staged-runtime': { result: selection.staged_validation ? 'success' : 'skipped' },
        'hub-e2e': { result: selection.worker_integration ? 'success' : 'skipped' },
        'hub-test': { result: 'success' },
        'hub-panel': { result: 'success' },
    };
}

for (const category of ['native', 'integration', 'independent']) {
    test(`gate accepts only the planned ${category} outcomes`, () => {
        checkResults(needsFor(category));
        for (const job of Object.keys(needsFor(category))) {
            for (const result of ['failure', 'cancelled', 'skipped', 'success', 'unknown', undefined]) {
                const needs = needsFor(category);
                if (result === needs[job].result) {
                    continue;
                }
                needs[job].result = result;
                assert.throws(() => checkResults(needs), `${job}: ${result}`);
            }
        }
    });
}

test('gate rejects missing jobs, malformed outputs and impossible producer/consumer choices', () => {
    const needs = needsFor('integration');
    delete needs['hub-e2e'];
    assert.throws(() => checkResults(needs));
    for (const value of ['', 'FALSE', undefined]) {
        const invalid = needsFor('independent');
        invalid.changes.outputs.portable_build = value;
        assert.throws(() => checkResults(invalid));
    }
    const invalid = needsFor('integration');
    invalid.changes.outputs.portable_build = 'false';
    assert.throws(() => checkResults(invalid));
    assert.throws(() => checkResults(null));
});
