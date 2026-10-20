/** Bounded compact preview and full local summary through the real C++ worker, Hub, replay and panel store. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { e2eSkip, startE2eHub, WORKER_BIN } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';
import { compactSummary } from '../helpers/compact.js';
import { sessionDir } from '../../src/launch/config-render.ts';
import { createPanelStore } from '../../web/src/state/store.ts';
import { buildRounds } from '../../web/src/app/rounds.ts';

for (const bytes of [8192, 32768, 65536]) {
    it(`preserves a ${bytes}-byte summary locally and its preview through live/replay delivery`,
        { skip: e2eSkip, timeout: 60000 }, async t => {
            const ctx = await startE2eHub();
            // This test covers complete display/replay, not a shipped persona's
            // size. Keep all installed tools and the production compact budget,
            // but reserve summary space with a dedicated minimal role prompt.
            // Prompt paths are installation-relative. Copy the executable(s)
            // into a private layout and link unchanged release assets; never
            // write into the supplied installation (which may be read-only).
            const originalBin = dirname(resolve(WORKER_BIN));
            const fixtureRoot = join(ctx.config.dataDir, 'worker');
            const fixtureBin = join(fixtureRoot, 'bin');
            mkdirSync(fixtureBin, { recursive: true });
            copyFileSync(WORKER_BIN, join(fixtureBin, 'simplex_worker'));
            // A musl distribution separates the entry wrapper from its ELF.
            // Copy the ELF too: symlinking it would resolve resources beside
            // the original installation and miss this fixture's private prompt.
            const workerElf = join(originalBin, 'simplex_worker.bin');
            if (existsSync(workerElf)) {
                copyFileSync(workerElf, join(fixtureBin, 'simplex_worker.bin'));
            }
            const sharedAssets = join(originalBin, '..', 'share');
            if (existsSync(sharedAssets)) {
                symlinkSync(sharedAssets, join(fixtureRoot, 'share'), 'dir');
            }
            symlinkSync(join(originalBin, '..', 'lib'), join(fixtureRoot, 'lib'), 'dir');
            for (const directory of ['plugins', 'schemas', 'prompts']) {
                symlinkSync(join(originalBin, directory), join(fixtureBin, directory), 'dir');
            }
            ctx.config.worker.bin = join(fixtureBin, 'simplex_worker');
            const promptFile = join(fixtureBin, 'compact-display-prompt.yaml');
            writeFileSync(promptFile, JSON.stringify({
                sections: [{ name: 'role', title: 'Role', stability: 'immutable',
                    text: 'Complete the requested task.' }],
            }));
            ctx.config.worker.systemPromptFile = 'compact-display-prompt.yaml';
            const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
            t.after(async () => {
                await panel.close();
                await ctx.hub.stop();
                rmSync(ctx.config.dataDir, { recursive: true, force: true });
            });
            const summary = compactSummary(bytes);
            let exchanges = 0;
            ctx.hub.mock.scenarioDelta = () => ({ text: ++exchanges === 1 ? 'Original answer.' : summary });
            const session = ctx.hub.registry.create('compact-summary', { provider: 'mock', model: 'mock-text' });
            const started = await ctx.hub.supervisor.start(session);
            assert.equal(started.ok, true, started.error);
            await until(() => session.workerCapabilities?.names.includes('context-compact'), { timeout: 20000 });
            panel.send({ v: 1, type: 'subscribe', session: session.id });
            await panel.waitFor(message => message.type === 'subscribed');
            panel.send({ v: 1, type: 'input', session: session.id, request_id: 'task',
                content: [{ type: 'text', modality: 'text', raw: 'Historical context. '.repeat(5000) }] });
            const ordinary = await panel.waitFor(message => message.type === 'event'
                && message.envelope.event === 'run_finished' && message.envelope.request_id === 'task', { timeout: 20000 });
            assert.equal(ordinary.envelope.data.status, 'completed');
            panel.send({ v: 1, type: 'input', session: session.id, request_id: 'compact', operation: 'compact' });
            const finished = await panel.waitFor(message => message.type === 'event'
                && message.envelope.event === 'run_finished' && message.envelope.request_id === 'compact', { timeout: 20000 });
            assert.equal(finished.envelope.data.status, 'completed', JSON.stringify(finished));
            const live = panel.messages.find(message => message.type === 'event'
                && message.envelope.event === 'compact_finished');
            const preview = live.envelope.data.summary;
            assert.equal(typeof preview, 'string');
            assert.ok(Buffer.byteLength(preview) <= 32768);
            const memory = join(sessionDir(ctx.config, session.id), 'memory.md');
            assert.ok(preview.includes(memory));
            assert.equal(readFileSync(memory, 'utf8'), summary);
            if (bytes >= 32768) assert.ok(preview.includes('Summary truncated'));
            else assert.ok(preview.includes(summary));
            assert.equal(live.envelope.data.display_truncated, undefined);
            const saved = JSON.parse(readFileSync(join(sessionDir(ctx.config, session.id), 'state/state.json'), 'utf8'));
            assert.deepEqual(saved.turns, []);
            assert.ok(JSON.stringify(saved).includes(JSON.stringify(preview).slice(1, -1)));
            // A new panel gets the same normalized summary from bounded replay.
            const replay = await connectWorker(`${ctx.wsBase}/panel/ws`);
            t.after(() => replay.close());
            replay.send({ v: 1, type: 'subscribe', session: session.id });
            const subscribed = await replay.waitFor(message => message.type === 'subscribed');
            assert.equal(subscribed.transcript.find(event => event.event === 'compact_finished').data.summary, preview);
            const store = createPanelStore();
            store.getState().applySubscribed(subscribed);
            const view = store.getState().views.get(session.id);
            const round = buildRounds(view.items, view.confirmations, view.requests).find(round => round.compactResult);
            assert.equal(round.compactResult.summary, preview);
        });
}
