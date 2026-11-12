/**
 * @file pending tool approvals.
 *
 * An approval is a decision the run is waiting on, not an interruption. The
 * call it belongs to is already in the transcript above; the operator is
 * usually typing in the composer below. So the block lives where an `ask_user`
 * question lives — above the composer, inside the reading measure — and never
 * covers the conversation, dims the page, or takes focus.
 *
 * The properties this is built to keep, each a decision rather than a detail:
 *
 * - **Nothing is armed** (D16). The old panel's modal focused its own "hide"
 *   button, so Enter dismissed a decision that had not been made. Here no
 *   keystroke decides anything: the block never takes focus, and both actions
 *   are explicit clicks.
 * - **A deferred prompt does not vanish or return** (D17). There is nothing to
 *   dismiss: the block stays until the Hub settles it, so a prompt cannot be
 *   lost in a corner *or* re-open itself on the next render.
 * - **Pending is recoverable.** One transport-owned attempt locks only its
 *   prompt. Missing replies are reconciled against the Hub before retry; the
 *   block keeps showing the attempt's state. Only the Hub settles permission.
 * - **An approval from another session is still visible** (A1). The block names
 *   the session that raised it, and the status bar counts what is waiting.
 *
 * The one thing the old modal did that a block cannot is take the whole screen.
 * That is replaced by a quieter cue: a new block draws its top rule in the
 * warning tone once, and the top bar keeps a count for as long as anything is
 * waiting.
 */
