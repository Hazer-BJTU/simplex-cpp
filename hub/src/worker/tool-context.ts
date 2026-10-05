/** Shared live-run authorization for executable remote routes. */
import { setTimeout as delay } from 'node:timers/promises';
import type { Session, SessionRegistry } from '../state/registry.ts';
import type { ToolRequest, ToolResponse } from '../protocol/tool-requests.ts';
import { dispatchToolRequest } from '../protocol/tool-requests.ts';

export interface ToolContext {
    request: ToolRequest;
    route: string;
    caller: Session;
    signal: AbortSignal;
    /** Recheck after awaits and immediately before committing a side effect. */
    validate(): void;
}

export class ToolFailure extends Error {
    readonly code: string;
    readonly status = 400;
    constructor(code: string, message: string) {
        super(message);
        this.code = code;
    }
}

export function rejectedTool(route: string, request: ToolRequest, error: ToolFailure): ToolResponse {
    const base = dispatchToolRequest(route, request);
    return { ...base, data: { ...base.data, error: { code: error.code, message: error.message } } };
}

export function succeededTool(context: ToolContext, result: Record<string, unknown>): ToolResponse {
    const { request, route } = context;
    return {
        type: 'tool_response',
        data: {
            worker_id: request.worker_id, session_id: request.session_id,
            run_id: request.run_id, request_id: request.request_id, route,
            status: 'succeeded', result,
        },
    };
}

/** Independent event/RPC sockets may deliver an otherwise valid run out of order. */
export async function authorizeTool(
    route: string, request: ToolRequest, registry: SessionRegistry,
    token: string, holdMs: number, signal: AbortSignal,
): Promise<ToolContext> {
    const caller = registry.get(request.session_id);
    const validate = (): void => {
        if (signal.aborted) throw new ToolFailure('aborted', 'request closed before commit');
        if (!caller || registry.get(caller.id) !== caller || caller.token !== token || caller.closing
            || !caller.connected || caller.identity.state !== 'live'
            || caller.identity.workerId !== request.worker_id || caller.activeRunId !== request.run_id) {
            throw new ToolFailure('unauthorized', 'active worker run is no longer authorized');
        }
    };
    const deadline = Date.now() + holdMs;
    while (true) {
        if (signal.aborted) throw new ToolFailure('aborted', 'request closed before commit');
        if (!caller || registry.get(request.session_id) !== caller || caller.token !== token || caller.closing
            || (caller.identity.state === 'live' && caller.identity.workerId !== request.worker_id)) {
            throw new ToolFailure('unauthorized', 'session or worker identity is no longer authorized');
        }
        if (caller.connected && caller.identity.state === 'live'
            && caller.identity.workerId === request.worker_id && caller.activeRunId === request.run_id) {
            validate();
            return { request, route, caller, signal, validate };
        }
        if (Date.now() >= deadline) throw new ToolFailure('unauthorized', 'active worker run was not verified');
        await delay(Math.min(10, Math.max(1, deadline - Date.now())), undefined, { signal });
    }
}
