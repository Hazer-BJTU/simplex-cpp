/**
 * @file token comparison and request authentication helpers.
 *
 * The worker-facing payload channel is an approval authority
 * (core/docs/worker-protocol.md): whoever may send a payload may select
 * `confirmation.mode: approve`. Correlation IDs are not credentials, so the hub
 * binds every worker connection to a deployment-authorized session with a
 * per-session token carried in the upgrade query string — the only place the
 * worker's configuration can put a credential, because the protocol exposes no
 * authentication headers, cookies, or subprotocols.
 */
import { timingSafeEqual } from 'node:crypto';
import type { IncomingMessage } from 'node:http';

/** Constant-time string comparison that tolerates different lengths. */
export function safeEqual(left: unknown, right: unknown): boolean {
    const a = Buffer.from(String(left ?? ''), 'utf8');
    const b = Buffer.from(String(right ?? ''), 'utf8');
    if (a.length !== b.length) {
        // Compare against itself to keep the timing profile independent of how
        // much of the secret matched, then report failure.
        timingSafeEqual(a, a);
        return false;
    }
    return timingSafeEqual(a, b);
}

/** Token presented by a worker or panel upgrade/request, or '' when absent. */
export function presentedToken(url: URL | null | undefined): string {
    const fromQuery = url?.searchParams?.get('token');
    return typeof fromQuery === 'string' ? fromQuery : '';
}

/** Bearer token from an Authorization header, or '' when absent. */
export function bearerToken(req: IncomingMessage | null | undefined): string {
    const header = req?.headers?.authorization;
    if (typeof header !== 'string') return '';
    const match = /^Bearer\s+(.+)$/i.exec(header.trim());
    return match ? (match[1] as string) : '';
}

/** Read one cookie value from a request, or '' when absent. */
export function cookieValue(req: IncomingMessage | null | undefined, name: string): string {
    const header = req?.headers?.cookie;
    if (typeof header !== 'string') return '';
    for (const part of header.split(';')) {
        const separator = part.indexOf('=');
        if (separator === -1) continue;
        if (part.slice(0, separator).trim() === name) {
            try {
                return decodeURIComponent(part.slice(separator + 1).trim());
            } catch {
                // A malformed cookie is an unusable credential, not a server
                // error. Other credentials on the request may still be valid.
                return '';
            }
        }
    }
    return '';
}

/** The slice of hub configuration this module reads. */
export interface PanelAuthConfig {
    panel?: { token?: string };
}

/** The verdict on a panel request. */
export type PanelAuth = { ok: true } | { ok: false; reason: string };

/**
 * Decide whether a panel request may proceed.
 *
 * Authentication is disabled when no token is configured, which configuration
 * validation only permits for a loopback listener.
 */
export function authorizePanel(
    config: PanelAuthConfig,
    req: IncomingMessage | null | undefined,
    url: URL | null | undefined,
): PanelAuth {
    const expected = config.panel?.token ?? '';
    if (expected.length === 0) return { ok: true };
    const candidates = [
        presentedToken(url),
        bearerToken(req),
        cookieValue(req, 'simplex_hub_token'),
    ];
    if (candidates.some((value) => value.length > 0 && safeEqual(value, expected))) {
        return { ok: true };
    }
    return { ok: false, reason: 'missing or invalid panel token' };
}
