/** Opt-in foreground Docker fork/lifetime/host-cleanup regression. */
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { it } from 'node:test';
import { startTestHub } from '../helpers/hub.js';
import { connectWorker, until } from '../helpers/worker.js';
import { sessionDir } from '../../src/launch/config-render.ts';
import { dockerRunning } from '../../src/subagents/docker.ts';

const enabled = process.env.SIMPLEX_DOCKER_WORKER_TEST === '1';
it('clean-forks Docker workers and cleans a root-written child after its launcher is killed',
    { skip: enabled ? false : 'opt-in Docker test', timeout: 90000 }, async t => {
        assert.notEqual(process.getuid(), 0, 'run the Hub test as a non-root user');
        // Optional installed runtime mount permits checking a released worker
        // against the existing test image without rebuilding that image.
        const runtime = process.env.SIMPLEX_DOCKER_WORKER_RUNTIME;
        const command = ['docker', 'run', '--rm', '--init', '--network', 'host',
            '--name', 'simplex-subagent-test-{session}', '--user', '0:{gid}',
            '--entrypoint', '/bin/sh', '-v', '{session_dir}:{session_dir}',
            '-v', '{session_dir}/config:{session_dir}/config:ro',
            ...(runtime ? ['-v', `${runtime}:/opt/subagent-runtime:ro`] : []),
            process.env.SIMPLEX_DOCKER_WORKER_IMAGE ?? 'simplex-hub-test:latest',
            '-c', 'umask 0002; exec "$@"', 'sh',
            runtime ? '/opt/subagent-runtime/bin/simplex_worker' : '/src/build/bin/simplex_worker',
            '--config', '{config}', '--session', '{session}', '--threads', '{threads}'];
        const ctx = await startTestHub({ launcher: { kind: 'command', command },
            mock: { enabled: true, slowMs: 5000 },
            worker: { connectHost: '127.0.0.1', stopTimeoutMs: 3000, sigtermGraceMs: 1000, sigkillGraceMs: 3000 } });
        t.after(() => ctx.hub.stop());
        const parent = ctx.hub.registry.create('docker-subagent-parent', { provider: 'mock', model: 'mock-slow' });
        const started = await ctx.hub.supervisor.start(parent);
        assert.equal(started.ok, true, started.error);
        await until(() => parent.workerCapabilities?.names.includes('session-history'), { timeout: 30000 });
        parent.connection.sendPayload({ type: 'payload', data: { operation: 'message', request_id: 'parent-task',
            content: [{ type: 'text', modality: 'text', raw: 'Parent delegation fixture.' }] } });
        await until(() => !!parent.activeRunId);
        async function rpc(route, arguments_) {
            const url = new URL(ctx.hub.supervisor.endpointsFor(parent.id, parent.token).tools);
            url.pathname += `/${route}`;
            const peer = await connectWorker(url.href);
            peer.send({ type: 'tool_request', data: { worker_id: parent.identity.workerId, session_id: parent.id,
                run_id: parent.activeRunId, request_id: `docker-${route}`, arguments: arguments_ } });
            const answer = (await peer.waitFor(message => message.type === 'tool_response')).data;
            await peer.waitForClose();
            assert.equal(answer.status, 'succeeded', JSON.stringify(answer));
            return answer.result;
        }
        const result = await rpc('subagent/clean-fork', {});
        const child = ctx.hub.registry.require(result.subagent_id);
        await until(() => child.subagent.lifecycle === 'ready', { timeout: 30000 });
        await rpc('subagent/send', { subagent_id: child.id, operation: 'message',
            content: [{ type: 'text', modality: 'text', raw: 'Child task.' }] });
        const record = ctx.hub.subagents.children.get(child.id);
        await until(() => record.conversation.value.turns[0]?.steps.length && !record.conversation.value.stale,
            { timeout: 15000 });
        const root = sessionDir(ctx.config, child.id);
        assert.equal(existsSync(`${root}/state/state.json`), true);
        const management = child.process.dockerManagement;
        assert.equal(await dockerRunning(management), true);
        process.kill(child.process.pid, 'SIGKILL');
        await until(() => child.subagent.lifecycle === 'stopped', { timeout: 15000 });
        // Cleanup removed the startup cwd; use the existing data root only for
        // this post-cleanup diagnostic, preserving the original CLI/environment.
        assert.equal(await dockerRunning({ ...management, cwd: ctx.config.dataDir }), false);
        assert.equal(existsSync(root), false);
        assert.equal(existsSync(sessionDir(ctx.config, parent.id)), true);
        assert.equal((await ctx.hub.supervisor.stop(parent)).ok, true);
    });
