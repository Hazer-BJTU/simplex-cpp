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
 * - **A failed decision disabled the buttons forever** (D18). The old panel set
 *   `disabled = true` on click and only raised a toast on failure. Here a
 *   decision button is never disabled: the component only exists while the hub
 *   still lists the prompt as open, so the only thing a click can do is send the
 *   decision again, and a refusal leaves a usable button rather than a stuck
 *   dialog. The "waiting" line is information, not a lock.
 */
import { useEffect, useMemo, useState } from 'react';
import type { ConfirmationPrompt } from '../../../shared/protocol.ts';
import { usePanel } from '../state/usePanel.ts';
import { Badge, Button } from '../ui/Button.tsx';
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
function Approval({ prompt, autoOpen, onDefer, onReview }: {
    prompt: ConfirmationPrompt;
    autoOpen: boolean;
    /** The operator closed the dialog without deciding. */
    onDefer: () => void;
    /** The operator asked for a deferred prompt back. */
    onReview: () => void;
}) {
    const client = useClient();
    const [open, setOpen] = useState(autoOpen);
    const [sentAt, setSentAt] = useState<number | null>(null);
    const [localError, setLocalError] = useState('');
    const notice = usePanel((state) => state.notice);

    // Opening the first unanswered prompt is what makes an approval from
    // another session impossible to miss. It is not a re-open: `autoOpen` comes
    // from the caller, which stops offering a prompt once it has been deferred.
    useEffect(() => {
        if (autoOpen) setOpen(true);
    }, [autoOpen]);

    const waiting = sentAt !== null;

    // A refusal from the hub clears the waiting state, so the hint stops
    // claiming a decision is in flight when it never left. Without this the
    // line would be a lie the operator cannot correct — which is the same
    // failure D18 was, in a quieter form.
    useEffect(() => {
        if (sentAt === null) return;
        if (!notice) return;
        if (notice.code !== 'unknown_confirmation' && notice.code !== 'confirmation_rejected') return;
        setSentAt(null);
        setLocalError(notice.text);
    }, [notice, sentAt]);

    function decide(decision: 'approved' | 'denied') {
        setLocalError('');
        const sent = client.sendConfirmation(
            prompt.session_id, prompt.confirmation_id, decision, 'decided in the panel',
        );
        if (!sent) {
            // Nothing left the machine, so nothing is in flight and the button
            // stays usable. This is the case the old panel turned into a
            // permanently disabled dialog.
            setLocalError('the panel is not connected, so nothing was sent');
            return;
        }
        setSentAt(Date.now());
    }

    const summary = describeCall(prompt);

    return (
        <>
            <div
                data-testid="approval-banner"
                data-confirmation={prompt.confirmation_id}
                className="flex flex-wrap items-center gap-2 rounded border border-amber-300
                    bg-amber-50 px-2 py-1 text-xs text-amber-900"
            >
                <span className="font-semibold">approval required</span>
                <span className="font-mono">{prompt.session_id}</span>
                <span className="font-mono font-medium">{prompt.call?.name ?? '(unnamed)'}</span>
                <span className="max-w-96 truncate font-mono text-[11px] text-amber-800">
                    {summary}
                </span>
                {!prompt.verified && (
                    <Badge tone="bad" title={`worker identity is ${prompt.identity_state}`}>
                        unverified worker
                    </Badge>
                )}
                <span className="flex-1" />
                <Button size="sm" onClick={() => { onReview(); setOpen(true); }}>
                    Review
                </Button>
                <Button size="sm" variant="primary" onClick={() => decide('approved')}>
                    Approve
                </Button>
                <Button size="sm" variant="danger" onClick={() => decide('denied')}>
                    Deny
                </Button>
            </div>

            <Dialog
                open={open}
                onOpenChange={(next) => {
                    setOpen(next);
                    // Closing without deciding is a deferral, and it is
                    // remembered: a dialog that re-opens itself on the next
                    // render is the old panel's defect D17.
                    if (!next) onDefer();
                }}
            >
                <DialogContent
                    focus="none"
                    title={`${prompt.call?.name ?? 'A tool call'} needs approval`}
                    description={`session ${prompt.session_id} · run ${prompt.run_id}`}
                    footer={
                        <>
                            <DialogButton onClick={() => setOpen(false)}>Later</DialogButton>
                            <DialogButton variant="danger" onClick={() => decide('denied')}>
                                Deny
                            </DialogButton>
                            <DialogButton variant="primary" onClick={() => decide('approved')}>
                                Approve
                            </DialogButton>
                        </>
                    }
                >
                    {/* Radix focuses the first tabbable element unless told not
                        to, which here would arm a decision the operator has not
                        made. Nothing is focused, so Enter does nothing. */}
                    <ApprovalBody
                        prompt={prompt}
                        summary={summary}
                        waiting={waiting}
                        error={localError}
                    />
                </DialogContent>
            </Dialog>
        </>
    );
}

