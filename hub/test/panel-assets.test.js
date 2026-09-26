/**
 * @file panel asset integrity.
 *
 * The panel has no build step: `hub/web` is served as-is. Nothing compiles it,
 * so nothing catches a renamed file, a typo in a module path, or a syntax error
 * until a browser console shows it — and on a headless CI run there is no
 * browser unless the end-to-end job provides one (see test/e2e/panel.test.js).
 *
 * These checks are the cheap half of that gap, and they run on every Node
 * version in the matrix:
 *   - every local asset index.html references exists,
 *   - every panel module parses as an ES module,
 *   - every relative import resolves to a file that exists,
 *   - both theme token sets are still declared,
 *   - nothing writes markup as HTML, because model and tool output is untrusted.
 *
 * None of this proves the panel works. It proves the files it needs are there
 * and are syntactically loadable, which is the part a browser test would
 * otherwise be the only witness to.
 */
import assert from 'node:assert/strict';
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { describe, it } from 'node:test';
import { hubRoot } from '../src/config.ts';

const webRoot = join(hubRoot, 'web');
const jsRoot = join(webRoot, 'js');

/**
 * Directories the integrity checks skip.
 *
 * `dist` is build output: it contains bundled third-party code, which has its
 * own opinions about `innerHTML` and is not ours to police. `node_modules` is
 * the same argument. The sources that produce them are checked instead.
 */
const SKIP_DIRECTORIES = new Set(['node_modules', 'dist']);

/** Every file under a directory, recursively, skipping build output. */
function walk(directory) {
    const found = [];
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
        if (entry.isDirectory() && SKIP_DIRECTORIES.has(entry.name)) continue;
        const path = join(directory, entry.name);
        if (entry.isDirectory()) found.push(...walk(path));
        else found.push(path);
    }
    return found;
}

/** Relative specifiers imported or re-exported by one module. */
function importSpecifiers(source) {
    const specifiers = [];
    const patterns = [
        /(?:^|\n)\s*import\s+[^'"]*?from\s*['"]([^'"]+)['"]/g,
        /(?:^|\n)\s*import\s*['"]([^'"]+)['"]/g,
        /(?:^|\n)\s*export\s+[^'"]*?from\s*['"]([^'"]+)['"]/g,
    ];
    for (const pattern of patterns) {
        for (const match of source.matchAll(pattern)) specifiers.push(match[1]);
    }
    return specifiers;
}

describe('panel assets', () => {
    it('references only files that exist', () => {
        const html = readFileSync(join(webRoot, 'index.html'), 'utf8');
        const references = [...html.matchAll(/(?:src|href)="([^"]+)"/g)].map((match) => match[1]);
        assert.ok(references.length > 0, 'index.html references nothing at all');
        for (const reference of references) {
            if (/^(?:[a-z]+:|\/\/|#)/i.test(reference)) continue;
            const path = join(webRoot, reference.replace(/^\//, '').split('?')[0]);
            assert.ok(existsSync(path), `index.html references a missing asset: ${reference}`);
        }
        // The modules are reached through main.js; assert the entry point is
        // one of the referenced files rather than trusting the loop above.
        assert.ok(references.some((reference) => reference.includes('js/main.js')),
            'index.html no longer loads js/main.js');
    });

    it('parses every panel module as an ES module', () => {
        const modules = readdirSync(jsRoot).filter((name) => name.endsWith('.js'));
        assert.ok(modules.length >= 4, `expected several panel modules, found ${modules.length}`);
        for (const name of modules) {
            const path = join(jsRoot, name);
            const result = spawnSync(
                process.execPath,
                ['--input-type=module', '--check'],
                { input: readFileSync(path, 'utf8'), encoding: 'utf8' },
            );
            assert.equal(result.status, 0,
                `${name} is not a loadable ES module: ${result.stderr?.trim()}`);
        }
    });

    it('resolves every relative import to a file that exists', () => {
        let checked = 0;
        for (const path of walk(jsRoot).filter((file) => file.endsWith('.js'))) {
            for (const specifier of importSpecifiers(readFileSync(path, 'utf8'))) {
                if (!specifier.startsWith('.')) continue;
                checked += 1;
                const target = resolve(dirname(path), specifier);
                assert.ok(existsSync(target) && statSync(target).isFile(),
                    `${path.slice(hubRoot.length + 1)} imports a missing module: ${specifier}`);
            }
        }
        assert.ok(checked > 0, 'no relative imports found; the panel would not be wired up');
    });

    it('keeps both theme token sets', () => {
        const css = readFileSync(join(webRoot, 'css', 'app.css'), 'utf8');
        assert.match(css, /:root\s*\{/, 'the light token set is gone');
        assert.match(css, /\[data-theme="dark"\]\s*\{/, 'the dark token set is gone');
        // The toggle drives this attribute; a panel that stopped setting it
        // would still pass the two matches above.
        const panel = walk(jsRoot).map((file) => readFileSync(file, 'utf8')).join('\n');
        assert.match(panel, /dataset\.theme|setAttribute\(\s*['"]data-theme['"]/,
            'nothing sets data-theme, so the dark theme is unreachable');
    });

    it('never writes panel markup as HTML', () => {
        // Model output, tool output, log lines and session ids are all
        // untrusted; a single innerHTML would turn any of them into script.
        const offenders = [];
        for (const path of walk(webRoot)) {
            const source = readFileSync(path, 'utf8');
            for (const pattern of [/\.innerHTML\b/, /\.outerHTML\b/, /insertAdjacentHTML\b/,
                /document\.write\b/, /\beval\s*\(/, /new Function\s*\(/]) {
                if (pattern.test(source)) offenders.push(`${path.slice(hubRoot.length + 1)}: ${pattern}`);
            }
        }
        assert.deepEqual(offenders, []);
    });
});
