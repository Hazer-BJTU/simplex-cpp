/**
 * @file the top bar: what the panel is connected to, and whether it is well.
 *
 * The old panel put a coloured dot with a `title` attribute in the sidebar and
 * a separate badge elsewhere, so "is this connected" had to be inferred from
 * two places. Here the connection is one sentence, and anything the operator
 * needs to act on — a rejected token, a version mismatch, a refused action —
 * appears directly under it rather than as a toast that expires.
 */
import type { ReactNode } from 'react';
import { usePanel, useVisiblePanel } from '../state/usePanel.ts';
import { viewProjection } from '../state/viewProjection.ts';
import { IconButton } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { Status, type StatusTone } from '../ui/Status.tsx';
import { ThemeToggle } from '../ui/ThemeToggle.tsx';
import { Tooltip } from '../ui/overlays.tsx';
import { useClient } from './ClientContext.tsx';

/** What the socket state means, in words rather than a colour. */
function describeConnection(state: string, attempt: number, nextDelayMs: number | null): {
    tone: StatusTone;
    label: string;
    /** What fits on a phone, where the full sentence would wrap the bar. */
    short: string;
    pulse: boolean;
} {
    switch (state) {
        case 'open':
            return { tone: 'ok', label: 'connected', short: 'connected', pulse: false };
        case 'connecting':
            return { tone: 'idle', label: 'connecting…', short: 'connecting…', pulse: true };
        case 'reconnecting':
            return {
                tone: 'warn',
                label: `reconnecting (attempt ${attempt}${nextDelayMs ? `, in ${Math.round(nextDelayMs / 1000)}s` : ''})`,
                short: 'reconnecting',
                pulse: true,
            };
        case 'rejected':
            return {
                tone: 'bad',
                label: 'the hub refused this connection',
                short: 'refused',
                pulse: false,
            };
        case 'closed':
            return { tone: 'bad', label: 'disconnected', short: 'disconnected', pulse: false };
        default:
            return { tone: 'idle', label: 'not connected', short: 'not connected', pulse: false };
    }
}

export function StatusBar({ onOpenSessions, sessionsCollapsed, onToggleSessions }: {
    /** Opens the session drawer, which below `md` is the only way to it. */
    onOpenSessions: () => void;
    /** Whether the session column above `md` is collapsed. */
    sessionsCollapsed: boolean;
    /** Shows or hides that column; the drawer button owns the narrow layout. */
    onToggleSessions: () => void;
}) {
    const client = useClient();
    const connection = usePanel((state) => state.connection);
    const notice = usePanel((state) => state.notice);
    const authRequired = usePanel((state) => state.authRequired);
    // Actions, not state: Zustand keeps these identities stable, so reading
    // them here never causes a re-render.
    const clearRefusal = usePanel((state) => state.clearRefusal);
    const dismissNotice = usePanel((state) => state.dismissNotice);

    const described = describeConnection(
        connection.state, connection.attempt, connection.nextDelayMs,
    );
    // An approval is the one message that fails if it is missed, so its count
    // belongs in the one place that is always on screen. The block itself is
    // where the decision is taken; this is only the reminder that it is there.
    const pendingApprovals = useVisiblePanel(true, state => viewProjection(state.views).prompts.length);

    return (
        <header className="border-b border-line bg-surface">
            {/* Two groups rather than one row: the status cluster may wrap onto
                a second line when a long connection message and a pending
                approval compete for a phone's width, while the theme control
                keeps its size and its place on the right. */}
            <div className="flex items-center gap-2 px-3 py-2 sm:gap-3 sm:px-4">
                <div className="flex min-w-0 flex-1 flex-wrap items-center gap-x-2 gap-y-1 sm:gap-x-3">
                    <span className="hidden md:inline-flex">
                        <Tooltip label={sessionsCollapsed ? 'Show the session list' : 'Hide the session list'}>
                            <IconButton
                                label={sessionsCollapsed ? 'Show the session list' : 'Hide the session list'}
                                data-testid="sessions-desktop-toggle"
                                aria-expanded={!sessionsCollapsed}
                                aria-controls="session-list"
                                onClick={onToggleSessions}
                            >
                                <Glyph name="sessions" />
                            </IconButton>
                        </Tooltip>
                    </span>
                    <Tooltip label="Sessions">
                        <IconButton
                            label="Open the session list"
                            data-testid="sessions-toggle"
                            onClick={onOpenSessions}
                            className="md:hidden"
                        >
                            <Glyph name="sessions" />
                        </IconButton>
                    </Tooltip>
                    <h1 className="hidden text-sm font-semibold text-ink sm:inline">Simplex</h1>
                    <Status tone={described.tone} pulse={described.pulse}>
                        {described.short === described.label
                            ? described.label
                            : <>
                                <span className="sm:hidden" title={described.label}>{described.short}</span>
                                <span className="hidden sm:inline">{described.label}</span>
                            </>}
                    </Status>
                    {pendingApprovals > 0 && (
                        <Status tone="warn">
                            {/* The count is the cue; "waiting" is what it means,
                                and on a phone the word costs a line of the bar. */}
                            <span
                                className="sm:hidden"
                                title={`${pendingApprovals} approval${pendingApprovals === 1 ? '' : 's'} waiting for a decision`}
                            >
                                {pendingApprovals} approval{pendingApprovals === 1 ? '' : 's'}
                            </span>
                            <span className="hidden sm:inline">
                                {pendingApprovals} approval{pendingApprovals === 1 ? '' : 's'} waiting
                            </span>
                        </Status>
                    )}
                    {connection.ignoredFrames > 0 && (
                        <span className="hidden text-xs text-ink-muted sm:inline">
                            {connection.ignoredFrames} message(s) from a newer hub were ignored
                        </span>
                    )}
                </div>
                <ThemeToggle />
            </div>

            {authRequired && (
                <TokenPrompt onSubmit={(value) => { void client.setToken(value); }} />
            )}

            {connection.refusal && (
                <Banner tone="bad" onDismiss={clearRefusal}>
                    <strong className="font-semibold">{connection.refusal.code}</strong>
                    {' — '}
                    {connection.refusal.detail}
                </Banner>
            )}

            {notice && (
                <Banner tone={notice.tone === 'error' ? 'bad' : 'warn'} onDismiss={dismissNotice}>
                    <strong className="font-semibold">{notice.code}</strong>
                    {' — '}
                    {notice.text}
                </Banner>
            )}
        </header>
    );
}

