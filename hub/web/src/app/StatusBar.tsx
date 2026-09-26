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
import { useClient } from './ClientContext.tsx';

/** One status pill. */
function Pill({ tone, children }: { tone: 'ok' | 'warn' | 'bad' | 'idle'; children: ReactNode }) {
    const tones = {
        ok: 'bg-emerald-50 text-emerald-700 ring-emerald-600/20',
        warn: 'bg-amber-50 text-amber-800 ring-amber-600/20',
        bad: 'bg-rose-50 text-rose-700 ring-rose-600/20',
        idle: 'bg-slate-100 text-slate-600 ring-slate-500/20',
    } as const;
    return (
        <span className={`inline-flex items-center rounded-full px-2 py-0.5 text-xs
            font-medium ring-1 ring-inset ${tones[tone]}`}>
            {children}
        </span>
    );
}

/** What the socket state means, in words rather than a colour. */
function describeConnection(state: string, attempt: number, nextDelayMs: number | null): {
    tone: 'ok' | 'warn' | 'bad' | 'idle';
    label: string;
} {
    switch (state) {
        case 'open':
            return { tone: 'ok', label: 'connected' };
        case 'connecting':
            return { tone: 'idle', label: 'connecting…' };
        case 'reconnecting':
            return {
                tone: 'warn',
                label: `reconnecting (attempt ${attempt}${nextDelayMs ? `, in ${Math.round(nextDelayMs / 1000)}s` : ''})`,
            };
        case 'rejected':
            return { tone: 'bad', label: 'the hub refused this connection' };
        case 'closed':
            return { tone: 'bad', label: 'disconnected' };
        default:
            return { tone: 'idle', label: 'not connected' };
    }
}

export function StatusBar() {
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

    const described = describeConnection(
        connection.state, connection.attempt, connection.nextDelayMs,
    );

    return (
        <header className="border-b border-slate-200 bg-white">
            <div className="flex items-center gap-3 px-4 py-2">
                <h1 className="text-sm font-semibold text-slate-900">simplex hub</h1>
                <Pill tone={described.tone}>{described.label}</Pill>
                {hub && (
                    <span className="text-xs text-slate-500">
                        {hub.name} {hub.version} · panel protocol v{hub.protocol.version}
                        {epoch ? ` · transcript ${epoch.slice(0, 8)}` : ''}
                    </span>
                )}
                <span className="flex-1" />
                {connection.ignoredFrames > 0 && (
                    <span className="text-xs text-slate-500">
                        {connection.ignoredFrames} message(s) from a newer hub were ignored
                    </span>
                )}
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
        bad: 'bg-rose-50 text-rose-800 border-rose-200',
        warn: 'bg-amber-50 text-amber-900 border-amber-200',
    } as const;
    return (
        <div className={`flex items-start gap-2 border-t px-4 py-2 text-xs ${tones[tone]}`}
            role="alert">
            <span className="flex-1">{children}</span>
            <button
                type="button"
                onClick={onDismiss}
                className="rounded px-1 font-medium underline-offset-2 hover:underline
                    focus:outline-2 focus:outline-offset-1"
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
            className="flex items-center gap-2 border-t border-slate-200 bg-slate-50 px-4 py-2"
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
            <label className="text-xs font-medium text-slate-700" htmlFor="panel-token">
                panel token
            </label>
            <input
                id="panel-token"
                name="token"
                type="password"
                autoComplete="off"
                className="w-64 rounded border border-slate-300 px-2 py-1 font-mono text-xs
                    focus:border-slate-500 focus:outline-none"
                placeholder="paste the token from the hub's output"
            />
            <button
                type="submit"
                className="rounded bg-slate-900 px-2.5 py-1 text-xs font-medium text-white
                    hover:bg-slate-700"
            >
                use token
            </button>
            <span className="text-xs text-slate-500">
                The hub prints one on startup when it is not bound to loopback only.
            </span>
        </form>
    );
}
