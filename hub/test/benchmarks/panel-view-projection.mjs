/**
 * Deterministic state/selector fixture; run against either checkout:
 * node hub/test/benchmarks/panel-view-projection.mjs [repository-root]
 * Getter/enumeration counts are deterministic. Wall-clock times are reports,
 * not CI thresholds. No DOM or React render performance is inferred from them.
 */
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = process.argv[2] ? resolve(process.argv[2])
    : fileURLToPath(new URL('../../..', import.meta.url));
const load = path => import(pathToFileURL(join(root, path)).href);
const { createPanelStore } = await load('hub/web/src/state/store.ts');
const { emptyView, noteItem, viewDisplayBytes } = await load('hub/web/src/state/view.ts');
const indexed = existsSync(join(root, 'hub/web/src/state/viewProjection.ts'))
    ? (await load('hub/web/src/state/viewProjection.ts')).viewProjection : null;
const store = createPanelStore();
const views = new Map([['active', emptyView('active')]]);
let inactiveReads = 0;
let latestEnumerations = 0;
const event = (session, sequence, name = 'model_response', data = {}) => ({
    type: 'event', event: name, session_id: session, worker_id: 'worker',
    sequence, hub_sequence: sequence, run_id: 'run', request_id: 'request', data,
});
for (let index = 0; index < 80; index += 1) {
    const id = `inactive-${index}`;
    const latestEvents = new Proxy(Object.fromEntries(Array.from({ length: 32 }, (_, key) =>
        [`cache-${key}`, event(id, key + 1, `cache-${key}`, { text: 'x'.repeat(1024) })])), {
        ownKeys(target) { latestEnumerations += 1; return Reflect.ownKeys(target); },
    });
    const approval = { session_id: id, confirmation_id: id, settled_at: null,
        received_at: '2026-01-01', call: { name: 'run_command', arguments: {} } };
    const view = { ...emptyView(id), latestEvents,
        items: [noteItem('retained '.repeat(100))], confirmations: new Map([[id, approval]]) };
    views.set(id, new Proxy(view, { get(target, key) {
        if (['items', 'history', 'historyLoad', 'transcriptNotices', 'latestEvents', 'confirmations'].includes(key)) {
            inactiveReads += 1;
        }
        return Reflect.get(target, key);
    } }));
}
function approvals(views) {
    if (indexed) return indexed(views).prompts;
    const prompts = [];
    for (const view of views.values()) prompts.push(...view.confirmations.values());
    return prompts.filter(prompt => prompt.settled_at === null)
        .sort((a, b) => a.received_at.localeCompare(b.received_at));
}
store.setState({ views, selected: 'active' });
approvals(views);
// Warm display measurement so the comparison targets repeated work, not setup.
for (const view of views.values()) viewDisplayBytes(view);
inactiveReads = 0;
latestEnumerations = 0;
let projections = 0;
let lastPrompts = approvals(views);
const unsubscribe = store.subscribe(state => {
    lastPrompts = approvals(state.views);
    projections += 1;
});
inactiveReads = 0;
const start = performance.now();
try {
    for (let sequence = 1; sequence <= 500; sequence += 1) {
        store.getState().applyEvent({ session: 'active', envelope: event('active', sequence) });
        store.getState().applyLogs({ session: 'active', lines: [`log ${sequence}`], dropped: 0 });
    }
    const milliseconds = Number((performance.now() - start).toFixed(2));
    assert.equal(lastPrompts.length, 80);
    assert.equal(store.getState().view('active').items.length, 500);
    console.log(JSON.stringify({ inactiveViews: 80, modelResponses: 500, logUpdates: 500,
        projections, inactiveReads, latestEnumerations, retainedItems: 500, openApprovals: 80,
        milliseconds }, null, 2));
} finally {
    unsubscribe();
}
