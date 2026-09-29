/**
 * One-shot worker tool RPC envelopes. Routing belongs to the URL, not arguments.
 * No remote operation is implemented yet; this module defines the validation
 * and dispatch boundary without exposing a handler registration API prematurely.
 */

/** Identifiers are opaque, nonempty strings echoed verbatim by the response. */
export interface ToolRequest {
    worker_id: string;
    session_id: string;
    run_id: string;
    request_id: string;
    arguments: Record<string, unknown>;
}

/** A rejected request has no result and reports a stable machine-readable code. */
export interface ToolResponse {
    type: 'tool_response';
    data: {
        worker_id: string;
        session_id: string;
        run_id: string;
        request_id: string;
        route: string;
        status: 'rejected';
        error: { code: 'not_implemented'; message: string };
    };
}

/** Narrow an untrusted decoded frame without interpreting operation arguments. */
export function parseToolRequest(document: unknown, sessionId: string): ToolRequest | null {
    if (!isObject(document) || document.type !== 'tool_request' || !isObject(document.data)) {
        return null;
    }
    const data = document.data;
    for (const key of ['worker_id', 'session_id', 'run_id', 'request_id']) {
        if (typeof data[key] !== 'string' || data[key].trim().length === 0) return null;
    }
    if (data.session_id !== sessionId || !isObject(data.arguments)) return null;
    return {
        worker_id: data.worker_id as string,
        session_id: data.session_id as string,
        run_id: data.run_id as string,
        request_id: data.request_id as string,
        arguments: data.arguments,
    };
}

function isObject(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * Dispatch boundary for future, explicitly registered remote operations.
 * Currently rejects every route, with no filesystem/process/model side effects.
 * A future implementation must authorize the live worker and operation before
 * invoking a handler; a session token alone is not a grant to execute tools.
 */
export function dispatchToolRequest(route: string, request: ToolRequest): ToolResponse {
    return {
        type: 'tool_response',
        data: {
            worker_id: request.worker_id,
            session_id: request.session_id,
            run_id: request.run_id,
            request_id: request.request_id,
            route,
            status: 'rejected',
            error: { code: 'not_implemented', message: 'remote tool route is not implemented' },
        },
    };
}
