/**
 * @file the theme, and the static rules that keep the token layer honest.
 *
 * The controller is a pure function of what it is handed — a target, a storage,
 * and a yes/no about the operating system — so every branch is reachable
 * without a browser. The three checks at the end are of a different kind: they
 * read the panel's own source and fail on the mistakes this stage actually
 * made, each of which a type checker and a browser test both let through:
 *
 *   - Tailwind's palette switched off *after* the panel's tokens were declared,
 *     which silently deleted every colour utility in the build;
 *   - a JSX attribute written as `attr="a" + \`b\``, which is not a
 *     concatenation at all — React renders the second half as class text;
 *   - two icons packages, two sizes, and no rule.
 */
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { hubRoot } from '../src/config.ts';
import {
    THEME_ATTRIBUTE,
    THEME_KEY,
    THEME_PREFERENCE_ATTRIBUTE,
    createThemeController,
    describeTheme,
    isThemePreference,
    nextThemePreference,
    readThemePreference,
    resolveTheme,
} from '../web/src/ui/theme.ts';

/** A storage backed by a Map, with the two behaviours a theme needs. */
function storage(initial = {}) {
    const map = new Map(Object.entries(initial));
    return {
        getItem: (key) => (map.has(key) ? map.get(key) : null),
        setItem: (key, value) => { map.set(key, value); },
        removeItem: (key) => { map.delete(key); },
        peek: () => map,
    };
}

/** A target that records what was written to it. */
function target() {
    const attributes = new Map();
    return {
        setAttribute: (name, value) => { attributes.set(name, value); },
        get: (name) => attributes.get(name) ?? null,
        all: () => attributes,
    };
}

describe('theme preference', () => {
    it('resolves a three-way preference against the operating system', () => {
        assert.equal(resolveTheme('light', true), 'light');
        assert.equal(resolveTheme('dark', false), 'dark');
        assert.equal(resolveTheme('system', true), 'dark');
        assert.equal(resolveTheme('system', false), 'light');
    });

    it('cycles light, dark, system', () => {
        assert.equal(nextThemePreference('light'), 'dark');
        assert.equal(nextThemePreference('dark'), 'system');
        assert.equal(nextThemePreference('system'), 'light');
    });

    it('accepts only the three preferences', () => {
        assert.ok(isThemePreference('dark'));
        assert.ok(!isThemePreference('sepia'));
        assert.ok(!isThemePreference(null));
        assert.ok(!isThemePreference(undefined));
    });

    it('falls back to system for a stored value it does not understand', () => {
        // A preference written by a future version must not stop the panel from
        // drawing; following the operating system is the safe reading.
        assert.equal(readThemePreference(storage({ [THEME_KEY]: 'sepia' })), 'system');
        assert.equal(readThemePreference(storage()), 'system');
        assert.equal(readThemePreference(null), 'system');
        assert.equal(readThemePreference(storage({ [THEME_KEY]: 'dark' })), 'dark');
    });

    it('survives a storage that throws on read', () => {
        const hostile = {
            getItem: () => { throw new Error('blocked'); },
            setItem: () => { throw new Error('blocked'); },
            removeItem: () => {},
        };
        assert.equal(readThemePreference(hostile), 'system');
        const controller = createThemeController({
            target: target(), storage: hostile, systemPrefersDark: () => true,
        });
        // The write fails, the theme still changes: persistence is a nicety.
        assert.equal(controller.set('light'), 'light');
        assert.equal(controller.preference(), 'light');
    });
});

describe('theme controller', () => {
    it('writes both attributes on creation, before any render', () => {
        const element = target();
        createThemeController({
            target: element, storage: storage(), systemPrefersDark: () => true,
        });
        assert.equal(element.get(THEME_ATTRIBUTE), 'dark');
        assert.equal(element.get(THEME_PREFERENCE_ATTRIBUTE), 'system');
    });

    it('persists the preference and applies it immediately', () => {
        const element = target();
        const store = storage();
        const controller = createThemeController({
            target: element, storage: store, systemPrefersDark: () => false,
        });
        assert.equal(controller.set('dark'), 'dark');
        assert.equal(store.peek().get(THEME_KEY), 'dark');
        assert.equal(element.get(THEME_ATTRIBUTE), 'dark');
        assert.equal(element.get(THEME_PREFERENCE_ATTRIBUTE), 'dark');

        assert.equal(controller.cycle(), 'system');
        assert.equal(element.get(THEME_ATTRIBUTE), 'light');
        assert.equal(store.peek().get(THEME_KEY), 'system');
    });

    it('starts from storage rather than from the default', () => {
        const element = target();
        const controller = createThemeController({
            target: element,
            storage: storage({ [THEME_KEY]: 'dark' }),
            systemPrefersDark: () => false,
        });
        assert.equal(controller.preference(), 'dark');
        assert.equal(controller.resolved(), 'dark');
    });

    it('follows the operating system only while the preference is system', () => {
        const element = target();
        let notify = () => {};
        let stop = 0;
        const controller = createThemeController({
            target: element,
            storage: storage(),
            systemPrefersDark: () => false,
            watchSystem: (onChange) => { notify = onChange; return () => { stop += 1; }; },
        });
        assert.equal(element.get(THEME_ATTRIBUTE), 'light');

        notify(true);
        assert.equal(element.get(THEME_ATTRIBUTE), 'dark', 'system did not follow the OS');

        controller.set('light');
        notify(true);
        assert.equal(element.get(THEME_ATTRIBUTE), 'light', 'a fixed preference moved anyway');

        controller.dispose();
        assert.equal(stop, 1);
    });

    it('notifies subscribers once per real change', () => {
        let notify = () => {};
        const controller = createThemeController({
            target: target(),
            storage: storage(),
            systemPrefersDark: () => false,
            watchSystem: (onChange) => { notify = onChange; return () => {}; },
        });
        let calls = 0;
        const unsubscribe = controller.subscribe(() => { calls += 1; });
        notify(false);          // already light: nothing changed
        assert.equal(calls, 0);
        notify(true);
        assert.equal(calls, 1);
        unsubscribe();
        notify(false);
        assert.equal(calls, 1, 'an unsubscribed listener was still called');
    });

    it('re-reads the operating system on refresh', () => {
        let dark = false;
        const element = target();
        const controller = createThemeController({
            target: element, storage: storage(), systemPrefersDark: () => dark,
        });
        dark = true;
        assert.equal(controller.refresh(), 'dark');
        assert.equal(element.get(THEME_ATTRIBUTE), 'dark');
    });

    it('labels the preference and what it currently means', () => {
        assert.deepEqual(describeTheme('light', 'light'), {
            label: 'light', detail: 'always light',
        });
        assert.deepEqual(describeTheme('system', 'dark'), {
            label: 'system (dark)',
            detail: 'following the operating system, which asks for dark',
        });
    });
});

