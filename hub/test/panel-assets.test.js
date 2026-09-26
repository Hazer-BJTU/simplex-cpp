/**
 * @file panel asset integrity.
 *
 * Two panels live here during the rewrite. `web/js` and `web/css` have no build
 * step and are served as-is, so nothing compiles them; `web/src` is TypeScript
 * that Vite bundles, so the compiler catches more of it but the bundle is what
 * actually runs. These checks cover the gap in both directions, and they run on
 * every Node version in the matrix:
 *
 *   - every local asset index.html references exists,
 *   - every legacy panel module parses as an ES module,
 *   - every relative import in the legacy panel resolves to a file that exists,
 *   - both theme token sets are still declared,
 *   - nothing writes markup as HTML, because model and tool output is untrusted,
 *   - and the markdown renderer does not re-enable inline HTML.
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

/**
 * Source with comments removed.
 *
 * The scans below look for tokens a panel must not use, and a comment that
 * explains *why* one is forbidden contains it too. Reading code rather than
 * prose is what keeps the rule writable down as well as up.
 *
 * Strings are tracked so a quote inside a comment does not desynchronise the
 * scan. The result is not a parser and does not need to be: the behavioural
 * check is `never turns model output into markup` in the browser suite, and
 * this is the cheap static backstop beside it.
 */
function stripComments(source) {
    let out = '';
    let quote = '';
    for (let index = 0; index < source.length; index += 1) {
        const char = source[index];
        const next = source[index + 1];
        if (quote) {
            out += char;
            if (char === '\\') {
                out += next ?? '';
                index += 1;
            } else if (char === quote) {
                quote = '';
            }
            continue;
        }
        if (char === '"' || char === "'" || char === '`') {
            quote = char;
            out += char;
            continue;
        }
        if (char === '/' && next === '*') {
            const end = source.indexOf('*/', index + 2);
            index = end === -1 ? source.length : end + 1;
            continue;
        }
        if (char === '/' && next === '/') {
            const end = source.indexOf('\n', index);
            index = end === -1 ? source.length : end;
            out += '\n';
            continue;
        }
        out += char;
    }
    return out;
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
        // `dangerouslySetInnerHTML` is the same hole with a React name on it,
        // and it is the one a markdown renderer is most likely to reach for.
        const offenders = [];
        for (const path of walk(webRoot)) {
            const source = stripComments(readFileSync(path, 'utf8'));
            for (const pattern of [/\.innerHTML\b/, /\.outerHTML\b/, /insertAdjacentHTML\b/,
                /dangerouslySetInnerHTML/, /document\.write\b/, /\beval\s*\(/,
                /new Function\s*\(/]) {
                if (pattern.test(source)) offenders.push(`${path.slice(hubRoot.length + 1)}: ${pattern}`);
            }
        }
        assert.deepEqual(offenders, []);
    });

    it('does not let the markdown renderer parse inline HTML', () => {
        // `react-markdown` is safe by construction — it builds elements, not
        // markup — but only while `rehype-raw` is absent. Adding that plugin is
        // a one-line change that would turn every `<img onerror=…>` a model
        // emits into a real element, so it is a change that has to be argued
        // for rather than made by accident.
        const newPanel = join(webRoot, 'src');
        const offenders = walk(newPanel)
            .filter((path) => /rehype-raw|rehypeRaw/.test(stripComments(readFileSync(path, 'utf8'))))
            .map((path) => path.slice(hubRoot.length + 1));
        assert.deepEqual(offenders, [],
            'a raw-HTML plugin is installed, so model output can become markup again');
    });
});
