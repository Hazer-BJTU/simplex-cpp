/**
 * @file pending tool approvals.
 *
 * This is the consumer of the highest-impact fix in the rewrite: confirmations
 * now reach every connected panel, not only the one subscribed to the session
 * that raised them. In the old panel an approval raised by a session you were
 * not looking at was invisible — you saw a count in the sidebar and nothing
 * else — so the tool call failed when its deadline passed.
 *
 * Deliberately plain for this stage. The dialog, the deadline countdown and the
 * focus behaviour are the next stage; what matters here is that the prompt is
 * visible, that it says which session and which call, and that answering it
 * reports what the hub said rather than assuming it worked.
 */
import { useMemo, useState } from 'react';
import type { ConfirmationPrompt } from '../../../shared/protocol.ts';
import { usePanel } from '../state/usePanel.ts';
import { useClient } from './ClientContext.tsx';

/** The most recognisable form of a proposed call. */
function describeCall(prompt: ConfirmationPrompt): string {
    const args = prompt.call?.arguments;
    if (typeof args === 'object' && args !== null && !Array.isArray(args)) {
        const record = args as Record<string, unknown>;
        for (const key of ['command', 'path', 'url', 'query']) {
            if (typeof record[key] === 'string') return String(record[key]);
        }
    }
    try {
        return JSON.stringify(args ?? {});
    } catch {
        return String(args);
    }
}

/** One prompt. */
function Approval({ prompt }: { prompt: ConfirmationPrompt }) {
    const client = useClient();
    const [answered, setAnswered] = useState(false);
    const [error, setError] = useState('');

    function decide(decision: 'approved' | 'denied') {
        setError('');
        const sent = client.sendConfirmation(
            prompt.session_id, prompt.confirmation_id, decision, 'decided in the panel',
        );
        if (!sent) {
            setError('the panel is not connected, so nothing was sent');
            return;
        }
        // Sent is not decided. The button waits for the hub to say so, which is
        // why it is disabled here rather than reporting success.
        setAnswered(true);
    }

    return (
        <div
            data-testid="approval"
            data-confirmation={prompt.confirmation_id}
            className="rounded border border-amber-300 bg-amber-50 p-2"
        >
            <p className="flex flex-wrap items-baseline gap-2 text-xs text-amber-900">
                <span className="font-semibold">approval required</span>
                <span className="font-mono">{prompt.session_id}</span>
                <span className="font-mono font-medium">{prompt.call?.name ?? '(unnamed)'}</span>
                {prompt.call?.security && (
                    <span className="rounded bg-amber-200 px-1 text-[10px]">
                        {prompt.call.security}
                    </span>
                )}
                {!prompt.verified && (
                    <span className="rounded bg-rose-200 px-1 text-[10px] text-rose-900"
                        title={`worker identity is ${prompt.identity_state}`}>
                        unverified worker
                    </span>
                )}
            </p>

            <pre className="mt-1 overflow-x-auto whitespace-pre-wrap break-all font-mono
                text-[11px] text-amber-950">
                {describeCall(prompt)}
            </pre>

            <div className="mt-2 flex items-center gap-2">
                <button
                    type="button"
                    disabled={answered}
                    onClick={() => decide('approved')}
                    className="rounded bg-emerald-700 px-2 py-1 text-xs font-medium text-white
                        hover:bg-emerald-600 disabled:opacity-50"
                >
                    Approve
                </button>
                <button
                    type="button"
                    disabled={answered}
                    onClick={() => decide('denied')}
                    className="rounded bg-rose-700 px-2 py-1 text-xs font-medium text-white
                        hover:bg-rose-600 disabled:opacity-50"
                >
                    Deny
                </button>
                {answered && !error && (
                    <span className="text-[11px] text-amber-800">
                        sent — waiting for the hub to confirm the decision
                    </span>
                )}
                {error && <span className="text-[11px] text-rose-700">{error}</span>}
            </div>
        </div>
    );
}

export function Approvals() {
    // Prompts live in each session's view, so this collects across all of them.
    // `views` is replaced only when one of them actually changed, which is what
    // makes the memo below effective and the selector safe: returning a fresh
    // array from a selector would re-render forever.
    const views = usePanel((state) => state.views);
    const prompts = useMemo(() => {
        const open: ConfirmationPrompt[] = [];
        for (const view of views.values()) open.push(...view.confirmations.values());
        return open.sort((a, b) => a.received_at.localeCompare(b.received_at));
    }, [views]);

    if (prompts.length === 0) return null;

    return (
        <section
            aria-label="pending approvals"
            className="space-y-2 border-b border-amber-200 bg-amber-50/60 px-4 py-2"
        >
            {prompts.map((prompt) => (
                <Approval key={prompt.confirmation_id} prompt={prompt} />
            ))}
        </section>
    );
}
