import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { collectChangedPaths, resolveRange } from './changes.mjs';
import { parseComparisonJson, verifyVersionContents, verifyVersionOnlyChange } from './version-only.mjs';

const classifier = fileURLToPath(new URL('./classify.mjs', import.meta.url));
const versionFiles = ['VERSION', 'hub/package.json', 'hub/package-lock.json'];

function contents(version) {
    const manifest = {
        name: '@example/hub', version, private: false,
        scripts: { test: 'node --test' }, engines: { node: '>=22' },
        dependencies: { sample: '^1.0.0' },
    };
    const lock = {
        name: manifest.name, version, lockfileVersion: 3, requires: true,
        packages: {
            '': { name: manifest.name, version, dependencies: manifest.dependencies },
            'node_modules/sample': {
                version: '1.0.0', resolved: 'https://example.invalid/sample.tgz',
                integrity: 'sha512-unchanged', optional: true,
            },
        },
    };
    return {
        VERSION: `${version}\n`,
        'hub/package.json': JSON.stringify(manifest, null, 2),
        'hub/package-lock.json': JSON.stringify(lock, null, 2),
    };
}

function mutateJson(files, path, mutate) {
    const value = JSON.parse(files[path]);
    mutate(value);
    files[path] = JSON.stringify(value);
}

test('only synchronized version values differ; JSON whitespace and object order are ignored', () => {
    const before = contents('0.1.3');
    const after = contents('0.1.4');
    for (const path of versionFiles.slice(1)) {
        const json = JSON.parse(after[path]);
        after[path] = JSON.stringify(Object.fromEntries(Object.entries(json).reverse()));
    }
    assert.deepEqual(verifyVersionContents(before, after), { from: '0.1.3', to: '0.1.4' });
    assert.equal(before.VERSION, '0.1.3\n');
    assert.equal(JSON.parse(after['hub/package.json']).version, '0.1.4');
});

const unrelatedChanges = [
    ['dependency', 'hub/package.json', (json) => { json.dependencies.sample = '^2.0.0'; }],
    ['script', 'hub/package.json', (json) => { json.scripts.test = 'node malicious.js'; }],
    ['engine', 'hub/package.json', (json) => { json.engines.node = '>=24'; }],
    ['package name', 'hub/package.json', (json) => { json.name = '@other/hub'; }],
    ['lockfile format', 'hub/package-lock.json', (json) => { json.lockfileVersion = 2; }],
    ['root dependency', 'hub/package-lock.json', (json) => { json.packages[''].dependencies.sample = '^2.0.0'; }],
    ['nested version', 'hub/package-lock.json', (json) => { json.packages['node_modules/sample'].version = '2.0.0'; }],
    ['integrity', 'hub/package-lock.json', (json) => { json.packages['node_modules/sample'].integrity = 'sha512-other'; }],
    ['resolution', 'hub/package-lock.json', (json) => { json.packages['node_modules/sample'].resolved = 'https://other.invalid'; }],
    ['boolean', 'hub/package-lock.json', (json) => { json.packages['node_modules/sample'].optional = false; }],
    ['new key', 'hub/package-lock.json', (json) => { json.newField = null; }],
    ['removed key', 'hub/package-lock.json', (json) => { delete json.requires; }],
];
for (const [name, path, mutate] of unrelatedChanges) {
    test(`a valid version bump cannot hide a changed ${name}`, () => {
        const after = contents('0.1.4');
        mutateJson(after, path, mutate);
        assert.equal(verifyVersionContents(contents('0.1.3'), after), null);
    });
}

test('comparison preserves arrays, types, number spelling and large integer differences', () => {
    for (const [oldValue, newValue] of [
        ['[1,2]', '[2,1]'], ['1', '"1"'], ['1', '1.0'],
        ['9007199254740992', '9007199254740993'], ['1e999', '2e999'], ['-0', '0'],
    ]) {
        const before = contents('0.1.3');
        const after = contents('0.1.4');
        before['hub/package.json'] = before['hub/package.json'].replace('{', `{"extra":${oldValue},`);
        after['hub/package.json'] = after['hub/package.json'].replace('{', `{"extra":${newValue},`);
        assert.equal(verifyVersionContents(before, after), null, `${oldValue} → ${newValue}`);
    }
});