/** The dialog's contents: what is being asked, and by whom. */
function ApprovalBody({ prompt, summary, waiting, error }: {
    prompt: ConfirmationPrompt;
    summary: string;
    waiting: boolean;
    error: string;
}) {
    return (
        <div className="space-y-2">
            <pre className="overflow-x-auto whitespace-pre-wrap break-all rounded bg-slate-50
                px-2 py-1 font-mono text-[12px] text-slate-800">
                {summary}
            </pre>

            <details className="text-[11px] text-slate-500">
                <summary className="cursor-pointer select-none">arguments as the worker sent them</summary>
                <pre className="mt-1 max-h-64 overflow-auto whitespace-pre-wrap break-all rounded
                    bg-slate-50 px-2 py-1 font-mono text-slate-700">
                    {JSON.stringify(prompt.call?.arguments ?? {}, null, 2)}
                </pre>
            </details>

            <dl className="grid grid-cols-[8rem_1fr] gap-x-2 text-[11px]">
                <dt className="text-slate-400">security</dt>
                <dd className="font-mono text-slate-700">{prompt.call?.security ?? '(none reported)'}</dd>
                <dt className="text-slate-400">worker identity</dt>
                <dd className={prompt.verified ? 'text-slate-700' : 'text-rose-700'}>
                    {prompt.identity_state}{prompt.verified ? '' : ' — the hub could not verify this worker'}
                </dd>
                <dt className="text-slate-400">deadline</dt>
                <dd className="text-slate-700">
                    {prompt.deadline_at
                        ? new Date(prompt.deadline_at).toLocaleTimeString()
                        : 'none reported'}
                </dd>
            </dl>

            {waiting && (
                <p className="text-[11px] text-amber-800">
                    sent — waiting for the hub to confirm the decision. This is information, not
                    a lock: the buttons stay available, so a refusal can simply be retried.
                </p>
            )}
            {error && <p className="text-[11px] text-rose-700">{error}</p>}
        </div>
    );
}

export function Approvals() {
    // Prompts live in each session's view, so this collects across all of them.
    // `views` is replaced only when one of them actually changed, which is what
    // makes the memo effective and the selector safe: returning a fresh array
    // from a selector would re-render forever.
    const views = usePanel((state) => state.views);
    const prompts = useMemo(() => {
        const open: ConfirmationPrompt[] = [];
        for (const view of views.values()) open.push(...view.confirmations.values());
        return open
            .filter((prompt) => prompt.settled_at === null)
            .sort((a, b) => a.received_at.localeCompare(b.received_at));
    }, [views]);

    // Which prompts the operator has deferred. Deferring is remembered against
    // the prompt, so it does not come back on the next render (D17) — but the
    // banner keeps it one click away, because a deferred prompt that vanished
    // would be the same silent failure A1 was about.
    const [deferred, setDeferred] = useState<ReadonlySet<string>>(new Set());
    const firstUnanswered = prompts.find((prompt) => !deferred.has(prompt.confirmation_id));

    useEffect(() => {
        // A prompt that has been answered leaves the set behind; without this
        // the set would grow for the life of the page.
        setDeferred((current) => {
            const live = new Set(prompts.map((prompt) => prompt.confirmation_id));
            const next = new Set([...current].filter((id) => live.has(id)));
            return next.size === current.size ? current : next;
        });
    }, [prompts]);

    if (prompts.length === 0) return null;

    return (
        <section
            aria-label="pending approvals"
            className="space-y-1 border-b border-amber-200 bg-amber-50/60 px-4 py-2"
        >
            {prompts.map((prompt) => (
                <Approval
                    key={prompt.confirmation_id}
                    prompt={prompt}
                    autoOpen={prompt.confirmation_id === firstUnanswered?.confirmation_id}
                    onDefer={() => setDeferred((current) => (
                        new Set([...current, prompt.confirmation_id])
                    ))}
                    onReview={() => setDeferred((current) => {
                        if (!current.has(prompt.confirmation_id)) return current;
                        const next = new Set(current);
                        next.delete(prompt.confirmation_id);
                        return next;
                    })}
                />
            ))}
            {deferred.size > 0 && (
                <p className="text-[11px] text-amber-800">
                    {deferred.size} prompt(s) deferred. They stay here until answered or until the
                    worker's own deadline passes.
                </p>
            )}
            <button
                type="button"
                className="text-[11px] text-amber-900 underline-offset-2 hover:underline"
                onClick={() => setDeferred(new Set(prompts.map((p) => p.confirmation_id)))}
            >
                defer all
            </button>
        </section>
    );
}
