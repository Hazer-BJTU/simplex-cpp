/**
 * @file pending tool approvals.
 *
 * This is the consumer of the highest-impact fix in the rewrite: confirmations
 * reach every connected panel, not only the one subscribed to the session that
 * raised them. In the old panel an approval raised by a session you were not
 * looking at was invisible — a count in the sidebar and nothing else — so the
 * tool call failed when its deadline passed.
 *
 * The old panel's approval modal also had three defects this is built to not
 * have, and each is a decision rather than a detail:
 *
 * - **Initial focus armed a button** (D16). `openModal` focused the first
 *   focusable element in DOM order, which was the header's "hide" button, so
 *   opening an approval and pressing Enter dismissed it silently. Here opening
 *   the dialog deliberately focuses nothing: Enter does nothing until the
 *   operator has actually chosen.
 * - **A dismissed prompt put itself back** (D17). The old panel re-opened every
 *   hidden prompt on each re-render. Here "Later" records the decision to defer,
 *   and the prompt waits in the banner — visible, answerable, and not in the way.
 * - **Pending is recoverable.** One transport-owned attempt locks only its
 *   prompt. Missing replies are reconciled against the Hub before retry;
 *   closing the dialog preserves that state. Only the Hub settles permission.
 */
import { useStore } from 'zustand';
import { confirmationKey } from '../lib/confirmationDecisions.ts';
import { memo, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { ConfirmationPrompt } from '../../../shared/protocol.ts';
import { profileCount } from '../lib/profile.ts';
import { usePanel, useVisiblePanel } from '../state/usePanel.ts';
import { Badge, Button } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Dialog, DialogButton, DialogContent } from '../ui/overlays.tsx';
import { useClient } from './ClientContext.tsx';
import { compactJson } from './content.ts';

/** The most recognisable form of a proposed call. */
function describeCall(prompt: ConfirmationPrompt): string {
    const args = prompt.call?.arguments;
    if (typeof args === 'object' && args !== null && !Array.isArray(args)) {
        const record = args as Record<string, unknown>;
        for (const key of ['command', 'path', 'url', 'query']) {
            if (typeof record[key] === 'string') return String(record[key]);
        }
    }
    return compactJson(args ?? {});
}