import { useStore } from 'zustand';
import { confirmationKey } from '../lib/confirmationDecisions.ts';
import { profileCount } from '../lib/profile.ts';
import { memo, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { ConfirmationPrompt } from '../../../ui/index.ts';
import { usePanel, useVisiblePanel } from '../state/usePanel.ts';
import { viewProjection } from '../state/viewProjection.ts';
import { Badge } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { useClient } from './ClientContext.tsx';
import { compactJson } from './content.ts';

/** The most recognisable form of a proposed call. */
function describeCall(prompt: ConfirmationPrompt): string {
    const args = prompt.call?.arguments;
    if (typeof args === 'object' && args !== null && !Array.isArray(args)) {
        const record = args as Record<string, unknown>;
        for (const key of ['command', 'path', 'url', 'query']) {
            if (typeof record[key] === 'string') return String(record[key]);
            const value = record[key];
            if (value !== null && typeof value === 'object' && !Array.isArray(value)) {
                const preview = value as Record<string, unknown>;
                if (preview.display_truncated === true && typeof preview.preview === 'string') {
                    return `${preview.preview}\n[argument preview]`;
                }
            }
        }
    }
    return compactJson(args ?? {});
}

/** Minutes and seconds, like the question card: the same clock, the same words. */
function countdown(deadline: number, now: number): string {
    const seconds = Math.max(0, Math.ceil((deadline - now) / 1000));
    return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
}

/** One prompt, as a block in the conversation's own measure. */
const Approval = memo(function Approval({ prompt }: { prompt: ConfirmationPrompt }) {
    const client = useClient();
    const key = confirmationKey(prompt);
    const submission = useStore(client.confirmations.states, state => state.get(key));
    const waiting = submission?.phase === 'pending' || submission?.phase === 'checking';
    const unknown = submission?.phase === 'failed' && !submission.retryable;
    const blocked = waiting || unknown;
    const primaryLabel = unknown || submission?.phase === 'checking' ? 'Check outcome' : 'Approve';
    const [localError, setLocalError] = useState('');
    const [now, setNow] = useState(() => Date.now());
    const [argumentsOpen, setArgumentsOpen] = useState(false);
    const deadline = prompt.deadline_at ? Date.parse(prompt.deadline_at) : NaN;
    const expired = Number.isFinite(deadline) && deadline <= now;

    // The Hub is authoritative about the deadline; this only stops the block
    // from offering an action that can no longer be accepted, and shows the
    // remaining time in the operator's own units.
    useEffect(() => {
        if (!Number.isFinite(deadline)) return;
        const timer = setInterval(() => setNow(Date.now()), 1000);
        return () => clearInterval(timer);
    }, [deadline]);

    function decide(decision: 'approved' | 'denied') {
        if (decision === 'approved' && unknown) {
            client.confirmations.review(prompt);
            return;
        }
        if (blocked || prompt.settled_at !== null || (Number.isFinite(deadline) && deadline <= Date.now())) return;
        setLocalError('');
        const sent = client.sendConfirmation(
            prompt.session_id, prompt.confirmation_id, decision, 'decided in the panel',
        );
        if (!sent && !client.confirmations.states.getState().has(key)) {
            // Nothing left the machine, so nothing is in flight and the buttons
            // stay usable. This is the case the old panel turned into a
            // permanently disabled dialog.
            setLocalError('The panel or worker is disconnected; no decision was sent.');
        }
    }

    const parent = usePanel(state => {
        const chain: string[] = [];
        const seen = new Set<string>([prompt.session_id]);
        let id = state.sessions.get(prompt.session_id)?.subagent?.parent;
        while (id && !seen.has(id)) {
            seen.add(id);
            chain.push(id);
            id = state.sessions.get(id)?.subagent?.parent;
        }
        return chain.join(' ← ');
    });
    const summary = useMemo(() => describeCall(prompt), [prompt]);
    // Stringify only what is open: a bounded argument preview can still be
    // tens of kilobytes, and most prompts are answered without expanding it.
    const argumentsText = useMemo(() => argumentsOpen
        ? JSON.stringify(prompt.call?.arguments ?? {}, null, 2) : '',
        [argumentsOpen, prompt.call?.arguments]);
    const error = localError || submission?.error || '';

    return (
        <article
            data-testid="approval-block"
            data-confirmation={prompt.confirmation_id}
            aria-label={`${prompt.call?.name ?? 'Tool call'} needs approval`}
            className="approval-block"
        >
            <div className="flex flex-wrap items-center gap-x-2 gap-y-1 text-xs">
                <span className="inline-flex items-center gap-1 font-medium text-warn">
                    <span aria-hidden="true" className="size-1.5 rounded-full bg-warn" />
                    Approval
                </span>
                <span className="break-all font-mono text-ink-muted">
                    {prompt.session_id}{parent ? ` · parent ${parent}` : ''}
                </span>
                <span className="break-all font-mono text-ink-muted">
                    · {prompt.call?.name ?? '(unnamed)'}
                </span>
                {!prompt.verified && (
                    <Badge tone="bad" title={`worker identity is ${prompt.identity_state}`}>
                        unverified worker
                    </Badge>
                )}
                <span className="ml-auto tabular-nums text-ink-muted">
                    {!Number.isFinite(deadline) ? 'no deadline'
                        : expired ? 'deadline passed' : `${countdown(deadline, now)} left`}
                </span>
            </div>

            <pre className="mt-1.5 max-h-[min(8rem,16dvh)] overflow-y-auto whitespace-pre-wrap font-mono
                text-sm leading-relaxed text-ink [overflow-wrap:anywhere]">
                {summary}
            </pre>

            {prompt.arguments_truncated && (
                <p className="mt-1 text-xs text-ink-muted" role="status">
                    Argument preview only. The decision applies to the original operation; omitted
                    details are unavailable in this panel.
                </p>
            )}

            <div className="mt-1 flex flex-wrap items-start gap-x-4 gap-y-1">
                <button
                    type="button"
                    data-testid="approval-disclosure"
                    aria-expanded={argumentsOpen}
                    onClick={() => setArgumentsOpen(!argumentsOpen)}
                    className="inline-flex items-center gap-1 text-left text-xs text-ink-muted
                        underline-offset-2 hover:underline"
                >
                    arguments · security · worker identity
                    <span
                        aria-hidden="true"
                        className={`inline-block text-ink-faint transition-transform
                            ${argumentsOpen ? 'rotate-90' : ''}`}
                    >
                        ▸
                    </span>
                </button>

                <div className="-mr-2 flex w-full shrink-0 items-center justify-end gap-1 sm:w-auto">
                    <span role="status" aria-live="polite" className="sr-only">
                        {waiting ? submission?.phase === 'checking'
                            ? 'Checking the decision outcome with the Hub.'
                            : 'Decision sent; waiting for the Hub to confirm.' : ''}
                    </span>
                    <button
                        type="button"
                        aria-label="Deny"
                        disabled={expired || blocked}
                        onClick={() => decide('denied')}
                        className="rounded px-2 py-1 text-sm text-danger underline-offset-2
                            hover:bg-danger-soft focus-visible:underline disabled:text-ink-faint
                            disabled:no-underline disabled:hover:bg-transparent"
                    >
                        <DecisionLabel waiting={waiting && submission?.decision === 'denied' && submission?.phase !== 'checking'}>
                            Deny
                        </DecisionLabel>
                    </button>
                    <button
                        type="button"
                        aria-label={primaryLabel}
                        disabled={expired || waiting}
                        onClick={() => decide('approved')}
                        className="rounded px-2 py-1 text-sm font-semibold text-ink underline-offset-2
                            hover:bg-subtle focus-visible:underline disabled:text-ink-faint
                            disabled:no-underline disabled:hover:bg-transparent"
                    >
                        <DecisionLabel waiting={waiting && (submission?.decision === 'approved' || submission?.phase === 'checking')}>
                            {primaryLabel}
                        </DecisionLabel>
                    </button>
                </div>
            </div>

            {/* The evidence opens below the decision row: expanding it must not
                push the two actions out of the strip. */}
            {argumentsOpen && (
                <div className="mt-1">
                    <pre data-testid="approval-arguments"
                        className="max-h-48 overflow-auto whitespace-pre-wrap break-all font-mono
                            text-xs text-ink">
                        {argumentsText}
                    </pre>
                    <dl className="mt-1 grid grid-cols-[7.5rem_1fr] gap-x-2 text-xs">
                        <dt className="text-ink-faint">security</dt>
                        <dd className="font-mono text-ink">{prompt.call?.security ?? '(none reported)'}</dd>
                        <dt className="text-ink-faint">worker identity</dt>
                        <dd className={prompt.verified ? 'text-ink' : 'text-danger'}>
                            {prompt.identity_state}
                            {prompt.verified ? '' : ' — the hub could not verify this worker'}
                        </dd>
                    </dl>
                </div>
            )}

            {expired && (
                <p className="mt-1 text-xs text-warn" role="status">
                    Approval deadline passed. Waiting for the Hub to report the outcome.
                </p>
            )}
            {error && <p className="mt-1 text-xs text-danger">{error}</p>}
        </article>
    );
});

/** Feedback replaces the existing label within exactly the same geometry. */
function DecisionLabel({ waiting, children }: { waiting: boolean; children: string }) {
    return <span className="relative inline-flex justify-center">
        <span className={waiting ? 'invisible' : ''}>{children}</span>
        {waiting && <span aria-hidden="true" className="absolute inset-0 flex items-center justify-center">
            <Glyph name="spinner" size="sm" />
        </span>}
    </span>;
}

/** Inline strip above the composer; renders nothing when nothing is open. */
export function Approvals() {
    profileCount('approvals');
    // Read the pre-indexed projection: unrelated events do no prompt scan/sort.
    // Actual prompt changes still reach the panel synchronously.
    const prompts = useVisiblePanel(true, state => viewProjection(state.views).prompts);
    const list = useRef<HTMLElement>(null);
    const hasPrompts = prompts.length > 0;
    useLayoutEffect(() => {
        const node = list.current;
        if (!node) return;
        // The strip can scroll independently of the transcript. Account for its
        // own scrollbar before adding the shared conversation gutter, so its
        // edges line up with the reading measure and the composer.
        const update = () => node.style.setProperty(
            '--approval-scrollbar', `${node.offsetWidth - node.clientWidth}px`,
        );
        update();
        const observer = new ResizeObserver(update);
        observer.observe(node);
        return () => observer.disconnect();
    }, [hasPrompts]);

    if (!hasPrompts) return null;

    return (
        <section
            ref={list}
            aria-label="pending approvals"
            data-testid="approvals"
            className="approvals-shell animate-enter"
        >
            {/* One line for the count rather than a live region around the whole
                section: the blocks change as decisions are sent, and a screen
                reader should hear "2 approvals waiting" once, not every
                keystroke of a re-render. */}
            <p role="status" aria-atomic="true" className="sr-only">
                {prompts.length} approval{prompts.length === 1 ? '' : 's'} waiting for a decision
            </p>
            <div className="reading-width">
                {prompts.map((prompt) => (
                    <Approval key={confirmationKey(prompt)} prompt={prompt} />
                ))}
            </div>
        </section>
    );
}
