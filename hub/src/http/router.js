/**
 * @file minimal path router for the hub's HTTP API.
 *
 * Routes are registered as `METHOD /path/with/:params`. Matching is exact on
 * the number of segments; `:name` captures one segment. The hub has a small,
 * fixed API surface, so a table is easier to read than a routing framework —
 * and it keeps the package's runtime dependency count at one.
 */

/** Split a pathname into nonempty segments. */
function segments(pathname) {
    return pathname.split('/').filter((part) => part.length > 0);
}

/**
 * Percent-decode one captured path segment.
 *
 * `decodeURIComponent` throws on a malformed escape such as `%ZZ`. That is a
 * client error, so it is surfaced with a 400 status instead of reaching the
 * server's catch-all as an opaque 500 with the raw URIError text.
 */
function decodeParam(segment) {
    try {
        return decodeURIComponent(segment);
    } catch {
        const error = new Error('malformed percent-encoding in the request path');
        error.status = 400;
        throw error;
    }
}

/**
 * @returns {{
 *   add: (method: string, pattern: string, handler: Function) => void,
 *   find: (method: string, pathname: string) => ({handler: Function, params: object}|null),
 *   list: () => Array<{method: string, pattern: string}>,
 * }}
 */
export function createRouter() {
    const routes = [];

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
                const params = {};
                let matched = true;
                for (let index = 0; index < route.parts.length; index += 1) {
                    const expected = route.parts[index];
                    if (expected.startsWith(':')) {
                        params[expected.slice(1)] = decodeParam(parts[index]);
                    } else if (expected !== parts[index]) {
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
