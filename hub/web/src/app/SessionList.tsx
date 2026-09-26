/**
 * @file the session list.
 *
 * Replaces a flat list of rows that showed a coloured dot with a `title`
 * attribute as their only explanation of state. Everything here is spelled out,
 * because the operator's first question — "which of these needs me?" — should
 * not require hovering over eight dots.
 *
 * The counts come from the session's own view. A session the panel has never
 * subscribed to has no view and so shows no counts, which is honest: the panel
 * does not know how many events it has not been told about.
 *
 * Below `md` this column becomes a drawer. It is one element with responsive
 * classes rather than two renderings of the list, because a second copy is a
 * second place for the rows to drift apart.
 */
import { useState, type FormEvent } from 'react';
import type { SessionDescription } from '../../../shared/protocol.ts';
import { usePanel, useView } from '../state/usePanel.ts';
import { statsOf } from '../state/view.ts';
import { IconButton } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Tooltip } from '../ui/overlays.tsx';
import { useClient } from './ClientContext.tsx';

/** What a session's worker process is doing. */
function describeProcess(session: SessionDescription): { tone: string; label: string } {
    const process = session.process;
    if (!process) return { tone: 'text-ink-faint', label: 'no process' };
    switch (process.state) {
        case 'running':
            return {
                tone: 'text-ok',
                label: process.pid ? `running · pid ${process.pid}` : 'running',
            };
        case 'starting':
            return { tone: 'text-warn', label: 'starting' };
        case 'stopping':
            return { tone: 'text-warn', label: 'stopping' };
        case 'exited':
            return {
                tone: 'text-ink-muted',
                label: `exited${process.exit_code === null ? '' : ` (${process.exit_code})`}`,
            };
        case 'failed':
            return {
                tone: 'text-danger',
                label: `failed${process.error ? `: ${process.error}` : ''}`,
            };
        default:
            return { tone: 'text-ink-muted', label: process.state };
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
                        ? 'bg-accent text-accent-ink'
                        : 'hover:bg-subtle focus-visible:bg-subtle'}`}
            >
                <span className="flex items-center gap-2">
                    <span className="truncate font-mono text-sm font-medium">
                        {session.session_id}
                    </span>
                    {pending > 0 && (
                        <span
                            data-testid="pending-approval"
                            className="inline-flex shrink-0 items-center gap-0.5 rounded-full
                                bg-warn-soft px-1.5 text-xs font-semibold text-warn ring-1
                                ring-inset ring-warn-line"
                            title={`${pending} tool call(s) waiting for a decision`}
                        >
                            <Glyph name="approval" size="sm" />
                            {pending}
                        </span>
                    )}
                    <span className="flex-1" />
                    <span className={`shrink-0 text-xs
                        ${selected ? 'text-accent-ink' : 'text-ink-faint'}`}>
                        {session.connected ? 'worker attached' : 'worker away'}
                    </span>
                </span>
                <span className={`mt-0.5 block truncate text-xs
                    ${selected ? 'text-accent-ink' : process.tone}`}>
                    {process.label}
                </span>
                {stats.items > 0 && (
                    <span className={`mt-0.5 block text-xs
                        ${selected ? 'text-accent-ink' : 'text-ink-faint'}`}>
                        {stats.items} item{stats.items === 1 ? '' : 's'}
                        {stats.gaps > 0 ? ` · ${stats.gaps} gap${stats.gaps === 1 ? '' : 's'}` : ''}
                        {stats.unknownRequests > 0 ? ` · ${stats.unknownRequests} unknown` : ''}
                    </span>
                )}
            </button>
        </li>
    );
}

export function SessionList({ open, onClose }: {
    /** Below `md`, whether the drawer is showing. Ignored on a wide screen. */
    open: boolean;
    onClose: () => void;
}) {
    const client = useClient();
    const sessions = usePanel((state) => state.sessions);
    const selected = usePanel((state) => state.selected);
    const [creating, setCreating] = useState(false);
    const [error, setError] = useState('');

    const ordered = [...sessions.values()].sort((a, b) => (
        (b.created_at ?? '').localeCompare(a.created_at ?? '')
        || a.session_id.localeCompare(b.session_id)
    ));

    async function create(event: FormEvent<HTMLFormElement>): Promise<void> {
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
        onClose();
        client.select(id);
    }

    return (
        <>
            {/* The backdrop exists only while the drawer does, and only below
                `md`: on a wide screen the sidebar is a column and there is
                nothing to dismiss. */}
            {open && (
                <button
                    type="button"
                    aria-label="close the session list"
                    onClick={onClose}
                    className="fixed inset-0 z-30 animate-fade bg-scrim md:hidden"
                />
            )}
            <aside
                aria-label="sessions"
                data-testid="session-list"
                data-open={open ? 'true' : 'false'}
                className={`${open ? 'flex' : 'hidden'} fixed inset-y-0 left-0 z-40 w-72
                    animate-drawer flex-col border-r border-line bg-sunken md:static md:z-auto
                    md:flex md:w-64 md:animate-none`}
            >
                <div className="flex items-center gap-1 px-2 py-2">
                    <h2 className="px-1 text-xs font-semibold uppercase tracking-wide text-ink-muted">
                        sessions
                    </h2>
                    <span className="flex-1" />
                    <Tooltip label="Ask the hub for the session list again">
                        <IconButton
                            label="Refresh the session list"
                            onClick={() => client.refreshSessions()}
                        >
                            <Glyph name="refresh" />
                        </IconButton>
                    </Tooltip>
                    <Tooltip label={creating ? 'Cancel' : 'Create a session'}>
                        <IconButton
                            label={creating ? 'Cancel creating a session' : 'Create a session'}
                            onClick={() => { setCreating((value) => !value); setError(''); }}
                        >
                            <Glyph name={creating ? 'close' : 'new-session'} />
                        </IconButton>
                    </Tooltip>
                    <IconButton
                        label="Close the session list"
                        onClick={onClose}
                        className="md:hidden"
                    >
                        <Glyph name="close" />
                    </IconButton>
                </div>

                {creating && (
                    <form
                        onSubmit={(event) => { void create(event); }}
                        className="animate-enter px-3 pb-2"
                    >
                        <input
                            name="session"
                            autoFocus
                            aria-label="new session id"
                            placeholder="session-id"
                            pattern="[A-Za-z0-9_-]{1,128}"
                            title="1-128 characters of letters, digits, underscore or dash"
                            className="w-full rounded border border-line-strong bg-surface px-2 py-1
                                font-mono text-xs focus-visible:border-interactive"
                        />
                        <button
                            type="submit"
                            className="mt-1 w-full rounded bg-accent px-2 py-1 text-xs font-medium
                                text-accent-ink transition-colors hover:bg-accent-hover
                                focus-visible:outline-2 focus-visible:outline-offset-1
                                focus-visible:outline-interactive"
                        >
                            create
                        </button>
                    </form>
                )}

                {error && <p role="alert" className="px-3 pb-2 text-xs text-danger">{error}</p>}

                {ordered.length === 0 ? (
                    <div className="px-4 py-6 text-center" data-testid="sessions-empty">
                        <Glyph name="empty-list" size="lg" className="mx-auto text-ink-faint" />
                        <p className="mt-2 text-sm font-medium text-ink">No sessions yet</p>
                        <p className="mt-1 text-xs text-ink-muted">
                            A session is one worker and its transcript. Create one, then start its
                            worker to talk to it.
                        </p>
                        <button
                            type="button"
                            onClick={() => setCreating(true)}
                            className="mt-3 rounded bg-accent px-2.5 py-1 text-xs font-medium
                                text-accent-ink transition-colors hover:bg-accent-hover
                                focus-visible:outline-2 focus-visible:outline-offset-1
                                focus-visible:outline-interactive"
                        >
                            Create a session
                        </button>
                    </div>
                ) : (
                    <ul className="flex-1 space-y-1 overflow-y-auto px-2 pb-2">
                        {ordered.map((session) => (
                            <SessionRow
                                key={session.session_id}
                                session={session}
                                selected={session.session_id === selected}
                                onSelect={() => { onClose(); client.select(session.session_id); }}
                            />
                        ))}
                    </ul>
                )}

                <p className="border-t border-line px-3 py-2 text-xs text-ink-faint">
                    The hub has no delivery acknowledgement: <em>sent</em> is not <em>executed</em>.
                </p>
            </aside>
        </>
    );
}