/** One dismissible line. */
function Banner({ tone, onDismiss, children }: {
    tone: 'bad' | 'warn';
    onDismiss: () => void;
    children: ReactNode;
}) {
    const tones = {
        bad: 'bg-danger-soft text-danger border-danger-line',
        warn: 'bg-warn-soft text-warn border-warn-line',
    } as const;
    return (
        <div className={`flex animate-enter items-start gap-2 border-t px-4 py-2 text-xs
            ${tones[tone]}`} role="alert">
            <Glyph name={tone === 'bad' ? 'error' : 'warning'} className="mt-0.5" />
            <span className="flex-1">{children}</span>
            <button
                type="button"
                onClick={onDismiss}
                aria-label="dismiss this message"
                className="rounded px-1 font-medium underline-offset-2 hover:underline
                    focus-visible:outline-2 focus-visible:outline-offset-1"
            >
                dismiss
            </button>
        </div>
    );
}

/** Ask for a token, once the hub has said one is required. */
function TokenPrompt({ onSubmit }: { onSubmit: (value: string) => void }) {
    return (
        <form
            className="flex items-center gap-2 border-t border-line bg-sunken px-4 py-2"
            onSubmit={(event) => {
                event.preventDefault();
                const field = event.currentTarget.elements.namedItem('token');
                if (!(field instanceof HTMLInputElement)) return;
                const value = field.value.trim();
                if (!value) return;
                field.value = '';
                onSubmit(value);
            }}
        >
            <label className="text-xs font-medium text-ink" htmlFor="panel-token">
                panel token
            </label>
            <input
                id="panel-token"
                name="token"
                type="password"
                autoComplete="off"
                className="w-64 rounded border border-line-strong px-2 py-1 font-mono text-xs
                    focus:border-line-strong focus:outline-none"
                placeholder="paste the token from the hub's output"
            />
            <button
                type="submit"
                className="rounded bg-accent px-2.5 py-1 text-xs font-medium text-accent-ink
                    hover:bg-accent-hover"
            >
                use token
            </button>
            <span className="text-xs text-ink-muted">
                The hub prints one on startup when it is not bound to loopback only.
            </span>
        </form>
    );
}
