import { createStore } from 'zustand/vanilla';
import type { ConfirmationPrompt, ErrorMessage, SessionDescription } from '../../../shared/protocol.ts';
import type { PanelStoreApi } from '../state/store.ts';

/** Include worker/run identity and prompt creation, never just a reusable ID. */
export function confirmationKey(prompt: ConfirmationPrompt): string {
    return JSON.stringify([prompt.session_id, prompt.worker_id, prompt.run_id,
        prompt.confirmation_id, prompt.received_at]);
}

export interface DecisionState {
    readonly decision: 'approved' | 'denied';
    readonly phase: 'pending' | 'checking' | 'failed';
    readonly error: string;
    /** An unknown outcome must be reconciled before a conflicting retry. */
    readonly retryable: boolean;
}

interface Attempt {
    prompt: ConfirmationPrompt;
    requestId: string;
    timer?: ReturnType<typeof setTimeout>;
    abort?: AbortController;
    state: DecisionState;
}

/**
 * Transport-scoped approval supervisor, independent of dialog mounting. Only
 * authoritative settlement/snapshot removes a prompt; timeout is not success.
 * Pending attempts are bounded by open prompts and pruned on identity change.
 */
export function createConfirmationDecisions(options: {
    panel: PanelStoreApi;
    send(prompt: ConfirmationPrompt, decision: 'approved' | 'denied', reason: string | undefined, requestId: string): boolean;
    snapshot(session: string, signal: AbortSignal): Promise<SessionDescription>;
    timeoutMs?: number;
}) {
    const states = createStore<ReadonlyMap<string, DecisionState>>(() => new Map());
    const attempts = new Map<string, Attempt>();
    const timeout = options.timeoutMs ?? 8000;
    let stopped = false;

    function publish(key: string, attempt?: Attempt) {
        const next = new Map(states.getState());
        if (attempt) next.set(key, attempt.state);
        else next.delete(key);
        states.setState(next, true);
    }
    function clear(key: string) {
        const attempt = attempts.get(key);
        if (!attempt) return;
        clearTimeout(attempt.timer);
        attempt.abort?.abort();
        attempts.delete(key);
        publish(key);
    }
    function current(attempt: Attempt) {
        const state = options.panel.getState();
        const prompt = state.confirmation(attempt.prompt.session_id, attempt.prompt.confirmation_id);
        return prompt && confirmationKey(prompt) === confirmationKey(attempt.prompt)
            && prompt.settled_at === null;
    }
    function connected(attempt: Attempt) {
        const state = options.panel.getState();
        const session = state.sessions.get(attempt.prompt.session_id);
        return state.connection.state === 'open' && session?.connected
            && session.identity.worker_id === attempt.prompt.worker_id;
    }
    function fail(key: string, attempt: Attempt, error: string, retryable: boolean) {
        clearTimeout(attempt.timer);
        attempt.abort?.abort();
        delete attempt.abort;
        attempt.state = { ...attempt.state, phase: 'failed', error, retryable };
        publish(key, attempt);
    }
    async function reconcile(key: string, attempt: Attempt) {
        if (!current(attempt) || attempts.get(key) !== attempt || stopped) return;
        if (!connected(attempt)) {
            fail(key, attempt, 'Connection lost; the decision outcome is unknown. Reconnect to check it.', false);
            return;
        }
        clearTimeout(attempt.timer);
        const abort = new AbortController();
        attempt.abort = abort;
        attempt.state = { ...attempt.state, phase: 'checking', error: '', retryable: false };
        publish(key, attempt);
        const deadline = setTimeout(() => abort.abort(), timeout);
        try {
            const session = await options.snapshot(attempt.prompt.session_id, abort.signal);
            if (attempts.get(key) !== attempt || attempt.abort !== abort || stopped || !current(attempt)) return;
            if (!connected(attempt) || session.session_id !== attempt.prompt.session_id
                || session.identity.worker_id !== attempt.prompt.worker_id) {
                fail(key, attempt, 'Worker connection changed; the decision outcome is unknown.', false);
                return;
            }
            const open = session.confirmations?.find(prompt => prompt.confirmation_id === attempt.prompt.confirmation_id);
            if (!open || confirmationKey(open) !== key || open.settled_at !== null) {
                // This only says the old prompt is no longer open. Do not
                // synthesize an approved/denied outcome or a tool result.
                options.panel.getState().applyConfirmation({ type: 'confirmation',
                    session: attempt.prompt.session_id, open: false, confirmation: attempt.prompt });
                clear(key);
            } else {
                fail(key, attempt, 'No confirmation received. The Hub still lists this prompt as open; you can retry.', true);
            }
        } catch {
            if (attempts.get(key) === attempt && attempt.abort === abort && !stopped) {
                fail(key, attempt, 'Could not check the decision outcome. Review again to check before retrying.', false);
            }
        } finally {
            clearTimeout(deadline);
        }
    }

    // State notifications never read/parse transcript bodies. Reconnect checks
    // unknown outcomes once; subsequent failures require an explicit review.
    const unsubscribe = options.panel.subscribe((next, previous) => {
        for (const [key, attempt] of attempts) {
            const session = next.sessions.get(attempt.prompt.session_id);
            if (!current(attempt) || (session?.identity.worker_id
                && session.identity.worker_id !== attempt.prompt.worker_id)) {
                clear(key);
            } else if (!connected(attempt) && attempt.state.phase !== 'failed') {
                fail(key, attempt, 'Connection lost; the decision outcome is unknown. Reconnect to check it.', false);
            } else if (attempt.state.phase === 'failed' && !attempt.state.retryable && connected(attempt)
                && (previous.connection.state !== next.connection.state
                    || previous.sessions.get(attempt.prompt.session_id)?.connected !== session?.connected)) {
                void reconcile(key, attempt);
            }
        }
    });

    return {
        states,
        submit(session: string, id: string, decision: 'approved' | 'denied', reason?: string): boolean {
            if (stopped) return false;
            const prompt = options.panel.getState().confirmation(session, id);
            if (!prompt || prompt.settled_at !== null
                || (prompt.deadline_at && Date.parse(prompt.deadline_at) <= Date.now())) return false;
            const key = confirmationKey(prompt);
            const previous = attempts.get(key);
            if (previous && (previous.state.phase !== 'failed' || !previous.state.retryable)) return false;
            const attempt: Attempt = { prompt, requestId: globalThis.crypto?.randomUUID?.()
                ?? `confirmation-${Date.now()}-${Math.random()}`, state: { decision, phase: 'pending', error: '', retryable: false } };
            if (!connected(attempt)) return false;
            if (previous) clear(key);
            // Close admission synchronously before sending: a double click or
            // another mounted control cannot submit a conflicting decision.
            attempts.set(key, attempt);
            publish(key, attempt);
            if (!options.send(prompt, decision, reason, attempt.requestId)) {
                fail(key, attempt, 'The panel is not connected, so nothing was sent.', true);
                return false;
            }
            attempt.timer = setTimeout(() => { void reconcile(key, attempt); }, timeout);
            return true;
        },
        review(prompt: ConfirmationPrompt) {
            const key = confirmationKey(prompt);
            const attempt = attempts.get(key);
            if (attempt?.state.phase === 'failed' && !attempt.state.retryable) void reconcile(key, attempt);
        },
        rejected(message: ErrorMessage) {
            const request = message.request as { type?: unknown; session?: unknown; confirmation_id?: unknown; request_id?: unknown } | undefined;
            const session = message.session ?? (request?.type === 'confirmation' ? request.session : undefined);
            const id = message.confirmation_id ?? (request?.type === 'confirmation' ? request.confirmation_id : undefined);
            if (typeof session !== 'string' || typeof id !== 'string') return;
            // Never treat an uncorrelated global notice as all prompts failing.
            for (const [key, attempt] of attempts) {
                if (attempt.prompt.session_id === session && attempt.prompt.confirmation_id === id
                    && attempt.state.phase === 'pending') {
                    if (typeof request?.request_id === 'string' && request.request_id !== attempt.requestId) continue;
                    if (request?.request_id === attempt.requestId && message.error !== 'unknown_confirmation') {
                        fail(key, attempt, message.message, true);
                    } else {
                        void reconcile(key, attempt); // Legacy/retired prompt: check rather than assume.
                    }
                }
            }
        },
        stop() {
            stopped = true;
            unsubscribe();
            for (const key of attempts.keys()) clear(key);
        },
    };
}

export type ConfirmationDecisions = ReturnType<typeof createConfirmationDecisions>;