describe('panel sources', () => {
    const webRoot = join(hubRoot, 'web');
    const sourceRoot = join(webRoot, 'src');

    /** Every file under the panel source, without build output. */
    function sources(directory = sourceRoot) {
        return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
            const path = join(directory, entry.name);
            if (entry.isDirectory()) return sources(path);
            return /\.tsx?$/.test(entry.name) ? [path] : [];
        });
    }

    it('keeps the boot script and the controller agreeing on the key', () => {
        // The panel sets the theme twice: once in an inline script before the
        // bundle loads (so a dark preference does not flash white), and once in
        // `ui/theme.ts`. They have to agree, and nothing else checks that.
        const pages = readdirSync(webRoot)
            .filter((name) => name.endsWith('.html'))
            .map((name) => join(webRoot, name));
        // Only the pages that carry the boot script: `data-theme` alone is
        // just the attribute on `<html>` that the CSS defaults to light for.
        const boot = pages
            .map((path) => readFileSync(path, 'utf8'))
            .filter((html) => html.includes('localStorage.getItem'));
        assert.ok(boot.length > 0, 'no page sets data-theme before the bundle loads');
        for (const html of boot) {
            assert.ok(html.includes(THEME_KEY), `the boot script does not read ${THEME_KEY}`);
            assert.ok(html.includes(THEME_PREFERENCE_ATTRIBUTE),
                'the boot script does not record the preference');
        }
    });

    it('declares the palette reset before the tokens it would clear', () => {
        // `--color-*: initial` inside the same `@theme` block clears the tokens
        // declared above it, and the result is a build with no colour utilities
        // at all — which compiles, renders, and is wrong everywhere.
        const css = readFileSync(join(sourceRoot, 'styles', 'app.css'), 'utf8');
        const reset = css.indexOf('--color-*: initial');
        const firstToken = css.indexOf('--color-app:');
        assert.ok(reset !== -1, 'the default palette is no longer switched off');
        assert.ok(firstToken !== -1, 'the panel no longer declares its own tokens');
        assert.ok(reset < firstToken,
            'the palette reset comes after the panel tokens, so it deletes them');

        const themeBlocks = [...css.matchAll(/@theme[^{]*\{/g)].map((match) => match.index);
        const resetBlock = themeBlocks.filter((start) => start < reset).pop();
        const tokenBlock = themeBlocks.filter((start) => start < firstToken).pop();
        assert.notEqual(resetBlock, tokenBlock,
            'the reset and the tokens share one @theme block, where order decides');
    });

    it('uses no palette utility in a component', () => {
        // The default palette is off, so `text-slate-500` styles nothing in
        // either theme. The point of catching it here is that it fails loudly
        // rather than looking fine in light mode.
        const palette = /\b(?:bg|text|border|ring|outline|fill|stroke|divide|accent|caret|decoration|shadow|from|to|via)-(?:l|r|t|b|x|y|s|e)?-?(?:slate|gray|zinc|neutral|stone|red|orange|amber|yellow|lime|green|emerald|teal|cyan|sky|blue|indigo|violet|purple|fuchsia|pink|rose|white|black)(?:-\d{2,3})?\b/;
        const offenders = sources()
            .filter((path) => palette.test(readFileSync(path, 'utf8')))
            .map((path) => `${path.slice(hubRoot.length + 1)}: ${readFileSync(path, 'utf8').match(palette)[0]}`);
        assert.deepEqual(offenders, []);
    });

    it('imports icons from one module', () => {
        const offenders = sources()
            .filter((path) => !path.endsWith(join('ui', 'icons.tsx')))
            .filter((path) => readFileSync(path, 'utf8').includes("from 'lucide-react'"))
            .map((path) => path.slice(hubRoot.length + 1));
        assert.deepEqual(offenders, [],
            'an icon is imported straight from lucide, so the size and stroke rules are bypassed');
    });

    it('writes no JSX attribute as a string concatenation', () => {
        // `className="a" + \`b\`` is not a concatenation in JSX: the parser ends
        // the attribute at the second quote and renders the rest as text. It
        // type-checks, it builds, and it silently drops every class after the
        // first line — which is how the dialog spent three stages with no
        // background and no horizontal centring.
        const offenders = sources().filter((path) => {
            const source = readFileSync(path, 'utf8');
            return /=\s*"[^"]*`\s*\n\s*\+/.test(source);
        }).map((path) => path.slice(hubRoot.length + 1));
        assert.deepEqual(offenders, []);
    });
});
