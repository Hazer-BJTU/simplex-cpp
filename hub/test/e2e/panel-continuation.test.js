/** Real panel client → Hub → worker: Ask must replace a previous Approve policy. */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { rmSync } from 'node:fs';
import { WebSocket } from 'ws';
import { createPanelClient } from '../../web/src/lib/client.ts';
import { createPanelStore } from '../../web/src/state/store.ts';
import { memoryStorage } from '../../web/src/lib/token.ts';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { until } from '../helpers/worker.js';

test('panel continuation switches a real worker from Approve to Ask', { skip: e2eSkip, timeout: 90000 }, async () => {
    const ctx = await startE2eHub();
    const store = createPanelStore();
    const client = createPanelClient({ store, WebSocketImpl: WebSocket,
        location: new URL(`${ctx.base}/?session=continuation`), storage: memoryStorage(),
        fetchImpl: (url, options) => fetch(new URL(new URL(url).pathname, ctx.base), options),
    });
    const post = async (path, body) => {
        const response = await fetch(`${ctx.base}${path}`, { method: 'POST',
            headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
        assert.ok(response.ok, await response.clone().text());
        return response.json();
    };
    try {
        // First complete without tools, but establish Approve in the worker.
        // Restore the normal offline tool scenario for the continuation.
        const scenario = ctx.hub.mock.scenarioDelta;
        ctx.hub.mock.scenarioDelta = () => ({ text: 'Initial saved state' });
        await post('/api/sessions', { session: 'continuation', spec: { provider: 'mock', model: 'mock-auto' } });
        const started = await post('/api/sessions/continuation/start');
        assert.equal(started.ok, true, started.error);
        await client.start();
        const session = () => store.getState().sessions.get('continuation');
        const view = () => store.getState().views.get('continuation');
        await until(() => store.getState().connection.state === 'open' && session()?.connected
            && view()?.latestEvents.status, { timeout: 30000, label: 'live panel worker' });
        store.getState().setConfirmMode('continuation', 'approve');
        assert.equal(client.sendInput('continuation', [{ type: 'text', raw: 'Perform the fixture task', modality: 'text' }]), true);
        await until(() => view()?.latestEvents.run_finished, { timeout: 30000, label: 'first completion' });
        const previous = view().latestEvents.run_finished;
        assert.equal(previous.data.status, 'completed');
        ctx.hub.mock.scenarioDelta = scenario;
        store.getState().setConfirmMode('continuation', 'ask');
        assert.equal(client.sendInput('continuation', [], 'continue'), true);
        await until(() => view()?.confirmations.size > 0,
            { timeout: 30000, label: 'Ask confirmation on continuation' });
        const prompt = [...view().confirmations.values()][0];
        assert.equal(prompt.call.name, 'run_command');
        assert.equal(prompt.verified, true);
        assert.equal(client.sendConfirmation('continuation', prompt.confirmation_id, 'approved', 'test'), true);
        await until(() => view()?.latestEvents.run_finished !== previous,
            { timeout: 30000, label: 'continued completion' });
        assert.equal(view().latestEvents.run_finished.data.status, 'completed');
    } finally {
        client.stop();
        await ctx.hub.stop();
        rmSync(ctx.config.dataDir, { recursive: true, force: true });
    }
});
