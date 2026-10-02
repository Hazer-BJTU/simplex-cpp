import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { collectChangedPaths, parseChangedPaths, resolveRange } from './changes.mjs';
import { classifyPaths } from './selection.mjs';

const classifier = fileURLToPath(new URL('./classify.mjs', import.meta.url));

function fixture(t) {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-ci-range-'));
    t.after(() => rmSync(directory, { recursive: true, force: true }));
    const git = (...args) => execFileSync('git', args, {
        cwd: directory, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'],
        env: { ...process.env, GIT_CONFIG_NOSYSTEM: '1', GIT_CONFIG_GLOBAL: '/dev/null' },
    }).trim();
    git('init', '-b', 'main');
    git('config', 'user.name', 'CI range test');
    git('config', 'user.email', 'ci-test@example.invalid');
    const commit = (path, content = path) => {
        mkdirSync(join(directory, path, '..'), { recursive: true });
        writeFileSync(join(directory, path), content);
        git('add', '--', path);
        git('commit', '-m', `change ${path}`);
        return git('rev-parse', 'HEAD');
    };
    const base = commit('README.md', 'base');
    return { directory, git, commit, base };
}

test('NUL parsing preserves rename/copy paths, spaces, tabs and newlines', () => {
    assert.deepEqual(parseChangedPaths(Buffer.from(
        'R100\0core/old.cpp\0docs/new.md\0A\0hub/web/with\ttab\nname.ts\0C75\0a b\0copy\0D\0gone\0')),
    ['core/old.cpp', 'docs/new.md', 'hub/web/with\ttab\nname.ts', 'a b', 'copy', 'gone']);
    assert.deepEqual(parseChangedPaths(Buffer.alloc(0)), []);
    for (const data of ['A\0file', 'A\0', 'R100\0only-one\0', 'U\0file\0', 'R999\0a\0b\0']) {
        assert.throws(() => parseChangedPaths(Buffer.from(data)));
    }
    assert.throws(() => parseChangedPaths(Buffer.from([0xff, 0])));
});

test('PR merge checkout includes all PR commits and rename/deletion inputs', (t) => {
    const f = fixture(t);
    f.commit('core/original.cpp', 'native source');
    const base = f.git('rev-parse', 'HEAD');
    f.git('switch', '-c', 'feature');
    f.git('mv', 'core/original.cpp', 'docs-renamed.md');
    f.git('commit', '-m', 'move native input');
    f.commit('hub/web/with space\nand tab\t.ts');
    f.git('rm', 'README.md');
    f.git('commit', '-m', 'delete README');
    const head = f.git('rev-parse', 'HEAD');
    f.git('switch', 'main');
    f.commit('target-only.md');
    const target = f.git('rev-parse', 'HEAD');
    f.git('merge', '--no-ff', 'feature', '-m', 'PR test merge');
    const revision = f.git('rev-parse', 'HEAD');
    const range = resolveRange(f.directory, 'pull_request', {
        pull_request: { base: { sha: target }, head: { sha: head } },
    }, revision);
    assert.equal(range.base, target);
    const paths = collectChangedPaths(f.directory, range);
    assert.ok(paths.includes('core/original.cpp'));
    assert.ok(paths.includes('docs-renamed.md'));
    assert.ok(paths.includes('hub/web/with space\nand tab\t.ts'));
    assert.ok(paths.includes('README.md'));
    assert.equal(paths.includes('target-only.md'), false);
    assert.equal(classifyPaths(paths).category, 'native');
    assert.notEqual(base, target);
});

for (const strategy of ['multi-commit', 'squash', 'rebase', 'merge']) {
    test(`main ${strategy} push compares the entire before/after range`, (t) => {
        const f = fixture(t);
        if (strategy !== 'multi-commit') {
            f.git('switch', '-c', 'feature');
        }
        f.commit('hub/src/backend.ts');
        f.commit('docs/last-commit.md');
        if (strategy === 'squash' || strategy === 'merge') {
            f.git('switch', 'main');
            if (strategy === 'squash') {
                f.git('merge', '--squash', 'feature');
                f.git('commit', '-m', 'squash');
            } else {
                f.git('merge', '--no-ff', 'feature', '-m', 'merge');
            }
        } else if (strategy === 'rebase') {
            f.git('switch', 'main');
            f.commit('docs/base-advanced.md');
            f.git('switch', 'feature');
            f.git('rebase', 'main');
            f.git('switch', 'main');
            f.git('merge', '--ff-only', 'feature');
        }
        const after = f.git('rev-parse', 'HEAD');
        const range = resolveRange(f.directory, 'push', {
            ref: 'refs/heads/main', before: f.base, after,
        }, after);
        assert.equal(classifyPaths(collectChangedPaths(f.directory, range)).category, 'integration');
    });
}

function runClassifier(f, eventName, event, revision) {
    const eventPath = join(f.directory, 'event.json');
    const output = join(f.directory, 'output');
    const summary = join(f.directory, 'summary');
    writeFileSync(eventPath, JSON.stringify(event));
    writeFileSync(output, '');
    execFileSync(process.execPath, [classifier], {
        cwd: f.directory, encoding: 'utf8',
        env: { ...process.env, GITHUB_EVENT_NAME: eventName, GITHUB_EVENT_PATH: eventPath,
            GITHUB_SHA: revision, GITHUB_OUTPUT: output, GITHUB_STEP_SUMMARY: summary },
    });
    return readFileSync(output, 'utf8');
}

test('missing/zero/unavailable bases, unsupported events and mismatched checkout fall back to full', (t) => {
    const f = fixture(t);
    for (const before of [undefined, '0'.repeat(40), 'a'.repeat(40), 'not-a-sha']) {
        const output = runClassifier(f, 'push', {
            ref: 'refs/heads/main', before, after: f.base,
        }, f.base);
        assert.match(output, /category=native/);
        assert.match(output, /worker_integration=true/);
    }
    assert.match(runClassifier(f, 'workflow_dispatch', {}, f.base), /category=native/);
    const after = f.commit('docs/only.md');
    assert.match(runClassifier(f, 'push', {
        ref: 'refs/heads/main', before: f.base, after,
    }, f.base), /category=native/);
    assert.match(runClassifier(f, 'pull_request', {
        pull_request: { base: { sha: 'a'.repeat(40) }, head: { sha: after } },
    }, after), /category=native/);
});

test('entry point reports independent/integration/native selections for complete real diffs', (t) => {
    const f = fixture(t);
    for (const [path, category] of [
        ['docs/page.md', 'independent'], ['hub/web/app.ts', 'independent'],
        ['hub/src/hub.ts', 'integration'], ['core/prompts/default.yaml', 'native'],
        ['.github/ci/new-test.mjs', 'native'],
    ]) {
        const before = f.git('rev-parse', 'HEAD');
        const after = f.commit(path);
        assert.match(runClassifier(f, 'push', {
            ref: 'refs/heads/main', before, after,
        }, after), new RegExp(`category=${category}`));
    }
    // Corrupted event JSON must not select the independent category either.
    writeFileSync(join(f.directory, 'event.json'), '{');
    writeFileSync(join(f.directory, 'output'), '');
    execFileSync(process.execPath, [classifier], {
        cwd: f.directory,
        env: { ...process.env, GITHUB_EVENT_PATH: join(f.directory, 'event.json'),
            GITHUB_OUTPUT: join(f.directory, 'output'), GITHUB_STEP_SUMMARY: join(f.directory, 'summary') },
    });
    assert.match(readFileSync(join(f.directory, 'output'), 'utf8'), /category=native/);
});
