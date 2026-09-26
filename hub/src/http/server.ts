/**
 * @file HTTP + WebSocket front door for the hub.
 *
 * One `node:http` server carries three things on one port:
 *   - the static web panel (`hub/web`),
 *   - the JSON API under `/api`,
 *   - WebSocket upgrades, dispatched to registered handlers.
 *
 * Upgrade routing is explicit rather than path-prefix based: the worker-facing
 * adapter (src/worker/connection.ts, src/worker/confirmation.ts) and the panel
 * API (src/panel/api.ts) each register a matcher, so the set of accepted
 * upgrade targets is visible in one place per role.
 */
import { createServer } from 'node:http';
import type { IncomingMessage, Server, ServerResponse } from 'node:http';
import type { Duplex } from 'node:stream';
import { join } from 'node:path';
import { createRouter } from './router.ts';
import type { RouteHandler, Router, RouteMatch, StatusError } from './router.ts';
import { serveStatic } from './static.ts';
import type { HubConfig } from '../config.ts';
import type { Logger } from '../log.ts';

/** Write a JSON response. */
export function sendJson(
    res: ServerResponse,
    status: number,
    body: unknown,
    headers: Record<string, string> = {},
): void {
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
export function sendError(
    res: ServerResponse,
    status: number,
    code: string,
    message: string,
    details?: unknown,
): void {
    sendJson(res, status, { error: code, message, ...(details ? { details } : {}) });
}

/** Read and parse a JSON request body, bounded by `limit` bytes. */
export async function readJsonBody(req: IncomingMessage, limit: number): Promise<unknown> {
    const chunks: Buffer[] = [];
    let size = 0;
    for await (const chunk of req) {
        const buffer = chunk as Buffer;
        size += buffer.length;
        if (size > limit) {
            const error = new Error(`request body exceeds ${limit} bytes`) as StatusError;
            error.status = 413;
            throw error;
        }
        chunks.push(buffer);
    }
    if (size === 0) return {};
    try {
        return JSON.parse(Buffer.concat(chunks).toString('utf8'));
    } catch (cause) {
        const message = cause instanceof Error ? cause.message : String(cause);
        const error = new Error(`invalid JSON body: ${message}`) as StatusError;
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
 * @returns null when the target cannot be parsed.
 */
export function requestUrl(req: IncomingMessage): URL | null {
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

/** What an upgrade handler is given once its matcher has claimed the request. */
export interface UpgradeContext {
    req: IncomingMessage;
    socket: Duplex;
    head: Buffer;
    url: URL;
    params: Record<string, unknown>;
}

/**
 * A WebSocket route: a matcher plus the handler it dispatches to.
 *
 * `match` returns null to decline, and any non-null value becomes `params`.
 * `handle` may be synchronous or asynchronous; the server contains a throw and
 * a rejection alike, because this runs from a synchronous event listener where
 * either shape would otherwise be fatal.
 */
export interface UpgradeHandler {
    match(req: IncomingMessage, url: URL): Record<string, unknown> | null;
    handle(context: UpgradeContext): void | Promise<void>;
    close?(): void;
}

/** The bound listener address. */
export interface BoundAddress {
    host: string;
    port: number;
    url: string;
}

/** The server facade used by the rest of the hub. */
export interface HubHttpServer {
    server: Server;
    router: Router;
    log: Logger;
    config: HubConfig;
    hubRoot: string;
    /** Register a JSON API route. */
    route(method: string, pattern: string, handler: RouteHandler): void;
    /** Register a WebSocket upgrade matcher/handler pair. */
    useUpgrade(handler: UpgradeHandler): void;
    /** Bind the listener. */
    listen(): Promise<BoundAddress>;
    /** Stop accepting connections and wait for open sockets to drain. */
    close(): Promise<void>;
}

/** Everything `createHttpServer` needs. */
export interface HttpServerOptions {
    config: HubConfig;
    log: Logger;
    /** Absolute `hub/` directory. */
    hubRoot: string;
}

/** Create the hub's HTTP front door. Call `listen()` to bind it. */
export function createHttpServer({ config, log, hubRoot }: HttpServerOptions): HubHttpServer {
    const router = createRouter();
    const upgradeHandlers: UpgradeHandler[] = [];
    const staticRoot = join(hubRoot, 'web', 'dist');

    const server = createServer((req, res) => {
        // Never `void` a request handler: a rejection that escapes it would end
        // the hub, and this callback runs before authentication.
        handleRequest(req, res).catch((error: Error) => {
            log.error('request failed', error);
            if (!res.headersSent) sendError(res, 500, 'internal_error', 'request failed');
            else res.destroy();
        });
    });

    async function handleRequest(req: IncomingMessage, res: ServerResponse): Promise<void> {
        const url = requestUrl(req);
        if (!url) {
            sendError(res, 400, 'bad_request', 'malformed request target');
            return;
        }
        try {
            if (url.pathname === '/api' || url.pathname.startsWith('/api/')) {
                const found: RouteMatch | null = router.find(req.method ?? 'GET', url.pathname);
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
            const failure = error as StatusError;
            const status = Number.isInteger(failure?.status) ? failure.status : 500;
            if (status >= 500) log.error('request failed', error as Error);
            if (!res.headersSent) {
                sendError(res, status, status >= 500 ? 'internal_error' : 'bad_request',
                    (error as Error).message);
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
            let params: Record<string, unknown> | null = null;
            try {
                params = handler.match(req, url);
            } catch (error) {
                log.error('upgrade matcher failed', error as Error);
            }
            if (!params) continue;
            // This listener is synchronous, so a throwing handler would become
            // an uncaughtException rather than a rejected promise. Both shapes
            // are contained here.
            try {
                const outcome = handler.handle({ req, socket, head, url, params });
                if (outcome && typeof (outcome as Promise<void>).catch === 'function') {
                    (outcome as Promise<void>).catch((error: Error) => {
                        log.error('upgrade handler failed', error);
                        socket.destroy();
                    });
                }
            } catch (error) {
                log.error('upgrade handler failed', error as Error);
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
        route: (method, pattern, handler) => router.add(method, pattern, handler),
        useUpgrade: (handler) => { upgradeHandlers.push(handler); },
        listen: () => new Promise<BoundAddress>((resolve, reject) => {
            server.once('error', reject);
            server.listen(config.listen.port, config.listen.host, () => {
                server.removeListener('error', reject);
                const address = server.address();
                if (address === null || typeof address === 'string') {
                    // Unreachable for a listening TCP server; rejecting rather
                    // than asserting keeps a surprise from becoming a crash.
                    reject(new Error('the listener has no bound address'));
                    return;
                }
                const host = address.family === 'IPv6' ? `[${address.address}]` : address.address;
                resolve({
                    host: address.address,
                    port: address.port,
                    url: `http://${host}:${address.port}`,
                });
            });
        }),
        close: () => new Promise<void>((resolve) => {
            server.close(() => resolve());
            server.closeAllConnections?.();
        }),
    };
}
