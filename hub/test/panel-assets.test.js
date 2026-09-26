/**
 * @file panel asset integrity.
 *
 * The panel is TypeScript that Vite bundles, so most of it is checked by the
 * compiler and by the browser suite. These four checks cover what neither does:
 *
 *   - the entry document references only files that exist,
 *   - every relative import in the panel's own sources resolves,
 *   - both theme token sets are still declared,
 *   - nothing writes markup as HTML, because model and tool output is untrusted,
 *     and the markdown renderer has not re-enabled inline HTML.
 *
 * There used to be a second, build-free panel here, and half of this file was
 * about it: its modules were checked with `node --check` because nothing else
 * compiled them. That panel is gone (P8), and so are those checks — a test that
 * verifies a deleted thing is worse than no test, because it reads as coverage.
 */
import assert from 'node:assert/strict';
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { describe, it } from 'node:test';
import { hubRoot } from '../src/config.ts';

const webRoot = join(hubRoot, 'web');
const sourceRoot = join(webRoot, 'src');

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
        assert.ok(references.some((reference) => reference.includes('src/main.tsx')),
            'index.html no longer loads the panel entry point');
    });

    it('resolves every relative import in the panel to a file that exists', () => {
        // The bundler would fail on a missing import, but this runs in the unit
        // suite, on every Node in the matrix, without a build — so a broken
        // specifier is caught before anything is installed or bundled.
        let checked = 0;
        for (const path of walk(sourceRoot).filter((file) => /\.tsx?$/.test(file))) {
            for (const specifier of importSpecifiers(readFileSync(path, 'utf8'))) {
                if (!specifier.startsWith('.')) continue;
                checked += 1;
                const target = resolve(dirname(path), specifier);
                assert.ok(existsSync(target) && statSync(target).isFile(),
                    `${path.slice(hubRoot.length + 1)} imports a missing module: ${specifier}`);
            }
        }
        assert.ok(checked > 20, `expected the panel to have imports, found ${checked}`);
    });

    it('keeps both theme token sets', () => {
        const css = readFileSync(join(sourceRoot, 'styles', 'app.css'), 'utf8');
        assert.match(css, /:root\s*\{/, 'the light token set is gone');
        assert.match(css, /\[data-theme="dark"\]\s*\{/, 'the dark token set is gone');
        // The two sets have to declare the same names, or a token that exists in
        // one theme and not the other renders as an invalid value in the other.
        const names = (block) => new Set([...block.matchAll(/(--[a-z-]+):/g)].map((m) => m[1]));
        const light = names(css.slice(css.indexOf(':root {'), css.indexOf('[data-theme="dark"]')));
        const dark = names(css.slice(css.indexOf('[data-theme="dark"]')));
        const missing = [...light].filter((name) => !dark.has(name));
        assert.deepEqual(missing, [], 'a token is declared for light and not for dark');
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
        const offenders = walk(sourceRoot)
            .filter((path) => /rehype-raw|rehypeRaw/.test(stripComments(readFileSync(path, 'utf8'))))
            .map((path) => path.slice(hubRoot.length + 1));
        assert.deepEqual(offenders, [],
            'a raw-HTML plugin is installed, so model output can become markup again');
    });
});
