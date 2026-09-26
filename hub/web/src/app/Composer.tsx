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
 * Here the mode belongs to a session, lives behind a `⚙` popover, and is shown
 * as a permanent badge whenever it is not the default — because a setting that
 * turns approvals off should not be discoverable only by opening the thing that
 * sets it.
 *
 * The other two carried-over behaviours: the draft survives a refused send
 * (defect D19, fixed in the store), and Enter sends while Shift+Enter breaks the
 * line.
 */
import { Paperclip, Send, Settings2, X } from 'lucide-react';
import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import type { ContentPart, PayloadOptions } from '../../../shared/protocol.ts';
import { usePanel, useSession, useView } from '../state/usePanel.ts';
import { Badge, Button, IconButton } from '../ui/Button.tsx';
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

    // The store's refunded draft. Keyed by `at` so two identical failures both
    // restore the text, and cleared so a re-render does not restore it again.
    useEffect(() => {
        if (!failed || failed.sessionId !== selected) return;
        setDraft(failed.parts.map((part) => part.raw).join('\n\n'));
        clearFailedInput();
        box.current?.focus();
    }, [failed, selected, clearFailedInput]);

    // Switching sessions must not carry one session's draft, or its references,
    // into another.
    useEffect(() => {
        setDraft('');
        setReferences([]);
        setRefOpen(false);
    }, [selected]);

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
        if (!canSend) return;
        // The mode travels with the payload. The worker freezes the policy per
        // run, so sending it every time is what makes a change take effect on
        // the next run rather than the next restart — and what keeps one
        // session's choice out of another's.
        const options: PayloadOptions = { confirmation: { mode } };
        const sent = client.sendInput(sessionId, parts(), operation, options);
        // Clear only when the frame actually left; otherwise the store has
        // already handed the text back and clearing here would lose it.
        if (sent) {
            setDraft('');
            setReferences([]);
        }
    }

    return (
        <form
            className="border-t border-slate-200 bg-white px-4 py-2"
            onSubmit={(event) => {
                event.preventDefault();
                send();
            }}
        >
            {references.length > 0 && (
                <ul className="mb-1 flex flex-wrap gap-1">
                    {references.map((reference) => (
                        <li
                            key={reference.raw}
                            className="flex items-center gap-1 rounded-full bg-slate-100 py-0.5 pl-2 pr-1
                                text-[11px] text-slate-600"
                        >
                            <span className="max-w-72 truncate font-mono">{reference.raw}</span>
                            <button
                                type="button"
                                aria-label={`remove reference ${reference.raw}`}
                                className="rounded-full p-0.5 hover:bg-slate-200"
                                onClick={() => setReferences((current) => (
                                    current.filter((item) => item.raw !== reference.raw)
                                ))}
                            >
                                <X aria-hidden className="h-3 w-3" />
                            </button>
                        </li>
                    ))}
                </ul>
            )}

            <div className="flex items-end gap-2">
                <Popover open={refOpen} onOpenChange={setRefOpen}>
                    <PopoverTrigger asChild>
                        <IconButton label="Attach a reference" disabled={!connected}>
                            <Paperclip aria-hidden className="h-4 w-4" />
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
                                // Attached as an external reference, never
                                // fetched: the protocol says a reference is
                                // data, and a panel that loaded it would turn
                                // the operator's paste into a request they did
                                // not make.
                                setReferences((current) => (
                                    current.some((item) => item.raw === value)
                                        ? current
                                        : [...current, { kind: 'external_ref', raw: value }]
                                ));
                                field.value = '';
                                setRefOpen(false);
                            }}
                        >
                            <label className="block text-[11px] font-medium text-slate-600"
                                htmlFor="composer-reference">
                                external reference
                            </label>
                            <input
                                id="composer-reference"
                                name="reference"
                                autoFocus
                                placeholder="https://…"
                                className="mt-1 w-full rounded border border-slate-300 px-2 py-1
                                    font-mono text-xs focus:border-slate-500 focus:outline-none"
                            />
                            <p className="mt-1 text-[11px] text-slate-500">
                                Sent to the worker as an <code>external_ref</code> part. The panel
                                never fetches it.
                            </p>
                            <div className="mt-2 flex justify-end">
                                <Button type="submit" variant="primary">Attach</Button>
                            </div>
                        </form>
                    </PopoverContent>
                </Popover>

                <textarea
                    ref={box}
                    value={draft}
                    rows={1}
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
                    placeholder={connected
                        ? 'Message the worker — Enter sends, Shift+Enter breaks the line'
                        : 'No worker is attached to this session'}
                    className="min-h-[2.25rem] flex-1 resize-none rounded border border-slate-300
                        px-2 py-1.5 text-sm focus:border-slate-500 focus:outline-none"
                />

                {runActive && (
                    <Button
                        onClick={() => send('continue')}
                        title="ask the worker to continue the run without a new message"
                    >
                        Continue
                    </Button>
                )}
                <Button
                    type="submit"
                    variant="primary"
                    size="md"
                    disabled={!canSend}
                    icon={<Send aria-hidden className="h-3.5 w-3.5" />}
                >
                    Send
                </Button>
            </div>

            <div className="mt-1 flex flex-wrap items-center gap-2 text-[11px] text-slate-500">
                {connectionState !== 'open' && (
                    <span className="text-amber-700">the panel is not connected</span>
                )}
                {!connected && <span>no worker attached</span>}
                {runActive && <span className="text-sky-700">a run is active</span>}

                <Popover>
                    <PopoverTrigger asChild>
                        <button
                            type="button"
                            className="inline-flex items-center gap-1 rounded px-1 hover:bg-slate-100"
                            title="how the worker should answer tool confirmations for this session"
                        >
                            <Settings2 aria-hidden className="h-3 w-3" />
                            confirmation
                        </button>
                    </PopoverTrigger>
                    <PopoverContent align="start">
                        <p className="text-[11px] font-medium text-slate-700">
                            Confirmation mode for <span className="font-mono">{sessionId}</span>
                        </p>
                        <div className="mt-2 space-y-1">
                            {(['ask', 'approve', 'deny'] as const).map((value) => (
                                <label
                                    key={value}
                                    className="flex cursor-pointer items-start gap-2 rounded p-1
                                        hover:bg-slate-50"
                                >
                                    <input
                                        type="radio"
                                        name="confirm-mode"
                                        value={value}
                                        checked={mode === value}
                                        onChange={() => setConfirmMode(sessionId, value)}
                                        className="mt-0.5 accent-slate-700"
                                    />
                                    <span>
                                        <span className="font-mono text-xs text-slate-800">{value}</span>
                                        <span className="block text-[11px] text-slate-500">
                                            {MODES[value].detail}
                                        </span>
                                    </span>
                                </label>
                            ))}
                        </div>
                        <p className="mt-2 border-t border-slate-100 pt-2 text-[11px] text-slate-500">
                            Sent with every message. The worker freezes the policy for a run before
                            it starts, so a change applies to the next run. This is stored per
                            session — the old panel kept one control for all of them.
                        </p>
                    </PopoverContent>
                </Popover>

                {/* A mode that turns approvals off is shown, not hidden behind
                    the control that set it. */}
                {mode !== 'ask' && (
                    <Badge tone={MODES[mode].tone} title={MODES[mode].detail}>
                        approvals: {MODES[mode].label}
                    </Badge>
                )}

                <span className="flex-1" />
                <span>Enter sends · Shift+Enter breaks the line · Esc clears</span>
            </div>
        </form>
    );
}
