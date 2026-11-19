/** Exercise the installed configurable adapter, TLS/SSE, loop and Hub event path. */
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:https';
import { dirname, join } from 'node:path';
import { it } from 'node:test';
import { renderWorkerConfig, workerConfigPath } from '../../src/launch/config-render.ts';
import { normalizeSpec } from '../../src/launch/spec.ts';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';

it('preserves research usage across real reasoning-only continuations and provider refusal',
    { skip: e2eSkip, timeout: 60000 }, async t => {
        const ctx = await startE2eHub();
        const cert = join(ctx.config.dataDir, 'fixture.pem');
        const key = join(ctx.config.dataDir, 'fixture.key');
        t.after(async () => {
            await ctx.hub.stop();
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
        });
        execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-keyout', key, '-out', cert, '-subj', '/CN=localhost',
            '-addext', 'subjectAltName=IP:127.0.0.1'], { stdio: 'ignore' });

        const requests = new Map();
        const server = createServer({ key: readFileSync(key), cert: readFileSync(cert) }, async (req, res) => {
            const chunks = [];
            for await (const bytes of req) chunks.push(bytes);
            const body = JSON.parse(Buffer.concat(chunks));
            const seen = requests.get(body.model) ?? [];
            seen.push(body);
            requests.set(body.model, seen);
            if (seen.length === 2 && body.model === 'refused') {
                res.writeHead(400).end('{"error":{"message":"unsupported continuation"}}');
                return;
            }
            const delta = seen.length === 1 ? { reasoning_content: 'saved thought' } : { content: 'final text' };
            // The first exchange explicitly measures zero. The refusal scenario
            // only charges this first, committed exchange.
            const frame = { choices: [{ index: 0, delta, finish_reason: 'stop' }],
                usage: { in: 12, out: 3, cached: seen.length === 1 ? 0 : 7 } };
            if (body.model === 'missing') {
                delete frame.usage.cached;
                // An unrelated standard counter cannot attest the custom mapping.
                frame.usage.prompt_cache_hit_tokens = 0;
            }
            const wire = body.model === 'large' && seen.length === 2
                ? JSON.stringify(frame).replace('"cached":7', '"cached":9007199254740993')
                    .replace('"in":12', '"in":9007199254740993')
                : JSON.stringify(frame);
            res.writeHead(200, { 'content-type': 'text/event-stream' })
                .end('data: ' + wire + '\n\ndata: [DONE]\n\n');
        });
        server.listen(0, '127.0.0.1');
        await once(server, 'listening');
        t.after(() => new Promise(resolve => { server.close(resolve); server.closeAllConnections(); }));
        const panel = await connectWorker(ctx.wsBase + '/panel/ws');
        t.after(() => panel.close());

        for (const model of ['replay', 'omit', 'refused', 'large', 'missing']) {
            const spec = { provider: 'mock', env: { SSL_CERT_FILE: cert } };
            const session = ctx.hub.registry.create('research-' + model, spec);
            const config = renderWorkerConfig({ config: ctx.config, sessionId: session.id,
                spec: normalizeSpec(ctx.config, spec),
                endpoints: ctx.hub.supervisor.endpointsFor(session.id, session.token) });
            config.driver_model = 'research';
            config.providers = { research: { plugin: 'chat-completions', model,
                endpoint: { base_url: 'https://127.0.0.1:' + server.address().port, auth: { scheme: 'none' } },
                retry: { max_attempts: 0 },
                config: { compatibility: { replay_assistant_reasoning: model === 'replay',
                    token_cost: { prompt: '/in', generated: '/out', cache_hit: '/cached' } } } } };
            delete config.modality_assist_model;
            config.experimental = { cumulative_token_control: { type: 'cache_hit', threshold: 100 } };
            const path = workerConfigPath(ctx.config, session.id);
            mkdirSync(dirname(path), { recursive: true });
            writeFileSync(path, JSON.stringify(config));
            const started = await ctx.hub.supervisor.start(session);
            assert.equal(started.ok, true, started.error);
            await until(() => session.connected && session.workerCapabilities, { timeout: 10000 })
                .catch(error => { throw new Error(ctx.hub.supervisor.logs(session).join('\n'), { cause: error }); });
            panel.send({ type: 'subscribe', session: session.id });
            await panel.waitFor(m => m.type === 'subscribed' && m.session.session_id === session.id);
            panel.send({ type: 'input', session: session.id, request_id: model,
                content: [{ type: 'text', raw: 'Test the continuation', modality: 'text' }] });
            const terminal = m => m.envelope?.event === 'run_finished' && m.envelope.request_id === model;
            const { envelope } = await panel.waitFor(terminal, { timeout: 10000 });
            const seen = requests.get(model);
            if (model === 'missing') {
                assert.equal(seen?.length, 1);
                assert.equal(envelope.data.status, 'failed');
                assert.match(envelope.data.experimental.error, /cache-hit usage unavailable/);
            } else {
                assert.equal(seen?.length, 2, JSON.stringify(envelope.data));
                const assistant = seen[1].messages.at(-1);
                assert.equal(assistant.role, 'assistant');
                assert.equal(assistant.reasoning_content, model === 'replay' ? 'saved thought' : undefined);
                assert.equal(envelope.data.status, model === 'refused' ? 'failed' : 'completed',
                    JSON.stringify(envelope.data));
                if (model === 'refused') assert.equal(envelope.data.failure.stage, 'model_request');
                else assert.ok(panel.messages.some(m => m.envelope?.event === 'model_response'
                    && m.envelope.request_id === model && JSON.stringify(m.envelope.data).includes('final text')));
                assert.equal(envelope.data.experimental.steps, model === 'refused' ? 1 : 2);
                assert.equal(envelope.data.experimental.tokens,
                    model === 'large' ? '9007199254740993' : model === 'refused' ? 0 : 7);
                assert.equal(envelope.data.experimental.error, '');
            }
            assert.equal((await ctx.hub.supervisor.stop(session)).ok, true);
            assert.equal(panel.messages.filter(terminal).length, 1);
        }
    });
