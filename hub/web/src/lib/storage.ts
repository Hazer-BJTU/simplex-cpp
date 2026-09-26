/**
 * @file browser storage, with the fallback that keeps the panel working.
 *
 * Two callers need this now — the token and the theme preference — and they
 * need the same thing: `localStorage` when the browser will actually let us
 * write to it, memory when it will not. A browser in private mode throws on
 * `localStorage` access rather than returning null, and a panel that crashed
 * there would be unusable in exactly the situation where someone is trying not
 * to leave state on disk.
 *
 * It lives in its own module rather than in `token.ts` so a second caller does
 * not have to import the token store to save a colour scheme.
 */

/**
 * The part of `Storage` this panel uses.
 *
 * Structural rather than `Storage` so a test can pass a plain object, and so
 * the memory fallback below is a legitimate implementation rather than a cast.
 */
export interface KeyValueStorage {
    getItem(key: string): string | null;
    setItem(key: string, value: string): void;
    removeItem(key: string): void;
}

/** A storage lookalike backed by a plain `Map`, for environments without one. */
export function memoryStorage(): KeyValueStorage {
    const map = new Map<string, string>();
    return {
        getItem: (key) => (map.has(key) ? map.get(key)! : null),
        setItem: (key, value) => { map.set(key, String(value)); },
        removeItem: (key) => { map.delete(key); },
    };
}

/**
 * `localStorage` when the browser exposes a usable one, memory otherwise.
 *
 * The probe write is the point: a `localStorage` that exists but throws on
 * `setItem` is common (private mode, blocked third-party storage) and would
 * otherwise fail on first use rather than here.
 */
export function safeStorage(storage?: KeyValueStorage): KeyValueStorage {
    if (storage) return storage;
    try {
        const candidate = globalThis.localStorage;
        if (candidate) {
            const probe = '__simplex_hub_probe__';
            candidate.setItem(probe, '1');
            candidate.removeItem(probe);
            return candidate;
        }
    } catch {
        // Private mode or a blocked origin: keep everything in memory only.
    }
    return memoryStorage();
}
