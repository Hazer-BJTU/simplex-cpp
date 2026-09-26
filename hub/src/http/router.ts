/**
 * @file minimal path router for the hub's HTTP API.
 *
 * Routes are registered as `METHOD /path/with/:params`. Matching is exact on
 * the number of segments; `:name` captures one segment. The hub has a small,
 * fixed API surface, so a table is easier to read than a routing framework —
 * and it keeps the package's runtime dependency count at one.
 */
import type { IncomingMessage, ServerResponse } from 'node:http';

/**
 * An error carrying the HTTP status its caller should answer with.
 *
 * The HTTP layer reads `status` to decide between 400 and 500, so this is the
 * contract between the modules that detect a bad request and the one that
 * writes the response.
 */
export interface StatusError extends Error {
    status: number;
}

/** Read and parse a JSON request body, bounded by a byte limit. */
export type JsonBodyReader = (req: IncomingMessage, limit: number) => Promise<unknown>;

/**
 * What a registered route handler receives.
 *
 * `body` is the reader itself rather than a parsed body: a handler that wants
 * one calls it with its own limit, and a handler that only reads the query
 * string never pays for a parse. That has been the shape since the first
 * version, and it is spelled out here because it reads like a mistake.
 */
export interface RouteContext {
    req: IncomingMessage;
    res: ServerResponse;
    url: URL;
    params: Record<string, string>;
    body: JsonBodyReader;
}

/** A route handler: it writes its own response. */
export type RouteHandler = (context: RouteContext) => void | Promise<void>;

/** One registered route, as `list()` reports it. */
export interface RegisteredRoute {
    method: string;
    pattern: string;
}

/** A route match: the handler plus its captured parameters. */
export interface RouteMatch {
    handler: RouteHandler;
    params: Record<string, string>;
}

/** The router facade. */
export interface Router {
    add(method: string, pattern: string, handler: RouteHandler): void;
    find(method: string, pathname: string): RouteMatch | null;
    list(): RegisteredRoute[];
}

/** Split a pathname into nonempty segments. */
function segments(pathname: string): string[] {
    return pathname.split('/').filter((part) => part.length > 0);
}

/**
 * Percent-decode one captured path segment.
 *
 * `decodeURIComponent` throws on a malformed escape such as `%ZZ`. That is a
 * client error, so it is surfaced with a 400 status instead of reaching the
 * server's catch-all as an opaque 500 with the raw URIError text.
 */
function decodeParam(segment: string): string {
    try {
        return decodeURIComponent(segment);
    } catch {
        const error = new Error('malformed percent-encoding in the request path') as StatusError;
        error.status = 400;
        throw error;
    }
}

/** Create an empty router. */
export function createRouter(): Router {
    const routes: Array<RegisteredRoute & { parts: string[]; handler: RouteHandler }> = [];

    return {
        add(method, pattern, handler) {
            routes.push({
                method: method.toUpperCase(),
                pattern,
                parts: segments(pattern),
                handler,
            });
        },

        find(method, pathname) {
            const parts = segments(pathname);
            const wanted = method.toUpperCase();
            for (const route of routes) {
                if (route.method !== wanted || route.parts.length !== parts.length) continue;
                const params: Record<string, string> = {};
                let matched = true;
                for (let index = 0; index < route.parts.length; index += 1) {
                    // Both sides have the same length: the guard above compared
                    // the segment counts.
                    const expected = route.parts[index] as string;
                    const actual = parts[index] as string;
                    if (expected.startsWith(':')) {
                        params[expected.slice(1)] = decodeParam(actual);
                    } else if (expected !== actual) {
                        matched = false;
                        break;
                    }
                }
                if (matched) return { handler: route.handler, params };
            }
            return null;
        },

        list() {
            return routes.map(({ method, pattern }) => ({ method, pattern }));
        },
    };
}
