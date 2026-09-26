/**
 * @file the composer.
 *
 * Scaffolding for this stage: a textarea and a send button, no
 * `external_ref` parts, no reference chips, no per-session confirmation mode.
 * The later stage rebuilds it properly.
 *
 * Two behaviours are already the corrected ones:
 *
 * - **A refused input comes back** (defect D19). The old panel cleared the
 *   box the moment `send()` returned true — which only means the frame left,
 *   not that the worker took it — and the text was gone by the time the hub
 *   answered `input_not_sent`. Here the store hands the text back and this
 *   component puts it in the box.
 * - **Enter sends, Shift+Enter breaks the line.** The old panel had a Send
 *   button and a textarea bound to nothing.
 */
import { useEffect, useRef, useState } from 'react';
import { usePanel } from '../state/usePanel.ts';
import { useClient } from './ClientContext.tsx';

export function Composer() {
    const client = useClient();
    const selected = usePanel((state) => state.selected);
    const failed = usePanel((state) => state.failedInput);
    const clearFailedInput = usePanel((state) => state.clearFailedInput);
    const connectionState = usePanel((state) => state.connection.state);
    const connected = usePanel((state) => (
        state.selected ? state.sessions.get(state.selected)?.connected ?? false : false
    ));
    const runActive = usePanel((state) => (
        state.selected ? Boolean(state.views.get(state.selected)?.runActive) : false
    ));

    const [draft, setDraft] = useState('');
    const box = useRef<HTMLTextAreaElement>(null);

    // The store's refunded draft. Keyed by `at` so two identical failures both
    // restore the text, and cleared so a re-render does not restore it again.
    useEffect(() => {
        if (!failed || failed.sessionId !== selected) return;
        setDraft(failed.parts.map((part) => part.raw).join('\n\n'));
        clearFailedInput();
        box.current?.focus();
    }, [failed, selected, clearFailedInput]);

    // Switching sessions must not carry one session's draft into another.
    useEffect(() => {
        setDraft('');
    }, [selected]);

    if (!selected) return null;

    const canSend = draft.trim().length > 0;

    function send() {
        if (!selected || !canSend) return;
        const parts = [{ type: 'text' as const, raw: draft }];
        const sent = client.sendInput(selected, parts);
        // Clear only when the frame actually left; otherwise the store has
        // already handed the text back and clearing here would lose it anyway.
        if (sent) setDraft('');
    }

    return (
        <form
            className="border-t border-slate-200 bg-white px-4 py-2"
            onSubmit={(event) => {
                event.preventDefault();
                send();
            }}
        >
            <div className="flex items-end gap-2">
                <textarea
                    ref={box}
                    value={draft}
                    onChange={(event) => setDraft(event.target.value)}
                    onKeyDown={(event) => {
                        if (event.key === 'Enter' && !event.shiftKey) {
                            event.preventDefault();
                            send();
                        }
                    }}
                    rows={2}
                    aria-label="message"
                    placeholder={connected
                        ? 'Message the worker — Enter sends, Shift+Enter breaks the line'
                        : 'No worker is attached to this session'}
                    className="min-h-[2.5rem] flex-1 resize-y rounded border border-slate-300
                        px-2 py-1.5 text-sm focus:border-slate-500 focus:outline-none"
                />
                <button
                    type="submit"
                    disabled={!canSend}
                    className="rounded bg-slate-900 px-3 py-2 text-sm font-medium text-white
                        hover:bg-slate-700 disabled:cursor-not-allowed disabled:opacity-40"
                >
                    Send
                </button>
            </div>

            <p className="mt-1 flex items-center gap-2 text-[11px] text-slate-500">
                {connectionState !== 'open' && (
                    <span className="text-amber-700">the panel is not connected</span>
                )}
                {runActive && <span className="text-sky-700">a run is active</span>}
                {!connected && <span>no worker attached</span>}
                <span className="flex-1" />
                <span>Enter sends · Shift+Enter breaks the line</span>
            </p>
        </form>
    );
}
