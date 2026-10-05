/** Minimal lifecycle/security view; headless conversations belong to the parent. */
import { useState } from 'react';
import type { SessionDescription } from '../../../shared/protocol.ts';
import { useClient } from './ClientContext.tsx';

export function HeadlessPanel({ session }: { session: SessionDescription }) {
    const client = useClient();
    const [saving, setSaving] = useState(false);
    const [error, setError] = useState('');
    const detail = session.subagent;
    async function setPolicy(policy: string): Promise<void> {
        setSaving(true);
        setError('');
        try {
            await client.rest.request('POST', `/api/sessions/${encodeURIComponent(session.session_id)}/subagent-policy`, { body: { policy } });
            client.refreshSessions();
        } catch (cause) { setError(cause instanceof Error ? cause.message : String(cause)); }
        finally { setSaving(false); }
    }
    return (
        <section className="flex-1 overflow-auto p-6" data-testid="headless-panel">
            <h1 className="break-all font-mono text-lg font-semibold">{session.session_id}</h1>
            <p className="mt-2 text-sm text-ink-muted">Headless subagent · parent {detail?.parent}</p>
            <dl className="mt-6 grid grid-cols-[8rem_1fr] gap-2 text-sm">
                <dt>Lifecycle</dt><dd>{detail?.lifecycle}</dd>
                <dt>Process</dt><dd>{session.process?.state ?? 'not started'}</dd>
                <dt>Connection</dt><dd>{session.connected ? 'connected' : 'disconnected'}</dd>
                <dt>Run</dt><dd>{detail?.active ? 'active' : 'idle'}</dd>
                <dt>Health</dt><dd>{detail?.health} · {detail?.reason}</dd>
                <dt>Last observed</dt><dd>{detail?.observed_at ?? 'no identified event yet'}</dd>
                <dt>Approvals</dt><dd>{session.confirmations.length} pending</dd>
            </dl>
            <label className="mt-6 flex items-center gap-3 text-sm">
                Safety policy
                <select aria-label="Subagent safety policy" value={detail?.policy ?? 'ask'}
                    disabled={saving || detail?.lifecycle !== 'ready'}
                    className="rounded border border-line bg-surface px-3 py-2 focus:outline-none"
                    onChange={event => { void setPolicy(event.target.value); }}>
                    <option value="ask">Ask</option><option value="deny">Deny</option><option value="approve">Approve</option>
                </select>
            </label>
            <p className="mt-2 max-w-xl text-xs text-ink-muted">Changes affect new confirmation requests. Existing approvals remain individually actionable. This policy does not sandbox tools.</p>
            <p className="mt-4 max-w-xl text-sm text-ink-muted">The parent worker controls conversation and lifetime. Persistence, including the primary dialogue, is removed after confirmed shutdown.</p>
            {error && <p role="alert" className="mt-3 text-sm text-danger">{error}</p>}
        </section>
    );
}