test('all version copies must exist and agree on both sides', () => {
    for (const side of ['before', 'after']) {
        for (const path of versionFiles.slice(1)) {
            for (const replacement of [undefined, null, 14, '0.9.9']) {
                const before = contents('0.1.3');
                const after = contents('0.1.4');
                mutateJson(side === 'before' ? before : after, path,
                    (json) => { json.version = replacement; });
                assert.equal(verifyVersionContents(before, after), null);
            }
        }
        for (const replacement of [undefined, null, 14, '0.9.9']) {
            const before = contents('0.1.3');
            const after = contents('0.1.4');
            mutateJson(side === 'before' ? before : after, 'hub/package-lock.json',
                (json) => { json.packages[''].version = replacement; });
            assert.equal(verifyVersionContents(before, after), null);
        }
    }
    const after = contents('0.1.4');
    mutateJson(after, 'hub/package-lock.json', (json) => { delete json.packages; });
    assert.equal(verifyVersionContents(contents('0.1.3'), after), null);
    for (const version of ['01.1.4', 'v0.1.4', '0.1', '0.1.4-beta', '', '0.1.3']) {
        assert.equal(verifyVersionContents(contents('0.1.3'), contents(version)), null);
        assert.equal(verifyVersionContents(contents(version), contents('0.1.3')), null);
    }
});

test('duplicate keys, escaped duplicate keys and malformed JSON cannot bypass', () => {
    for (const text of [
        '{"version":"0.1.4","version":"0.1.4"}',
        '{"version":"0.1.4","\\u0076ersion":"0.1.4"}',
        '{"version":"0.1.4","nested":{"x":1,"x":1}}',
        '{"version":"0.1.4",}', '{"version":"0.1.4"} false',
        '{"version":"0.1.4","x":[1,]}', '{"version":"0.1.4","x":01}',
        '{"version":"0.1.4","x":"\\x00"}', '{', '',
    ]) {
        for (const side of ['before', 'after']) {
            const before = contents('0.1.3');
            const after = contents('0.1.4');
            (side === 'before' ? before : after)['hub/package.json'] = text;
            assert.throws(() => verifyVersionContents(before, after));
        }
    }
    assert.throws(() => parseComparisonJson('['.repeat(130) + '0' + ']'.repeat(130)), /depth limit/);
});

/** Isolated real Git repositories exercise metadata and full event comparisons. */
function fixture(t, initial = contents('0.1.3')) {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-ci-version-'));
    t.after(() => rmSync(directory, { recursive: true, force: true }));
    const git = (...args) => execFileSync('git', args, {
        cwd: directory, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'],
        env: { ...process.env, GIT_CONFIG_NOSYSTEM: '1', GIT_CONFIG_GLOBAL: '/dev/null' },
    }).trim();
    git('init', '-b', 'main');
    git('config', 'user.name', 'CI version test');
    git('config', 'user.email', 'ci-test@example.invalid');
    git('config', 'core.fileMode', 'true');
    const save = (files) => {
        for (const [path, text] of Object.entries(files)) {
            mkdirSync(join(directory, path, '..'), { recursive: true });
            writeFileSync(join(directory, path), text);
        }
    };
    const commit = () => {
        git('add', '-A');
        git('commit', '-m', 'fixture change');
        return git('rev-parse', 'HEAD');
    };
    save({ 'README.md': 'base', ...initial });
    const base = commit();
    return { directory, git, save, commit, base };
}

function runClassifier(f, eventName, event, revision, environment = {}) {
    const metadata = join(f.directory, '.git', 'ci-fixture');
    mkdirSync(metadata, { recursive: true });
    const eventPath = join(metadata, 'event.json');
    const output = join(metadata, 'output');
    const summary = join(metadata, 'summary');
    writeFileSync(eventPath, JSON.stringify(event));
    writeFileSync(output, '');
    writeFileSync(summary, '');
    execFileSync(process.execPath, [classifier], {
        cwd: f.directory, stdio: ['ignore', 'pipe', 'pipe'],
        env: { ...process.env, GITHUB_EVENT_NAME: eventName, GITHUB_EVENT_PATH: eventPath,
            GITHUB_SHA: revision, GITHUB_OUTPUT: output, GITHUB_STEP_SUMMARY: summary,
            ...environment },
    });
    return {
        outputs: Object.fromEntries(readFileSync(output, 'utf8').trim().split('\n')
            .map((line) => line.split('='))),
        summary: readFileSync(summary, 'utf8'),
    };
}

