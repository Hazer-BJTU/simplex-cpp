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
import { usePanel } from '../state/usePanel.ts';
import { IconButton } from '../ui/Button.tsx';
import { Glyph } from '../ui/icons.tsx';
import { ThemeToggle } from '../ui/ThemeToggle.tsx';
import { Tooltip } from '../ui/overlays.tsx';
import { useClient } from './ClientContext.tsx';

/**
 * One status pill.
 *
 * The tone is carried by an icon and by the words as well as by the colour: an
 * operator who cannot tell amber from green still reads "reconnecting", and the
 * icon is the same one the rest of the panel uses for that meaning.
 */
function Pill({ tone, icon, children }: {
    tone: 'ok' | 'warn' | 'bad' | 'idle';
    icon: 'online' | 'offline' | 'warning' | 'info';
    children: ReactNode;
}) {
    const tones = {
        ok: 'bg-ok-soft text-ok ring-ok-line',
        warn: 'bg-warn-soft text-warn ring-warn-line',
        bad: 'bg-danger-soft text-danger ring-danger-line',
        idle: 'bg-subtle text-ink-muted ring-line',
    } as const;
    return (
        <span className={`inline-flex shrink-0 items-center gap-1 rounded-full px-2 py-0.5
            text-xs font-medium ring-1 ring-inset ${tones[tone]}`}>
            <Glyph name={icon} size="sm" />
            {children}
        </span>
    );
}

/** What the socket state means, in words rather than a colour. */
function describeConnection(state: string, attempt: number, nextDelayMs: number | null): {
    tone: 'ok' | 'warn' | 'bad' | 'idle';
    icon: 'online' | 'offline' | 'warning' | 'info';
    label: string;
} {
    switch (state) {
        case 'open':
            return { tone: 'ok', icon: 'online', label: 'connected' };
        case 'connecting':
            return { tone: 'idle', icon: 'info', label: 'connecting…' };
        case 'reconnecting':
            return {
                tone: 'warn',
                icon: 'warning',
                label: `reconnecting (attempt ${attempt}${nextDelayMs ? `, in ${Math.round(nextDelayMs / 1000)}s` : ''})`,
            };
        case 'rejected':
            return { tone: 'bad', icon: 'offline', label: 'the hub refused this connection' };
        case 'closed':
            return { tone: 'bad', icon: 'offline', label: 'disconnected' };
        default:
            return { tone: 'idle', icon: 'offline', label: 'not connected' };
    }
}

export function StatusBar({ onOpenSessions }: {
    /** Opens the session drawer, which below `md` is the only way to it. */
    onOpenSessions: () => void;
}) {
    const client = useClient();
    const connection = usePanel((state) => state.connection);
    const notice = usePanel((state) => state.notice);
    const hub = usePanel((state) => state.hub);
    const epoch = usePanel((state) => state.epoch);
    const authRequired = usePanel((state) => state.authRequired);
    // Actions, not state: Zustand keeps these identities stable, so reading
    // them here never causes a re-render.
    const clearRefusal = usePanel((state) => state.clearRefusal);
    const dismissNotice = usePanel((state) => state.dismissNotice);
    const showDetails = usePanel((state) => state.showDetails);
    const toggleDetails = usePanel((state) => state.toggleDetails);

    const described = describeConnection(
        connection.state, connection.attempt, connection.nextDelayMs,
    );

    return (
        <header className="border-b border-line bg-surface">
            <div className="flex items-center gap-2 px-3 py-2 sm:gap-3 sm:px-4">
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
                <h1 className="hidden text-sm font-semibold text-ink sm:inline">simplex hub</h1>
                <Pill tone={described.tone} icon={described.icon}>{described.label}</Pill>
                {hub && (
                    <span className="hidden truncate text-xs text-ink-muted lg:inline">
                        {hub.name} {hub.version} · panel protocol v{hub.protocol.version}
                        {epoch ? ` · transcript ${epoch.slice(0, 8)}` : ''}
                    </span>
                )}
                <span className="flex-1" />
                {connection.ignoredFrames > 0 && (
                    <span className="hidden text-xs text-ink-muted sm:inline">
                        {connection.ignoredFrames} message(s) from a newer hub were ignored
                    </span>
                )}
                <label
                    className="flex cursor-pointer select-none items-center gap-1.5 text-xs text-ink-muted"
                    title="show the protocol's own events, in the order they arrived"
                >
                    <input
                        type="checkbox"
                        data-testid="details-toggle"
                        checked={showDetails}
                        onChange={(event) => toggleDetails()}
                        className="h-3.5 w-3.5 accent-interactive"
                    />
                    <span className="hidden sm:inline">technical details</span>
                </label>
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
