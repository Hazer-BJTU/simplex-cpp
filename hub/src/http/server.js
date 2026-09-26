/**
 * @file HTTP + WebSocket front door for the hub.
 *
 * One `node:http` server carries three things on one port:
 *   - the static web panel (`hub/web`),
 *   - the JSON API under `/api`,
 *   - WebSocket upgrades, dispatched to registered handlers.
 *
 * Upgrade routing is explicit rather than path-prefix based: the worker-facing
 * adapter (src/worker/connection.js, src/worker/confirmation.js) and the panel
 * API (src/panel/api.js) each register a matcher, so the set of accepted
 * upgrade targets is visible in one place per role.
 */
import { createServer } from 'node:http';
import { join } from 'node:path';
import { createRouter } from './router.js';
import { serveStatic } from './static.js';

/** Write a JSON response. */
export function sendJson(res, status, body, headers = {}) {
    const payload = Buffer.from(`${JSON.stringify(body)}\n`, 'utf8');
    res.writeHead(status, {
        'Content-Type': 'application/json; charset=utf-8',
        'Content-Length': payload.length,
        'Cache-Control': 'no-store',
        ...headers,
    });
    res.end(payload);
}

/** Write a JSON error response with a stable machine-readable code. */
export function sendError(res, status, code, message, details) {
    sendJson(res, status, { error: code, message, ...(details ? { details } : {}) });
}

/** Read and parse a JSON request body, bounded by `limit` bytes. */
export async function readJsonBody(req, limit) {
    const chunks = [];
    let size = 0;
    for await (const chunk of req) {
        size += chunk.length;
        if (size > limit) {
            const error = new Error(`request body exceeds ${limit} bytes`);
            error.status = 413;
            throw error;
        }
        chunks.push(chunk);
    }
    if (size === 0) return {};
    try {
        return JSON.parse(Buffer.concat(chunks).toString('utf8'));
    } catch (cause) {
        const error = new Error(`invalid JSON body: ${cause.message}`);
        error.status = 400;
        throw error;
    }
}

/**
 * Parse a request target against the authority the client presented.
 *
 * `req.headers.host` is client-controlled and `new URL` throws on a malformed
 * authority, so this never lets a bad `Host` header escape as an exception —
 * the caller answers 400 instead. That matters because the request handler runs
 * before any authentication, and an unhandled rejection ends the process.
 *
 * An origin-form target with no `Host` header at all is still parsed, against a
 * fixed authority, since the hub's routing never consults the host.
 *
 * @returns {URL|null} null when the target cannot be parsed.
 */
export function requestUrl(req) {
    const target = typeof req.url === 'string' && req.url.length > 0 ? req.url : '/';
    const host = req.headers?.host;
    if (typeof host === 'string' && host.length > 0) {
        try {
            return new URL(target, `http://${host}`);
        } catch {
            // A malformed authority is a client error, not a crash.
            return null;
        }
    }
    try {
        return new URL(target, 'http://localhost');
    } catch {
        return null;
    }
}

/**
 * Create the hub's HTTP front door. Call `listen()` to bind it.
 *
 * @param {object} options
 * @param {object} options.config validated hub configuration.
 * @param {object} options.log hub logger.
 * @param {string} options.hubRoot absolute `hub/` directory.
 * @returns {object} server facade used by the rest of the hub.
 */
export function createHttpServer({ config, log, hubRoot }) {
    const router = createRouter();
    const upgradeHandlers = [];
    const staticRoot = join(hubRoot, 'web');

    const server = createServer((req, res) => {
        // Never `void` a request handler: a rejection that escapes it would end
        // the hub, and this callback runs before authentication.
        handleRequest(req, res).catch((error) => {
            log.error('request failed', error);
            if (!res.headersSent) sendError(res, 500, 'internal_error', 'request failed');
            else res.destroy();
        });
    });

    async function handleRequest(req, res) {
        const url = requestUrl(req);
        if (!url) {
            sendError(res, 400, 'bad_request', 'malformed request target');
            return;
        }
        try {
            if (url.pathname === '/api' || url.pathname.startsWith('/api/')) {
                const found = router.find(req.method ?? 'GET', url.pathname);
                if (!found) {
                    sendError(res, 404, 'not_found', `no route for ${req.method} ${url.pathname}`);
                    return;
                }
                await found.handler({ req, res, url, params: found.params, body: readJsonBody });
                return;
            }
            if (req.method !== 'GET' && req.method !== 'HEAD') {
                sendError(res, 405, 'method_not_allowed', `${req.method} is not supported here`);
                return;
            }
            await serveStatic({ root: staticRoot, pathname: url.pathname, res, log: log.debug });
        } catch (error) {
            const status = Number.isInteger(error?.status) ? error.status : 500;
            if (status >= 500) log.error('request failed', error);
            if (!res.headersSent) {
                sendError(res, status, status >= 500 ? 'internal_error' : 'bad_request', error.message);
            } else {
                res.destroy();
            }
        }
    }

    server.on('upgrade', (req, socket, head) => {
        const url = requestUrl(req);
        if (!url) {
            log.debug('rejected upgrade with a malformed request target');
            socket.write('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
            socket.destroy();
            return;
        }
        for (const handler of upgradeHandlers) {
            let params = null;
            try {
                params = handler.match(req, url);
            } catch (error) {
                log.error('upgrade matcher failed', error);
            }
            if (!params) continue;
            // This listener is synchronous, so a throwing handler would become
            // an uncaughtException rather than a rejected promise. Both shapes
            // are contained here.
            try {
                const outcome = handler.handle({ req, socket, head, url, params });
                if (outcome && typeof outcome.catch === 'function') {
                    outcome.catch((error) => {
                        log.error('upgrade handler failed', error);
                        socket.destroy();
                    });
                }
            } catch (error) {
                log.error('upgrade handler failed', error);
                socket.destroy();
            }
            return;
        }
        log.debug(`rejected upgrade for ${url.pathname}`);
        socket.write('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
        socket.destroy();
    });

    // Client disconnect noise is expected (browsers and workers reconnect);
    // keep it at debug level instead of letting Node print stack traces.
    server.on('clientError', (error, socket) => {
        log.debug(`client error: ${error.message}`);
        if (socket.writable) socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
        else socket.destroy();
    });

    return {
        server,
        router,
        log,
        config,
        hubRoot,
        /** Register a JSON API route. */
        route: (method, pattern, handler) => router.add(method, pattern, handler),
        /** Register a WebSocket upgrade matcher/handler pair. */
        useUpgrade: (handler) => upgradeHandlers.push(handler),
        /**
         * Bind the listener.
         * @returns {Promise<{host: string, port: number, url: string}>}
         */
        listen: () => new Promise((resolve, reject) => {
            server.once('error', reject);
            server.listen(config.listen.port, config.listen.host, () => {
                server.removeListener('error', reject);
                const address = server.address();
                const host = address.family === 'IPv6' ? `[${address.address}]` : address.address;
                resolve({
                    host: address.address,
                    port: address.port,
                    url: `http://${host}:${address.port}`,
                });
            });
        }),
        /** Stop accepting connections and wait for open sockets to drain. */
        close: () => new Promise((resolve) => {
            server.close(() => resolve());
            server.closeAllConnections?.();
        }),
    };
}