/** One prompt, answered. */
const Approval = memo(function Approval({ prompt, autoOpen, onDefer, onReview }: {
    prompt: ConfirmationPrompt;
    autoOpen: boolean;
    /** The operator closed the dialog without deciding. */
    onDefer: (key: string) => void;
    /** The operator asked for a deferred prompt back. */
    onReview: (key: string) => void;
}) {
    const client = useClient();
    const [open, setOpen] = useState(autoOpen);
    const key = confirmationKey(prompt);
    const submission = useStore(client.confirmations.states, state => state.get(key));
    const waiting = submission?.phase === 'pending' || submission?.phase === 'checking';
    const unknown = submission?.phase === 'failed' && !submission.retryable;
    const blocked = waiting || unknown;
    const primaryLabel = unknown || submission?.phase === 'checking' ? 'Check outcome' : 'Approve';
    const [localError, setLocalError] = useState('');
    const deadline = prompt.deadline_at ? Date.parse(prompt.deadline_at) : NaN;
    const [expired, setExpired] = useState(Number.isFinite(deadline) && deadline <= Date.now());
    // The Hub remains authoritative. Disable a locally expired prompt while
    // awaiting its settlement event, without claiming that the tool stopped.
    useEffect(() => {
        if (!Number.isFinite(deadline)) {
            setExpired(false);
            return;
        }
        let timer: ReturnType<typeof setTimeout>;
        function check(): void {
            const remaining = deadline - Date.now();
            setExpired(remaining <= 0);
            if (remaining > 0) {
                timer = setTimeout(check, Math.min(remaining, 2_147_483_647));
            }
        }
        check();
        return () => clearTimeout(timer);
    }, [deadline]);
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

    // Opening the first unanswered prompt is what makes an approval from
    // another session impossible to miss. It is not a re-open: `autoOpen` comes
    // from the caller, which stops offering a prompt once it has been deferred.
    useEffect(() => {
        if (autoOpen) setOpen(true);
    }, [autoOpen]);

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
            // Nothing left the machine, so nothing is in flight and the button
            // stays usable. This is the case the old panel turned into a
            // permanently disabled dialog.
            setLocalError('The panel or worker is disconnected; no decision was sent.');
            return;
        }
    }

    const summary = useMemo(() => describeCall(prompt), [prompt]);

    return (
        <>
            <span role="status" aria-live="polite" className="sr-only">
                {waiting ? submission?.phase === 'checking'
                    ? 'Checking the decision outcome with the Hub.'
                    : 'Decision sent; waiting for the Hub to confirm.' : ''}
            </span>
            <div
                data-testid="approval-banner"
                data-confirmation={prompt.confirmation_id}
                className="approval-row"
            >
                <div className="min-w-0 flex-1 space-y-1">
                    <div className="flex flex-wrap items-center gap-x-2 gap-y-1 text-xs">
                        <span className="inline-flex items-center gap-1 font-medium text-warn">
                            <Glyph name="approval" />Approval required
                        </span>
                        <span className="break-all font-mono text-ink-muted">
                            {prompt.session_id}{parent ? ` · parent ${parent}` : ''}
                        </span>
                        {!prompt.verified && (
                            <Badge tone="bad" title={`worker identity is ${prompt.identity_state}`}>
                                unverified worker
                            </Badge>
                        )}
                    </div>
                    <p className="truncate text-xs text-ink-muted">
                        <span className="font-mono font-medium text-ink">{prompt.call?.name ?? '(unnamed)'}</span>
                        {' · '}<span className="font-mono">{summary}</span>
                    </p>
                </div>
                <div className="flex shrink-0 items-center gap-1">
                    <Button size="sm" variant="ghost" onClick={() => { onReview(key); client.confirmations.review(prompt); setOpen(true); }}>
                        Review
                    </Button>
                    <Button size="sm" variant="primary" aria-label={primaryLabel} disabled={expired || waiting} onClick={() => decide('approved')}>
                        <DecisionLabel waiting={waiting && (submission?.decision === 'approved' || submission?.phase === 'checking')}>{primaryLabel}</DecisionLabel>
                    </Button>
                    <Button size="sm" variant="danger" aria-label="Deny" disabled={expired || blocked} onClick={() => decide('denied')}>
                        <DecisionLabel waiting={waiting && submission?.decision === 'denied' && submission?.phase !== 'checking'}>Deny</DecisionLabel>
                    </Button>
                </div>
            </div>

            <Dialog
                open={open}
                onOpenChange={(next) => {
                    setOpen(next);
                    // Closing without deciding is a deferral, and it is
                    // remembered: a dialog that re-opens itself on the next
                    // render is the old panel's defect D17.
                    if (!next) onDefer(key);
                }}
            >
                <DialogContent
                    focus="self"
                    title={`${prompt.call?.name ?? 'A tool call'} needs approval`}
                    description={`session ${prompt.session_id}${parent ? ` · parent ${parent}` : ''} · run ${prompt.run_id}`}
                    footer={
                        <>
                            <DialogButton onClick={() => { onDefer(key); setOpen(false); }}>Later</DialogButton>
                            <DialogButton variant="danger" aria-label="Deny" disabled={expired || blocked} onClick={() => decide('denied')}>
                                <DecisionLabel waiting={waiting && submission?.decision === 'denied' && submission?.phase !== 'checking'}>Deny</DecisionLabel>
                            </DialogButton>
                            <DialogButton variant="primary" aria-label={primaryLabel} disabled={expired || waiting} onClick={() => decide('approved')}>
                                <DecisionLabel waiting={waiting && (submission?.decision === 'approved' || submission?.phase === 'checking')}>{primaryLabel}</DecisionLabel>
                            </DialogButton>
                        </>
                    }
                >
                    {/* The dialog container takes focus rather than its first
                        button: nothing is armed, so Enter cannot decide, and the
                        prompt is still announced because focus is inside the
                        labelled modal. */}
                    {expired && <p role="status" className="mb-3 text-sm text-warn">
                        Approval deadline passed. Waiting for the Hub to report the outcome.
                    </p>}
                    <ApprovalBody
                        prompt={prompt}
                        summary={summary}
                        status={waiting ? submission?.phase === 'checking'
                            ? 'Checking the decision outcome with the Hub.'
                            : 'Decision sent; waiting for the Hub to confirm.' : ''}
                        error={localError || submission?.error || ''}
                    />
                </DialogContent>
            </Dialog>
        </>
    );
});

