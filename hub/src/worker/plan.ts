/** Authorization and execution of fixed plan routes. No model-selected paths. */
import { setTimeout as delay } from 'node:timers/promises';
import type { ToolRequest, ToolResponse } from '../protocol/tool-requests.ts';
import { dispatchToolRequest } from '../protocol/tool-requests.ts';
import { validMarkdown, type PlanStore } from '../state/plan.ts';
import type { SessionRegistry } from '../state/registry.ts';

export async function dispatchPlan(
    route: string, request: ToolRequest, registry: SessionRegistry,
    plans: PlanStore, token: string, holdMs: number, signal: AbortSignal,
    changed: (session: string) => void,
): Promise<ToolResponse> {
    const response = dispatchToolRequest(route, request);
    const reject = (code: string, message: string): ToolResponse => ({
        ...response, data: { ...response.data, error: { code, message } },
    });
    const session = registry.get(request.session_id);
    const deadline = Date.now() + holdMs;
    // Events and RPCs travel on independent sockets. Wait for admission/status
    // rather than rejecting a valid call whose run announcement is still queued.
    while (true) {
        if (signal.aborted) throw new Error('request closed');
        if (!session || registry.get(request.session_id) !== session || session.token !== token) {
            return reject('unauthorized', 'session is no longer authorized');
        }
        if (session.identity.state === 'live' && session.identity.workerId !== request.worker_id) {
            return reject('unauthorized', 'worker identity mismatch');
        }
        if (session.connected && session.identity.state === 'live'
            && session.identity.workerId === request.worker_id && session.activeRunId === request.run_id) break;
        if (Date.now() >= deadline) return reject('unauthorized', 'active worker run was not verified');
        await delay(Math.min(10, Math.max(1, deadline - Date.now())), undefined, { signal });
    }
    const args = request.arguments;
    if (Object.keys(args).some((key) => key !== 'operation' && key !== 'markdown')
        || args.operation !== (route === 'plan/read' ? 'read' : 'replace')
        || (route === 'plan/read' && Object.hasOwn(args, 'markdown'))
        || (route === 'plan/replace' && !validMarkdown(args.markdown))) {
        return reject('invalid_arguments', 'invalid plan operation or markdown (maximum 64 KiB)');
    }
    // No await between the authorization check, disk commit and publication.
    try {
        const result = route === 'plan/read'
            ? { plan: plans.read(session.id), changed: false }
            : plans.replace(session.id, args.markdown as string);
        if (result.changed) {
            // Observer failure cannot turn a committed replacement into failure.
            try { changed(session.id); } catch { /* publication is recoverable by resubscription */ }
        }
        return {
            type: 'tool_response',
            data: {
                worker_id: request.worker_id, session_id: request.session_id,
                run_id: request.run_id, request_id: request.request_id, route,
                status: 'succeeded',
                result: route === 'plan/read' ? { ...result.plan }
                    : { revision: result.plan.revision, updated_at: result.plan.updated_at, changed: result.changed },
            },
        };
    } catch {
        return reject('storage_error', 'plan storage failed; previous plan was preserved');
    }
}
