/**
 * @file where the panel keeps its bearer token.
 *
 * Three sources, in order of authority: the `?token=` query parameter, a
 * previous visit's `localStorage` entry, and memory. The URL wins because that
 * is how the hub hands a token to a browser it just opened, and it is consumed
 * — written to storage and stripped from the address bar — so the token does
 * not survive in history, in a screenshot, or in a copied link.
 *
 * Ported from `web/js/api.js`, where the storage fallback was already correct:
 * a browser in private mode throws on `localStorage` access rather than
 * returning null, and a panel that crashed there would be unusable in exactly
 * the situation where someone is trying not to leave a token on disk.
 *
 * That fallback now lives in `./storage.ts`, because the theme preference
 * needed exactly the same thing. It is re-exported here so the modules that
 * have always imported it from this one do not have to care.
 */
import { safeStorage, type KeyValueStorage } from './storage.ts';

export { memoryStorage, safeStorage } from './storage.ts';
export type { KeyValueStorage } from './storage.ts';

/** localStorage key holding an accepted panel token. */
export const TOKEN_KEY = 'simplex-hub-token';

/** The parts of a `Location` this panel reads. */
export interface LocationLike {
    readonly search: string;
    readonly href: string;
    /** Absent from a hand-made test double; the socket defaults it. */
    readonly host?: string | undefined;
    readonly protocol?: string | undefined;
}

/** The part of `History` this module calls. */
export interface HistoryLike {
    replaceState(data: unknown, unused: string, url?: string | null): void;
}

/** Token present in a page URL (`?token=...`), or `''` when absent. */
export function tokenFromUrl(location: LocationLike | null | undefined): string {
    try {
        const value = location?.search
            ? new URLSearchParams(location.search).get('token')
            : null;
        return typeof value === 'string' ? value : '';
    } catch {
        return '';
    }
}

/** Rewrite the address bar without the token query parameter. */
export function stripTokenFromUrl(
    location: LocationLike | null | undefined,
    history: HistoryLike | null | undefined,
): void {
    try {
        if (!location || !history) return;
        const url = new URL(location.href);
        if (!url.searchParams.has('token')) return;
        url.searchParams.delete('token');
        history.replaceState(null, '', `${url.pathname}${url.search}${url.hash}`);
    } catch {
        // A failed rewrite only leaves the token in the address bar; never fatal.
    }
}

/** The current token, and the two ways it changes. */
export interface TokenStore {
    /** The token to present, or `''`. Reading it is the only way to get it. */
    get(): string;
    has(): boolean;
    set(value: unknown): void;
    clear(): void;
}

/** Where a token came from, which decides how the UI talks about it. */
export type TokenOrigin = 'none' | 'url' | 'stored';

/** Everything `createTokenStore` reads. */
export interface TokenStoreOptions {
    storage?: KeyValueStorage;
    location?: LocationLike | null;
    history?: HistoryLike | null;
}

/**
 * Create the token holder.
 *
 * A URL token is persisted and removed from the address bar; the value is never
 * rendered anywhere, and this module has no accessor that would let it be.
 */
export function createTokenStore(options: TokenStoreOptions = {}): TokenStore & {
    origin(): TokenOrigin;
} {
    const store = safeStorage(options.storage);
    const loc = options.location ?? (globalThis.location as LocationLike | undefined) ?? null;
    const hist = options.history ?? (globalThis.history as HistoryLike | undefined) ?? null;
    let token = '';
    let origin: TokenOrigin = 'none';
    try {
        token = store.getItem(TOKEN_KEY) ?? '';
        if (token) origin = 'stored';
    } catch {
        token = '';
    }
    const fromUrl = tokenFromUrl(loc);
    if (fromUrl) {
        token = fromUrl;
        origin = 'url';
        try {
            store.setItem(TOKEN_KEY, token);
        } catch { /* memory-only fallback */ }
        stripTokenFromUrl(loc, hist);
    }
    return {
        get: () => token,
        has: () => token.length > 0,
        origin: () => origin,
        set(value) {
            token = typeof value === 'string' ? value : '';
            origin = token ? 'url' : 'none';
            try {
                if (token) store.setItem(TOKEN_KEY, token);
                else store.removeItem(TOKEN_KEY);
            } catch { /* memory-only fallback */ }
        },
        clear() {
            this.set('');
        },
    };
}