/** The dialog's contents: what is being asked, and by whom. */
const ApprovalBody = memo(function ApprovalBody({ prompt, summary, status, error }: {
    prompt: ConfirmationPrompt;
    summary: string;
    status: string;
    error: string;
}) {
    const [argumentsOpen, setArgumentsOpen] = useState(false);
    const argumentsText = useMemo(() => argumentsOpen
        ? JSON.stringify(prompt.call?.arguments ?? {}, null, 2) : '', [argumentsOpen, prompt.call?.arguments]);
    return (
        <div className="space-y-2">
            <span role="status" aria-live="polite" className="sr-only">{status}</span>
            <pre className="max-h-48 overflow-auto whitespace-pre-wrap break-all rounded bg-sunken
                px-2 py-1 font-mono text-xs text-ink">
                {summary}
            </pre>

            <details onToggle={event => setArgumentsOpen(event.currentTarget.open)} className="text-xs text-ink-muted">
                <summary onClick={event => setArgumentsOpen(!(event.currentTarget.parentElement as HTMLDetailsElement).open)}
                    className="cursor-pointer select-none">arguments as the worker sent them</summary>
                <pre className="mt-1 max-h-64 overflow-auto whitespace-pre-wrap break-all rounded
                    bg-sunken px-2 py-1 font-mono text-ink">
                    {argumentsText}
                </pre>
            </details>

            <dl className="grid grid-cols-[8rem_1fr] gap-x-2 text-xs">
                <dt className="text-ink-faint">security</dt>
                <dd className="font-mono text-ink">{prompt.call?.security ?? '(none reported)'}</dd>
                <dt className="text-ink-faint">worker identity</dt>
                <dd className={prompt.verified ? 'text-ink' : 'text-danger'}>
                    {prompt.identity_state}{prompt.verified ? '' : ' — the hub could not verify this worker'}
                </dd>
                <dt className="text-ink-faint">deadline</dt>
                <dd className="text-ink">
                    {prompt.deadline_at
                        ? new Date(prompt.deadline_at).toLocaleTimeString()
                        : 'none reported'}
                </dd>
            </dl>

            {error && <p className="text-xs text-danger">{error}</p>}
        </div>
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

export function Approvals() {
    profileCount('approvals');
    // Only prompt identities trigger this subscription, not logs or messages.
    const prompts = useVisiblePanel(true, state => {
        const open: ConfirmationPrompt[] = [];
        for (const view of state.views.values()) open.push(...view.confirmations.values());
        return open.filter(prompt => prompt.settled_at === null)
            .sort((a, b) => a.received_at.localeCompare(b.received_at));
    });

    // Which prompts the operator has deferred. Deferring is remembered against
    // the prompt, so it does not come back on the next render (D17) — but the
    // banner keeps it one click away, because a deferred prompt that vanished
    // would be the same silent failure A1 was about.
    const [deferred, setDeferred] = useState<ReadonlySet<string>>(new Set());
    const firstUnanswered = prompts.find((prompt) => !deferred.has(confirmationKey(prompt)));

    useEffect(() => {
        // A prompt that has been answered leaves the set behind; without this
        // the set would grow for the life of the page.
        setDeferred((current) => {
            const live = new Set(prompts.map(confirmationKey));
            const next = new Set([...current].filter((id) => live.has(id)));
            return next.size === current.size ? current : next;
        });
    }, [prompts]);

    const defer = useCallback((key: string) => setDeferred(current => new Set([...current, key])), []);
    const review = useCallback((key: string) => setDeferred(current => {
        if (!current.has(key)) return current;
        const next = new Set(current);
        next.delete(key);
        return next;
    }), []);
    const list = useRef<HTMLElement>(null);
    const hasPrompts = prompts.length > 0;
    useLayoutEffect(() => {
        const node = list.current;
        if (!node) return;
        // The list can scroll independently of the transcript. Account for its
        // own scrollbar before adding the shared conversation gutter.
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
            className="approval-list animate-enter"
        >
            {/* One line for the count rather than a live region around the whole
                section: the prompt bodies change as decisions are sent, and a
                screen reader should hear "2 approvals waiting" once, not every
                keystroke of a re-render. */}
            <p role="status" aria-atomic="true" className="sr-only">
                {prompts.length} approval{prompts.length === 1 ? '' : 's'} waiting for a decision
            </p>
            <div className="reading-width">
                {prompts.map((prompt) => (
                    <Approval
                        key={confirmationKey(prompt)}
                        prompt={prompt}
                        autoOpen={prompt === firstUnanswered}
                        onDefer={defer}
                        onReview={review}
                    />
                ))}
                {deferred.size > 0 && (
                    <p className="text-xs text-warn">
                        {deferred.size} prompt(s) deferred. They stay here until answered or until the
                        worker's own deadline passes.
                    </p>
                )}
                <button
                    type="button"
                    className="text-xs text-warn underline-offset-2 hover:underline"
                    onClick={() => setDeferred(new Set(prompts.map(confirmationKey)))}
                >
                    defer all
                </button>
            </div>
        </section>
    );
}
