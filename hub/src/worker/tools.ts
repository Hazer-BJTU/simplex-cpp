/**
 * Dedicated, authenticated, one-request/one-response worker tool transport.
 * It shares session credentials with confirmation but owns no confirmation UI,
 * event subscription, persistent queue, replay, or automatic retry mechanism.
 */
import { WebSocketServer } from 'ws';
import type { RawData, WebSocket } from 'ws';
import type { HubConfig } from '../config.ts';
import type { Logger } from '../log.ts';
import { presentedToken, safeEqual } from '../http/auth.ts';
import type { UpgradeHandler } from '../http/server.ts';
import { dispatchToolRequest, parseToolRequest } from '../protocol/tool-requests.ts';
import type { SessionRegistry } from '../state/registry.ts';
import { isValidSessionId } from '../state/session-id.ts';

/** Lowercase literal path segments; no decoding, wildcards, or argument routing. */
const TOOL_ROUTE = /^\/agent\/([^/]+)\/tools\/([a-z][a-z0-9_-]*(?:\/[a-z][a-z0-9_-]*)*)$/;

export interface ToolRouteOptions {
    registry: SessionRegistry;
    config: HubConfig;
    log: Logger;
}

/** Bind this adapter only to the dedicated tool listener. */
export function createWorkerToolRoute({ registry, config, log }: ToolRouteOptions): UpgradeHandler & {
    close(): void;
} {
    const wss = new WebSocketServer({
        noServer: true,
        maxPayload: config.limits.maxMessageBytes,
        perMessageDeflate: false,
    });
    const open = new Set<WebSocket>();
    let closing = false;

    /** Every accepted socket has a hard deadline, including its closing handshake. */
    function accept(ws: WebSocket, sessionId: string, route: string): void {
        open.add(ws);
        let received = false;
        const timer = setTimeout(() => ws.terminate(), config.toolRequests.timeoutMs);
        timer.unref();
        ws.once('close', () => {
            clearTimeout(timer);
            open.delete(ws);
        });
        ws.on('error', () => {
            // Never log raw requests or query strings: both can contain secrets.
            log.debug(`tool request transport error for session ${sessionId}`);
            ws.terminate();
        });
        ws.on('message', (data: RawData, binary: boolean) => {
            if (received) {
                ws.close(1008, 'only one tool request is allowed');
                return;
            }
            received = true;
            if (binary) {
                ws.close(1003, 'tool requests must be text');
                return;
            }
            let document: unknown;
            try {
                document = JSON.parse(data.toString());
            } catch {
                ws.close(1008, 'invalid tool request JSON');
                return;
            }
            const request = parseToolRequest(document, sessionId);
            if (!request) {
                ws.close(1008, 'invalid tool request envelope');
                return;
            }
            const response = dispatchToolRequest(route, request);
            ws.send(JSON.stringify(response), (error) => {
                if (error) ws.terminate();
                else ws.close(1000, 'tool request completed');
            });
        });
    }

    return {
        match(req, url) {
            if (req.method !== 'GET') return null;
            const match = TOOL_ROUTE.exec(url.pathname);
            if (!match) return null;
            return { session: match[1], route: match[2] };
        },
        handle({ req, socket, head, url, params }) {
            const sessionId = params.session;
            const session = isValidSessionId(sessionId) ? registry.get(sessionId) : null;
            let rejection: string | null = null;
            if (!session) rejection = '404 Not Found';
            else if (!safeEqual(presentedToken(url), session.token)) rejection = '401 Unauthorized';
            else if (closing || open.size >= config.toolRequests.maxConnections) {
                rejection = '503 Service Unavailable';
            }
            if (rejection) {
                socket.end(`HTTP/1.1 ${rejection}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
                return;
            }
            wss.handleUpgrade(req, socket, head, (ws) => {
                accept(ws, sessionId as string, params.route as string);
            });
        },
        close() {
            closing = true;
            // A lifetime fence must not wait for a peer's closing handshake.
            for (const ws of open) ws.terminate();
            wss.close();
        },
    };
}
