/** Execution of the fixed `ask_user/ask` route: validate, publish, wait, settle. */
import { authorizeTool, rejectedTool, ToolFailure } from './tool-context.ts';
import type { ToolRequest, ToolResponse } from '../protocol/tool-requests.ts';
import {
    QuestionConflictError, QuestionLimitError, validOptions, validQuestion,
} from '../state/questions.ts';
import type { QuestionResult, QuestionStore } from '../state/questions.ts';
import type { SessionRegistry } from '../state/registry.ts';

/**
 * One question waits for an operator answer, its deadline, or transport abort.
 * The Hub owns the wait; a timeout is a normal result, not a failure.
 */
export async function dispatchQuestion(
    route: string, request: ToolRequest, registry: SessionRegistry,
    questions: QuestionStore, token: string, holdMs: number, signal: AbortSignal,
    maxTimeoutMs: number, changed: (session: string) => void,
): Promise<ToolResponse> {
    let context;
    try {
        context = await authorizeTool(route, request, registry, token, holdMs, signal);
    } catch (error) {
        if (error instanceof ToolFailure) return rejectedTool(route, request, error);
        throw error;
    }
    const reject = (code: string, message: string): ToolResponse => rejectedTool(
        route, request, new ToolFailure(code, message));
    if (context.caller.kind === 'headless') {
        // No surface can answer a child worker's question; fail in milliseconds
        // instead of making the subagent wait out the whole budget.
        return reject('not_implemented', 'headless workers have no operator to answer questions');
    }

    const args = request.arguments;
    if (Object.keys(args).some(key => !['question', 'options', 'timeout_ms'].includes(key))
        || !validQuestion(args.question)) {
        return reject('invalid_arguments', 'question must be 1..8192 bytes of nonblank UTF-8');
    }
    if (args.options !== undefined && !validOptions(args.options)) {
        return reject('invalid_arguments',
            'options must contain 2..8 distinct shorter-than-200-byte strings, or be omitted');
    }
    const options = args.options === undefined ? [] : args.options;
    const requested = args.timeout_ms;
    if (!Number.isSafeInteger(requested) || (requested as number) < 1000) {
        return reject('invalid_arguments', 'timeout_ms must be an integer of at least 1000');
    }
    // The hub setting is a ceiling, not a contract: a longer request waits for
    // the cap instead of failing. The effective budget is reported back.
    const budget = Math.min(requested as number, maxTimeoutMs);

    const sessionId = context.caller.id;
    const askedAt = Date.now();
    let pending: Promise<QuestionResult>;
    try {
        pending = questions.open({
            session: sessionId,
            questionId: request.request_id,
            question: args.question,
            options: options as string[],
            timeoutMs: budget,
        });
    } catch (error) {
        if (error instanceof QuestionLimitError) return reject('too_many_questions', error.message);
        if (error instanceof QuestionConflictError) return reject('request_conflict', error.message);
        throw error;
    }
    // Operator-facing publication is recoverable: a failed broadcast must not
    // change the answer the worker receives.
    try { changed(sessionId); } catch { /* the panel can resubscribe */ }

    const withdraw = (): void => questions.withdraw(request.request_id, 'worker_disconnected');
    signal.addEventListener('abort', withdraw, { once: true });
    let result: QuestionResult;
    try {
        result = await pending;
    } finally {
        signal.removeEventListener('abort', withdraw);
    }
    try { changed(sessionId); } catch { /* settlement is already decided */ }

    if (signal.aborted) {
        // The socket is gone; the caller drops this response without sending it.
        return reject('aborted', 'request closed before the question settled');
    }
    // The run may have ended while waiting; an accepted answer is still valid.
    return {
        type: 'tool_response',
        data: {
            worker_id: request.worker_id,
            session_id: request.session_id,
            run_id: request.run_id,
            request_id: request.request_id,
            route,
            status: 'succeeded',
            result: {
                outcome: result.outcome,
                // The wait the hub actually granted, which a ceiling may lower.
                timeout_ms: budget,
                // Clamped: a wall-clock step must not make a wait look negative.
                elapsed_ms: Math.max(0, Date.now() - askedAt),
                ...(result.outcome === 'answered' ? { reply: result.reply } : {}),
                ...(result.outcome === 'cancelled' ? { reason: result.reason } : {}),
            },
        },
    };
}
