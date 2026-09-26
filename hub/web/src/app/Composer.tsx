/**
 * @file the composer.
 *
 * The old one put every control on a single row — Send, Continue, a URL field,
 * "Add ref", a confirmation-mode `<select>`, and a hint — and let `flex-wrap`
 * sort it out. Worse, the confirmation mode was one DOM control shared by every
 * session and never reset, so choosing `approve` in one session silently applied
 * it to all of them (defect D15): `approve` means "approve every call that would
 * have asked", so the leak quietly disabled approvals session-wide.
 *
 * Here the mode belongs to a session, lives in a compact settings popover, and is shown
 * as a permanent badge whenever it is not the default — because a setting that
 * turns approvals off should not be discoverable only by opening the thing that
 * sets it.
 *
 * The other two carried-over behaviours: the draft survives a refused send
 * (defect D19, fixed in the store), and Enter sends while Shift+Enter breaks the
 * line.
 */
import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import type { ContentPart, PayloadOptions } from '../../../shared/protocol.ts';
import { usePanel, useSession, useView } from '../state/usePanel.ts';
import { Badge, Button, IconButton } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Popover, PopoverContent, PopoverTrigger } from '../ui/overlays.tsx';
import type { ConfirmMode } from '../state/store.ts';
import { useClient } from './ClientContext.tsx';

/** How tall the textarea may grow before it scrolls instead. */
const MAX_HEIGHT_PX = 220;

/** One removable part the operator attached. */
interface Reference {
    readonly kind: 'external_ref';
    readonly raw: string;
}

/** How a confirmation mode reads, and how loudly. */
const MODES: Record<ConfirmMode, { label: string; detail: string; tone: 'neutral' | 'warn' | 'bad' }> = {
    ask: {
        label: 'ask',
        detail: 'every call that needs approval opens a prompt',
        tone: 'neutral',
    },
    approve: {
        label: 'approve',
        detail: 'every call that would have asked is approved without a prompt',
        tone: 'bad',
    },
    deny: {
        label: 'deny',
        detail: 'every call that would have asked is denied without a prompt',
        tone: 'warn',
    },
};

