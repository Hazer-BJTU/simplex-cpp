/**
 * @file the session list.
 *
 * Replaces a flat list of rows that showed a coloured dot with a `title`
 * attribute as their only explanation of state. Everything here is spelled out,
 * because the operator's first question — "which of these needs me?" — should
 * not require hovering over eight dots.
 *
 * The counts come from `stats`, which reads each session's view. A session the
 * panel has never subscribed to has no view and so shows no counts, which is
 * honest: the panel does not know how many events it has not been told about.
 */
import { useState, type FormEvent } from 'react';
import type { SessionDescription } from '../../../shared/protocol.ts';
import { usePanel, useView } from '../state/usePanel.ts';
import { statsOf } from '../state/view.ts';
import { useClient } from './ClientContext.tsx';

/** What a session's worker process is doing. */
function describeProcess(session: SessionDescription): { tone: string; label: string } {
    const process = session.process;
    if (!process) return { tone: 'text-slate-400', label: 'no process' };
    switch (process.state) {
        case 'running':
            return { tone: 'text-emerald-600', label: process.pid ? `running · pid ${process.pid}` : 'running' };
        case 'starting':
            return { tone: 'text-amber-600', label: 'starting' };
        case 'stopping':
            return { tone: 'text-amber-600', label: 'stopping' };
        case 'exited':
            return { tone: 'text-slate-500', label: `exited${process.exit_code === null ? '' : ` (${process.exit_code})`}` };
        case 'failed':
            return { tone: 'text-rose-600', label: `failed${process.error ? `: ${process.error}` : ''}` };
        default:
            return { tone: 'text-slate-500', label: process.state };
    }
}

/** One row. */
function SessionRow({ session, selected, onSelect }: {
    session: SessionDescription;
    selected: boolean;
    onSelect: () => void;
}) {
    // The view's own identity changes exactly when its counters do, so this is
    // both correct and cheap — where a `stats()` selector would be a new object
    // on every store change and would re-render forever.
    const stats = statsOf(useView(session.session_id));
    const process = describeProcess(session);
    // An open approval is the one thing that must not go unnoticed: it blocks a
    // tool call until it is answered or its deadline passes.
    const pending = session.confirmations.filter((prompt) => prompt.settled_at === null).length;

    return (
        <li>
            <button
                type="button"
                onClick={onSelect}
                aria-current={selected ? 'true' : undefined}
                data-testid="session-row"
                data-session={session.session_id}
                className={`w-full rounded-md px-2.5 py-2 text-left transition-colors
                    ${selected
                        ? 'bg-slate-900 text-white'
                        : 'hover:bg-slate-100 focus-visible:bg-slate-100'}`}
            >
                <span className="flex items-center gap-2">
                    <span className="truncate font-mono text-[13px] font-medium">
                        {session.session_id}
                    </span>
                    {pending > 0 && (
                        <span
                            data-testid="pending-approval"
                            className="rounded-full bg-amber-400 px-1.5 text-[11px] font-semibold
                                text-amber-950"
                            title={`${pending} tool call(s) waiting for a decision`}
                        >
                            {pending} approval{pending === 1 ? '' : 's'}
                        </span>
                    )}
                    <span className="flex-1" />
                    <span
                        className={`shrink-0 text-[11px] ${selected ? 'text-slate-300' : 'text-slate-500'}`}
                    >
                        {session.connected ? 'worker attached' : 'worker away'}
                    </span>
                </span>
                <span className={`mt-0.5 block truncate text-[11px] ${selected ? 'text-slate-300' : process.tone}`}>
                    {process.label}
                </span>
                {stats.items > 0 && (
                    <span className={`mt-0.5 block text-[11px] ${selected ? 'text-slate-400' : 'text-slate-400'}`}>
                        {stats.items} item{stats.items === 1 ? '' : 's'}
                        {stats.gaps > 0 ? ` · ${stats.gaps} gap${stats.gaps === 1 ? '' : 's'}` : ''}
                        {stats.unknownRequests > 0 ? ` · ${stats.unknownRequests} unknown` : ''}
                    </span>
                )}
            </button>
        </li>
    );
}

export function SessionList() {
    const client = useClient();
    const sessions = usePanel((state) => state.sessions);
    const selected = usePanel((state) => state.selected);
    const [creating, setCreating] = useState(false);
    const [error, setError] = useState('');

    const ordered = [...sessions.values()].sort((a, b) => (
        (b.created_at ?? '').localeCompare(a.created_at ?? '')
        || a.session_id.localeCompare(b.session_id)
    ));

    async function create(event: FormEvent<HTMLFormElement>) {
        event.preventDefault();
        const field = event.currentTarget.elements.namedItem('session');
        if (!(field instanceof HTMLInputElement)) return;
        const id = field.value.trim();
        if (!id) return;
        setError('');
        const ok = await client.createSession(id);
        if (!ok) {
            setError(`could not create "${id}"`);
            return;
        }
        setCreating(false);
        client.select(id);
    }

    return (
        <aside className="flex w-64 shrink-0 flex-col border-r border-slate-200 bg-slate-50">
            <div className="flex items-center gap-2 px-3 py-2">
                <h2 className="text-xs font-semibold uppercase tracking-wide text-slate-500">
                    sessions
                </h2>
                <span className="flex-1" />
                <button
                    type="button"
                    onClick={() => client.refreshSessions()}
                    className="rounded px-1.5 py-0.5 text-xs text-slate-600 hover:bg-slate-200"
                >
                    refresh
                </button>
                <button
                    type="button"
                    onClick={() => { setCreating((open) => !open); setError(''); }}
                    className="rounded px-1.5 py-0.5 text-xs font-medium text-slate-700 hover:bg-slate-200"
                >
                    {creating ? 'cancel' : 'new'}
                </button>
            </div>

            {creating && (
                <form onSubmit={(event) => { void create(event); }} className="px-3 pb-2">
                    <input
                        name="session"
                        autoFocus
                        aria-label="new session id"
                        placeholder="session-id"
                        pattern="[A-Za-z0-9_-]{1,128}"
                        title="1-128 characters of letters, digits, underscore or dash"
                        className="w-full rounded border border-slate-300 px-2 py-1 font-mono
                            text-xs focus:border-slate-500 focus:outline-none"
                    />
                    <button
                        type="submit"
                        className="mt-1 w-full rounded bg-slate-900 px-2 py-1 text-xs
                            font-medium text-white hover:bg-slate-700"
                    >
                        create
                    </button>
                </form>
            )}

            {error && <p className="px-3 pb-2 text-[11px] text-rose-700">{error}</p>}

            {ordered.length === 0 ? (
                <p className="px-3 py-4 text-xs text-slate-500">
                    This hub has no sessions yet. Create one and start its worker.
                </p>
            ) : (
                <ul className="flex-1 space-y-1 overflow-y-auto px-2 pb-2">
                    {ordered.map((session) => (
                        <SessionRow
                            key={session.session_id}
                            session={session}
                            selected={session.session_id === selected}
                            onSelect={() => client.select(session.session_id)}
                        />
                    ))}
                </ul>
            )}
        </aside>
    );
}
