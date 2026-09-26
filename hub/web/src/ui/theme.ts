/**
 * @file the panel's theme.
 *
 * Three choices — `light`, `dark`, `system` — resolved to two token sets. The
 * distinction matters: "follow the operating system" is a different preference
 * from "be light", and collapsing them is what makes a theme toggle feel like
 * it is fighting the machine. The preference is persisted; the resolution is
 * derived from it and from `prefers-color-scheme`, and never persisted.
 *
 * The controller is deliberately free of React, of `document`, and of
 * `localStorage`: everything it touches arrives through the options object, so
 * `node --test` can drive every branch. `panelTheme()` at the bottom is the one
 * function that reaches for real browser globals, and nothing imports it except
 * the components that need it.
 *
 * The token sets themselves are in `styles/app.css`, keyed off the
 * `data-theme` attribute this writes. That attribute is also set by a small
 * inline script in `index.html` — before the bundle loads, so a dark preference
 * does not flash white on every reload. The two agree on the attribute name and
 * the storage key because a test asserts they do (`panel-theme.test.js`).
 */
import { safeStorage, type KeyValueStorage } from '../lib/storage.ts';

/** What the operator chose. `system` defers to the operating system. */
export type ThemePreference = 'light' | 'dark' | 'system';

/** What that choice currently amounts to. */
export type ResolvedTheme = 'light' | 'dark';

/** localStorage key holding the preference. Mirrored by the boot script. */
export const THEME_KEY = 'simplex.panel.theme';

/** The attribute the token sets are keyed off. Mirrored by the boot script. */
export const THEME_ATTRIBUTE = 'data-theme';

/** The attribute recording the *preference*, for the CSS that shows it. */
export const THEME_PREFERENCE_ATTRIBUTE = 'data-theme-preference';

/** Narrow an unknown value — a storage read, a URL parameter — to a preference. */
export function isThemePreference(value: unknown): value is ThemePreference {
    return value === 'light' || value === 'dark' || value === 'system';
}

/**
 * The preference, or `system` when there is nothing usable stored.
 *
 * Anything unrecognised is treated as absent rather than as an error: a
 * preference written by a future version should make the panel follow the
 * operating system, not refuse to draw.
 */
export function readThemePreference(storage: KeyValueStorage | null): ThemePreference {
    if (!storage) return 'system';
    try {
        const raw = storage.getItem(THEME_KEY);
        return isThemePreference(raw) ? raw : 'system';
    } catch {
        return 'system';
    }
}

/** What a preference amounts to, given what the operating system reports. */
export function resolveTheme(
    preference: ThemePreference,
    systemPrefersDark: boolean,
): ResolvedTheme {
    if (preference === 'system') return systemPrefersDark ? 'dark' : 'light';
    return preference;
}

/** The next preference in the cycle the toggle walks. */
export function nextThemePreference(current: ThemePreference): ThemePreference {
    switch (current) {
        case 'light': return 'dark';
        case 'dark': return 'system';
        default: return 'light';
    }
}

/** How each preference is labelled. The label says which one is in effect. */
export function describeTheme(
    preference: ThemePreference,
    resolved: ResolvedTheme,
): { label: string; detail: string } {
    if (preference === 'system') {
        return {
            label: `system (${resolved})`,
            detail: `following the operating system, which asks for ${resolved}`,
        };
    }
    return { label: preference, detail: `always ${preference}` };
}

/** The part of an element this module writes to. */
export interface ThemeTarget {
    setAttribute(name: string, value: string): void;
}

/** Everything `createThemeController` reads, so a test can supply all of it. */
export interface ThemeOptions {
    target: ThemeTarget;
    storage?: KeyValueStorage | null;
    /** What the operating system asks for right now. */
    systemPrefersDark?: () => boolean;
    /**
     * Subscribe to operating-system changes. Returns an unsubscribe function.
     *
     * Absent means "this environment cannot report changes", which is honest
     * rather than wrong: the preference still resolves correctly on every read,
     * it just does not re-render when the OS flips.
     */
    watchSystem?: (onChange: (dark: boolean) => void) => () => void;
    /** The preference to start from, when storage has nothing to say. */
    initial?: ThemePreference;
}

/** The theme, and the three ways to change it. */
export interface ThemeController {
    preference(): ThemePreference;
    resolved(): ResolvedTheme;
    /** Set and persist a preference; returns what it resolved to. */
    set(preference: ThemePreference): ResolvedTheme;
    /** Advance to the next preference and return it. */
    cycle(): ThemePreference;
    /** Re-read the operating system and re-apply. */
    refresh(): ResolvedTheme;
    subscribe(listener: () => void): () => void;
    /** Stop following the operating system. Idempotent. */
    dispose(): void;
}

/**
 * Create the controller and apply the starting preference immediately.
 *
 * Applying on creation rather than on first render is what keeps the attribute
 * and the React tree from disagreeing for a frame.
 */
export function createThemeController(options: ThemeOptions): ThemeController {
    const { target } = options;
    const storage = options.storage ?? null;
    const systemPrefersDark = options.systemPrefersDark ?? (() => false);
    const listeners = new Set<() => void>();
    let preference: ThemePreference = options.initial ?? readThemePreference(storage);
    let resolved: ResolvedTheme = resolveTheme(preference, systemPrefersDark());

    function notify(): void {
        for (const listener of listeners) listener();
    }

    function write(): void {
        target.setAttribute(THEME_ATTRIBUTE, resolved);
        target.setAttribute(THEME_PREFERENCE_ATTRIBUTE, preference);
    }

    write();

    // Following the operating system is only meaningful while the preference is
    // `system`; the guard is inside the callback so the subscription itself does
    // not have to be torn down and rebuilt on every change of mind.
    const unwatch = options.watchSystem?.((dark) => {
        if (preference !== 'system') return;
        const next = resolveTheme(preference, dark);
        if (next === resolved) return;
        resolved = next;
        write();
        notify();
    }) ?? null;

    return {
        preference: () => preference,
        resolved: () => resolved,
        refresh() {
            const next = resolveTheme(preference, systemPrefersDark());
            if (next !== resolved) {
                resolved = next;
                write();
                notify();
            }
            return resolved;
        },
        set(next) {
            preference = next;
            try {
                if (storage) storage.setItem(THEME_KEY, next);
            } catch {
                // A storage that refuses the write costs persistence, not the theme.
            }
            resolved = resolveTheme(preference, systemPrefersDark());
            write();
            notify();
            return resolved;
        },
        cycle() {
            const next = nextThemePreference(preference);
            this.set(next);
            return next;
        },
        subscribe(listener) {
            listeners.add(listener);
            return () => { listeners.delete(listener); };
        },
        dispose() {
            unwatch?.();
        },
    };
}

/**
 * The panel's own controller.
 *
 * Created on first use rather than at import time, so importing this module in
 * a Node test — where there is no `document` — costs nothing.
 */
let singleton: ThemeController | null = null;

export function panelTheme(): ThemeController {
    if (singleton) return singleton;
    singleton = createThemeController({
        target: document.documentElement,
        storage: safeStorage(),
        systemPrefersDark: () => globalThis.matchMedia?.('(prefers-color-scheme: dark)').matches
            ?? false,
        watchSystem: (onChange) => {
            const query = globalThis.matchMedia?.('(prefers-color-scheme: dark)');
            if (!query) return () => {};
            const handler = (event: MediaQueryListEvent): void => onChange(event.matches);
            query.addEventListener('change', handler);
            return () => query.removeEventListener('change', handler);
        },
    });
    return singleton;
}