for (const eventName of ['pull_request', 'push']) {
    test(`${eventName}: synchronized bump skips all worker jobs and reports the transition`, (t) => {
        const f = fixture(t);
        f.git('switch', '-c', 'bump');
        f.save(contents('0.1.4'));
        const head = f.commit();
        f.git('switch', 'main');
        if (eventName === 'pull_request') {
            f.save({ 'docs/target-only.md': 'target advance' });
            f.commit();
        }
        const base = f.git('rev-parse', 'HEAD');
        f.git('merge', '--no-ff', 'bump', '-m', 'merge bump');
        const revision = f.git('rev-parse', 'HEAD');
        const event = eventName === 'pull_request'
            ? { pull_request: { base: { sha: base }, head: { sha: head } } }
            : { ref: 'refs/heads/main', before: base, after: revision };
        const { outputs, summary } = runClassifier(f, eventName, event, revision);
        assert.deepEqual(outputs, {
            category: 'version-only', cpp_validation: 'false', portable_build: 'false',
            staged_validation: 'false', worker_integration: 'false',
            version_from: '0.1.3', version_to: '0.1.4',
        });
        assert.match(summary, /content-verified version-only bypass: 0\.1\.3 → 0\.1\.4/);
        const range = resolveRange(f.directory, eventName, event, revision);
        assert.deepEqual(verifyVersionOnlyChange(f.directory, range,
            collectChangedPaths(f.directory, range)), { from: '0.1.3', to: '0.1.4' });
    });

    test(`${eventName}: an earlier implementation commit cannot hide behind a final bump`, (t) => {
        const f = fixture(t);
        f.git('switch', '-c', 'mixed');
        f.save({ 'core/src/implementation.cpp': 'implementation' });
        f.commit();
        f.save(contents('0.1.4'));
        const head = f.commit();
        f.git('switch', 'main');
        f.git('merge', '--no-ff', 'mixed', '-m', 'merge mixed');
        const revision = f.git('rev-parse', 'HEAD');
        const event = eventName === 'pull_request'
            ? { pull_request: { base: { sha: f.base }, head: { sha: head } } }
            : { ref: 'refs/heads/main', before: f.base, after: revision };
        const { outputs } = runClassifier(f, eventName, event, revision);
        assert.equal(outputs.category, 'native');
        assert.equal(outputs.worker_integration, 'true');
    });
}

for (const mutation of ['mode', 'symlink', 'addition', 'copy', 'deletion', 'rename', 'extra-path']) {
    test(`Git ${mutation} does not qualify for the version bypass`, (t) => {
        const initial = contents('0.1.3');
        if (mutation === 'addition' || mutation === 'copy') {
            delete initial.VERSION;
        }
        if (mutation === 'copy') {
            initial['original-version'] = '0.1.4\n';
        }
        const f = fixture(t, initial);
        f.save(contents('0.1.4'));
        if (mutation === 'mode') {
            f.git('add', '-A');
            f.git('update-index', '--chmod=+x', 'VERSION');
            f.git('config', 'core.fileMode', 'false');
        } else if (mutation === 'symlink') {
            rmSync(join(f.directory, 'VERSION'));
            symlinkSync('README.md', join(f.directory, 'VERSION'));
        } else if (mutation === 'deletion') {
            rmSync(join(f.directory, 'VERSION'));
        } else if (mutation === 'rename') {
            f.git('mv', 'VERSION', 'old-version');
        } else if (mutation === 'extra-path') {
            f.save({ 'docs/additional.md': 'docs' });
        }
        const head = f.commit();
        const range = { base: f.base, head };
        assert.equal(verifyVersionOnlyChange(f.directory, range,
            collectChangedPaths(f.directory, range)), null);
        assert.equal(runClassifier(f, 'push', {
            ref: 'refs/heads/main', before: f.base, after: head,
        }, head).outputs.category, 'native');
    });
}

test('both sides may be executable regular files if their modes remain unchanged', (t) => {
    const f = fixture(t);
    f.git('update-index', '--chmod=+x', 'VERSION');
    f.git('commit', '-m', 'existing executable mode');
    f.git('config', 'core.fileMode', 'false');
    const base = f.git('rev-parse', 'HEAD');
    f.save(contents('0.1.4'));
    const head = f.commit();
    assert.deepEqual(verifyVersionOnlyChange(f.directory, { base, head }, versionFiles),
        { from: '0.1.3', to: '0.1.4' });
});

test('unreadable blobs, invalid UTF-8 and ambiguous JSON select full fallback', (t) => {
    const f = fixture(t);
    f.save(contents('0.1.4'));
    const head = f.commit();
    const event = { ref: 'refs/heads/main', before: f.base, after: head };
    const gitPath = execFileSync('which', ['git'], { encoding: 'utf8' }).trim();
    const wrappers = join(f.directory, '.git', 'wrappers');
    mkdirSync(wrappers);
    // The wrapper only injects a read failure; all range and diff operations use
    // the real Git executable. Shell quoting preserves paths containing spaces.
    writeFileSync(join(wrappers, 'git'),
        `#!/bin/sh\nif [ "$1" = cat-file ]; then exit 1; fi\nexec '${gitPath.replaceAll("'", "'\\''")}' "$@"\n`,
        { mode: 0o755 });
    const failed = runClassifier(f, 'push', event, head,
        { PATH: `${wrappers}:${process.env.PATH}` });
    assert.equal(failed.outputs.category, 'native');
    assert.match(failed.summary, /Full-validation fallback/);
    for (const data of [Buffer.from([0xff]), '\ufeff' + contents('0.1.4')['hub/package.json'],
        '{"version":"0.1.4","version":"0.1.4"}']) {
        f.save({ 'hub/package.json': data });
        const revision = f.commit();
        const result = runClassifier(f, 'push', { ...event, after: revision }, revision);
        assert.equal(result.outputs.category, 'native');
        assert.match(result.summary, /Full-validation fallback/);
    }
});
