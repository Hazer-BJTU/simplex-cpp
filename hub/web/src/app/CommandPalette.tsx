/**
 * @file the command palette.
 *
 * The old panel had no keyboard path at all: every action was a button, and the
 * only way to reach a session was to click it. The palette is one surface for
 * everything the header and the sidebar can do, reachable with `⌘K`/`Ctrl+K`,
 * which is also what makes the panel usable without a mouse.
 *
 * It is deliberately *not* a second implementation of those actions: each entry
 * calls the same client method the button does, and the entries are built from
 * the same store state the buttons read. A palette that reimplemented them
 * would be a second place for them to be wrong.
 *
 * The filtering is a plain subsequence match. Fuzzy scoring is a nice thing and
 * not this stage's job; a list of twenty entries filtered by substring is
 * faster to read than one sorted by a score nobody can predict.
 */
import { Command } from 'lucide-react';
import { useEffect, useMemo, useState } from 'react';
import type { SessionDescription } from '../../../shared/protocol.ts';
import { usePanel, useView } from '../state/usePanel.ts';
import { Badge } from '../ui/Button.tsx';
import { Dialog, DialogContent } from '../ui/overlays.tsx';
import { buildCommands, filterCommands, type PaletteAction, type PaletteInput } from './palette.ts';
import { useClient } from './ClientContext.tsx';

/** True when a session's worker process is up. */
function isRunning(session: SessionDescription | undefined): boolean {
    const state = session?.process?.state;
    return state === 'running' || state === 'starting';
}

export function CommandPalette() {
    const client = useClient();
    const open = usePanel((state) => state.paletteOpen);
    const setOpen = usePanel((state) => state.setPaletteOpen);
    const togglePalette = usePanel((state) => state.togglePalette);
    const sessions = usePanel((state) => state.sessions);
    const views = usePanel((state) => state.views);
    const confirmMode = usePanel((state) => state.confirmMode);
    const selected = usePanel((state) => state.selected);
    const showDetails = usePanel((state) => state.showDetails);
    const inspectorOpen = usePanel((state) => state.inspectorOpen);
    const setInspectorTab = usePanel((state) => state.setInspectorTab);
    const setInspectorOpen = usePanel((state) => state.setInspectorOpen);
    const toggleDetails = usePanel((state) => state.toggleDetails);
    const pingMs = usePanel((state) => state.pingMs);
    const [query, setQuery] = useState('');
    const [active, setActive] = useState(0);

    // `⌘K` and `Ctrl+K`. Registered on the document because the palette has to
    // be reachable from anywhere, including from inside the composer's
    // textarea — where a keystroke is otherwise the field's.
    useEffect(() => {
        function onKeyDown(event: KeyboardEvent): void {
            if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'k') {
                event.preventDefault();
                togglePalette();
            }
        }
        document.addEventListener('keydown', onKeyDown);
        return () => document.removeEventListener('keydown', onKeyDown);
    }, [togglePalette]);

    // A fresh query each time it opens, so the palette never starts filtered by
    // something typed in a previous visit.
    useEffect(() => {
        if (open) {
            setQuery('');
            setActive(0);
        }
    }, [open]);

    const input: PaletteInput = useMemo(() => ({
        sessions: [...sessions.values()],
        selected,
        running: selected !== null && isRunning(sessions.get(selected)),
        runActive: selected !== null && Boolean(views.get(selected)?.runActive),
        inspectorOpen,
        showDetails,
        confirmMode: selected !== null ? confirmMode.get(selected) ?? 'ask' : 'ask',
        pingMs,
    }), [sessions, views, selected, inspectorOpen, showDetails, confirmMode, pingMs]);

    const commands = useMemo(() => buildCommands(input), [input]);

    /** Perform one entry. The palette describes; this is the only place it acts. */
    function perform(action: PaletteAction): void {
        switch (action.kind) {
            case 'select-session':
                client.select(action.session);
                return;
            case 'worker':
                void client.workerAction(action.session, action.action);
                return;
            case 'signal':
                client.sendSignal(action.session, action.operation);
                return;
            case 'reload-transcript':
                client.reloadTranscript(action.session);
                return;
            case 'inspector':
                if (action.tab) setInspectorTab(action.tab);
                setInspectorOpen(action.open);
                return;
            case 'toggle-details':
                toggleDetails();
                return;
            case 'ping':
                client.ping();
                return;
        }
    }

    const filtered = useMemo(() => filterCommands(commands, query), [commands, query]);

    if (!open) return null;

    const highlighted = Math.min(active, Math.max(0, filtered.length - 1));

    return (
        <Dialog open onOpenChange={(next) => { if (!next) setOpen(false); }}>
            <DialogContent focus="none" title="Commands" description="⌘K or Ctrl+K to toggle">
                <input
                    autoFocus
                    value={query}
                    onChange={(event) => { setQuery(event.target.value); setActive(0); }}
                    onKeyDown={(event) => {
                        if (event.key === 'ArrowDown') {
                            event.preventDefault();
                            setActive((current) => Math.min(current + 1, filtered.length - 1));
                        } else if (event.key === 'ArrowUp') {
                            event.preventDefault();
                            setActive((current) => Math.max(current - 1, 0));
                        } else if (event.key === 'Enter') {
                            event.preventDefault();
                            const command = filtered[highlighted];
                            if (!command) return;
                            setOpen(false);
                            perform(command.action);
                        }
                    }}
                    aria-label="filter commands"
                    placeholder="Type to filter…"
                    className="w-full rounded border border-slate-300 px-2 py-1.5 text-sm
                        focus:border-slate-500 focus:outline-none"
                />

                <ul role="listbox" aria-label="commands" className="mt-2 max-h-80 overflow-y-auto">
                    {filtered.length === 0 && (
                        <li className="px-2 py-3 text-center text-xs text-slate-500">
                            Nothing matches “{query}”.
                        </li>
                    )}
                    {filtered.map((command, position) => (
                        <li key={command.id}>
                            <button
                                type="button"
                                role="option"
                                aria-selected={position === highlighted}
                                data-testid="palette-command"
                                onMouseEnter={() => setActive(position)}
                                onClick={() => { setOpen(false); perform(command.action); }}
                                className={`flex w-full items-center gap-2 rounded px-2 py-1 text-left
                                    text-xs ${position === highlighted ? 'bg-slate-100' : ''}`}
                            >
                                <span className="text-slate-700">{command.label}</span>
                                <span className="flex-1" />
                                {command.hint && (
                                    <span className="font-mono text-[10px] text-slate-400">
                                        {command.hint}
                                    </span>
                                )}
                                <Badge tone="neutral">{command.group}</Badge>
                            </button>
                        </li>
                    ))}
                </ul>

                <p className="mt-2 flex items-center gap-1 border-t border-slate-100 pt-2 text-[11px]
                    text-slate-400">
                    <Command aria-hidden className="h-3 w-3" />
                    ↑↓ to move · Enter to run · Esc to close
                </p>
            </DialogContent>
        </Dialog>
    );
}