export function Composer() {
    const client = useClient();
    const selected = usePanel((state) => state.selected);
    const session = useSession(selected);
    const view = useView(selected);
    const failed = usePanel((state) => state.failedInput);
    const clearFailedInput = usePanel((state) => state.clearFailedInput);
    const connectionState = usePanel((state) => state.connection.state);
    const mode = usePanel((state) => (state.selected
        ? state.confirmMode.get(state.selected) ?? 'ask'
        : 'ask'));
    const setConfirmMode = usePanel((state) => state.setConfirmMode);

    const [draft, setDraft] = useState('');
    const [references, setReferences] = useState<readonly Reference[]>([]);
    const [refOpen, setRefOpen] = useState(false);
    const box = useRef<HTMLTextAreaElement>(null);

    // Switching sessions must not carry one session's draft, or its references,
    // into another.
    useEffect(() => {
        setDraft('');
        setReferences([]);
        setRefOpen(false);
    }, [selected]);

    // Restore each refused part in its original form. A reference must remain
    // an external_ref: turning its URL into message text silently changes the
    // next request. This runs after the selection reset so a failed request
    // for the newly selected session is not cleared by that reset.
    useEffect(() => {
        if (!failed || failed.sessionId !== selected) return;
        if (failed.operation === 'continue') {
            // A continuation has no content to refund. In particular, a
            // rejected continuation must not erase a separate unsent draft.
            clearFailedInput();
            return;
        }
        setDraft(failed.parts.filter((part) => part.type === 'text')
            .map((part) => part.raw).join('\n\n'));
        setReferences(failed.parts.filter((part) => part.type === 'external_ref')
            .map((part) => ({ kind: 'external_ref', raw: part.raw })));
        clearFailedInput();
        box.current?.focus();
    }, [failed, selected, clearFailedInput]);

    // Grow with the content, up to a limit, then scroll. `height` is reset first
    // so the textarea can also shrink when the text does.
    useLayoutEffect(() => {
        const node = box.current;
        if (!node) return;
        node.style.height = 'auto';
        node.style.height = `${Math.min(node.scrollHeight, MAX_HEIGHT_PX)}px`;
    }, [draft]);

    if (!selected || !session) return null;

    const sessionId: string = selected;
    const connected = session.connected;
    const runActive = Boolean(view?.runActive);
    const canSend = draft.trim().length > 0 || references.length > 0;

    function parts(): ContentPart[] {
        const list: ContentPart[] = [];
        if (draft.trim().length > 0) list.push({ type: 'text', raw: draft });
        for (const reference of references) {
            list.push({ type: 'external_ref', raw: reference.raw });
        }
        return list;
    }

    function send(operation: 'message' | 'continue' = 'message'): void {
        if (operation === 'continue' ? !runActive : !canSend) return;
        // The mode travels with the payload. The worker freezes the policy per
        // run, so sending it every time is what makes a change take effect on
        // the next run rather than the next restart — and what keeps one
        // session's choice out of another's.
        const options: PayloadOptions = { confirmation: { mode } };
        const sent = client.sendInput(
            sessionId,
            operation === 'continue' ? [] : parts(),
            operation,
            options,
        );
        // Clear only when the frame actually left; otherwise the store has
        // already handed the text back and clearing here would lose it.
        if (sent && operation === 'message') {
            setDraft('');
            setReferences([]);
        }
    }

    return (
        <form
            className="shrink-0 border-t border-line bg-sunken px-3 py-3 sm:px-5"
            onSubmit={(event) => {
                event.preventDefault();
                send();
            }}
        >
            <div className="mx-auto max-w-4xl overflow-hidden rounded-2xl border border-line-strong
                bg-surface shadow-sm transition-colors focus-within:border-interactive">
                {references.length > 0 && (
                    <ul className="flex flex-wrap gap-1.5 px-4 pt-3">
                        {references.map((reference) => (
                            <li
                                key={reference.raw}
                                className="flex items-center gap-1 rounded-full border border-line
                                    bg-subtle py-1 pl-2.5 pr-1 text-xs text-ink-muted"
                            >
                                <span className="max-w-72 truncate font-mono">{reference.raw}</span>
                                <button
                                    type="button"
                                    aria-label={`remove reference ${reference.raw}`}
                                    className="rounded-full p-0.5 hover:bg-line"
                                    onClick={() => setReferences((current) => (
                                        current.filter((item) => item.raw !== reference.raw)
                                    ))}
                                >
                                    <Glyph name="close" size="sm" />
                                </button>
                            </li>
                        ))}
                    </ul>
                )}

                <textarea
                    ref={box}
                    value={draft}
                    rows={2}
                    aria-label="message"
                    onChange={(event) => setDraft(event.target.value)}
                    onKeyDown={(event) => {
                        if (event.key === 'Enter' && !event.shiftKey) {
                            event.preventDefault();
                            send();
                            return;
                        }
                        if (event.key === 'Escape') {
                            event.preventDefault();
                            setDraft('');
                        }
                    }}
                    placeholder={connected ? 'Message the worker…' : 'No worker is attached to this session'}
                    className="block min-h-16 w-full resize-none bg-transparent px-4 pb-1 pt-3
                        text-sm text-ink placeholder:text-ink-faint focus:outline-none"
                />

                <div className="flex items-center gap-2 px-3 pb-3">
                    <Popover open={refOpen} onOpenChange={setRefOpen}>
                        <PopoverTrigger asChild>
                            <IconButton label="Attach a reference" disabled={!connected}>
                                <Glyph name="attach" />
                            </IconButton>
                        </PopoverTrigger>
                        <PopoverContent align="start" width="w-80">
                            <form
                                onSubmit={(event) => {
                                    event.preventDefault();
                                    const field = event.currentTarget.elements.namedItem('reference');
                                    if (!(field instanceof HTMLInputElement)) return;
                                    const value = field.value.trim();
                                    if (!value) return;
                                    // A reference is sent as data, not fetched by the panel.
                                    setReferences((current) => (
                                        current.some((item) => item.raw === value)
                                            ? current
                                            : [...current, { kind: 'external_ref', raw: value }]
                                    ));
                                    field.value = '';
                                    setRefOpen(false);
                                }}
                            >
                                <label className="block text-xs font-medium text-ink-muted"
                                    htmlFor="composer-reference">
                                    external reference
                                </label>
                                <input
                                    id="composer-reference"
                                    name="reference"
                                    autoFocus
                                    placeholder="https://…"
                                    className="mt-1 w-full rounded border border-line-strong px-2 py-1
                                        font-mono text-xs focus:border-line-strong focus:outline-none"
                                />
                                <p className="mt-1 text-xs text-ink-muted">
                                    Sent to the worker as an <code>external_ref</code> part. The panel
                                    never fetches it.
                                </p>
                                <div className="mt-2 flex justify-end">
                                    <Button type="submit" variant="primary">Attach</Button>
                                </div>
                            </form>
                        </PopoverContent>
                    </Popover>

                    <Popover>
                        <PopoverTrigger asChild>
                            <button
                                type="button"
                                aria-label={`confirmation mode: ${mode}`}
                                className="inline-flex h-8 items-center gap-1.5 rounded-full px-2.5
                                    text-xs text-ink-muted hover:bg-subtle hover:text-ink"
                                title="how the worker should answer tool confirmations for this session"
                            >
                                <Glyph name="options" size="sm" />
                                <span className="hidden sm:inline">confirmation ·</span>
                                <span>{mode}</span>
                            </button>
                        </PopoverTrigger>
                        <PopoverContent align="start">
                            <p className="text-xs font-medium text-ink">
                                Confirmation mode for <span className="font-mono">{sessionId}</span>
                            </p>
                            <div className="mt-2 space-y-1">
                                {(['ask', 'approve', 'deny'] as const).map((value) => (
                                    <label
                                        key={value}
                                        className="flex cursor-pointer items-start gap-2 rounded p-1
                                            hover:bg-sunken"
                                    >
                                        <input
                                            type="radio"
                                            name="confirm-mode"
                                            value={value}
                                            checked={mode === value}
                                            onChange={() => setConfirmMode(sessionId, value)}
                                            className="mt-0.5 accent-interactive"
                                        />
                                        <span>
                                            <span className="font-mono text-xs text-ink">{value}</span>
                                            <span className="block text-xs text-ink-muted">
                                                {MODES[value].detail}
                                            </span>
                                        </span>
                                    </label>
                                ))}
                            </div>
                            <p className="mt-2 border-t border-line pt-2 text-xs text-ink-muted">
                                This choice applies to the next run for this session.
                            </p>
                        </PopoverContent>
                    </Popover>

                    {runActive && (
                        <Button
                            onClick={() => send('continue')}
                            title="ask the worker to continue the run without a new message"
                            className="ml-auto"
                        >
                            Continue
                        </Button>
                    )}
                    <Button
                        type="submit"
                        variant="primary"
                        size="md"
                        disabled={!canSend}
                        className={runActive ? '' : 'ml-auto'}
                        icon={<Glyph name="send" />}
                    >
                        Send
                    </Button>
                </div>
            </div>

            <div className="mx-auto mt-2 flex max-w-4xl flex-wrap items-center gap-x-3 gap-y-1
                px-1 text-xs text-ink-muted">
                {connectionState !== 'open' && (
                    <span className="text-warn">the panel is not connected</span>
                )}
                {!connected && <span>no worker attached</span>}
                {mode !== 'ask' && (
                    <Badge tone={MODES[mode].tone} title={MODES[mode].detail}>
                        approvals: {MODES[mode].label}
                    </Badge>
                )}

                <span className="flex-1" />
                <span className="hidden sm:inline">Enter to send · Shift+Enter for a new line</span>
            </div>
        </form>
    );
}
