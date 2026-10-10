/**
 * Deterministic event-burst fixture using real atomic writes/fsyncs.
 * Run: node hub/test/benchmarks/subagent-persistence.mjs [repository-root]
 * The optional root runs this same fixture against an older checkout (with its
 * Hub dependencies installed). Timings describe this machine, not CI thresholds.
 */
import fs from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { basename, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { monitorEventLoopDelay } from 'node:perf_hooks';

const root = process.argv[2] ? resolve(process.argv[2])
    : fileURLToPath(new URL('../../..', import.meta.url));
const load = path => import(pathToFileURL(join(root, path)).href);
const { testConfig } = await load('hub/test/helpers/hub.js');
const { SessionRegistry } = await load('hub/src/state/registry.ts');
const { SubagentService } = await load('hub/src/subagents/service.ts');
const { ConversationProjection } = await load('hub/src/subagents/conversation.ts');
const { sessionDir } = await load('hub/src/launch/config-render.ts');
const { createLogger } = await load('hub/src/log.ts');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const config = testConfig();
const log = createLogger({ level: 'silent' });
const registry = new SessionRegistry({ config, log });
const parent = registry.create('parent');
const session = registry.create('subagent-12345678-1234-4123-8123-123456789abc');
session.kind = 'headless';
session.subagent = { parent: parent.id, lifecycle: 'ready', policy: 'ask',
    health: 'healthy', reason: '', observed_at: null, active: true };
session.activeRunId = 'run';
session.noteIdentity('worker');
session.workerCapabilities = { workerId: 'worker', names: [] };
const connection = { session, isOpen: true, terminate() {} };
session.connection = connection;
const supervisor = {
    isRunning: () => false,
    stopProcess: async () => ({ ok: true, how: 'not-started', forced: false }),
    stop: current => supervisor.cascade(current, () => supervisor.stopProcess(current)),
};
let broadcasts = 0;
const service = new SubagentService({ config, registry, supervisor, log,
    changed: () => { broadcasts += 1; }, removed() {} });
const conversation = new ConversationProjection(session,
    join(sessionDir(config, session.id), 'conversation.json'), config.subagents.conversationBytes);
service.children.set(session.id, { session, conversation, parent: { session_id: parent.id,
    lifecycle_id: parent.lifecycleId, worker_id: 'parent-worker' }, removed: false,
    startup: null, startupTimer: null, terminalTimer: null, error: '', uncertainStart: false,
    terminationConfirmed: true, cleanupAttempts: 0, retryAt: 0 });
session.trackRequest('request', 'message');
service.operations.set(session.id, { receipts: [], requests: [{ request_id: 'request',
    operation: 'message', state: 'sent', run_id: '', at: 'fixture' }] });

const counts = { fsyncs: 0, metadata: 0, conversation: 0, operations: 0 };
const originalFsync = fs.fsyncSync;
const originalRename = fs.renameSync;
fs.fsyncSync = fd => { counts.fsyncs += 1; return originalFsync(fd); };
fs.renameSync = (from, to) => {
    const result = originalRename(from, to);
    const kind = basename(to).replace('.json', '');
    if (Object.hasOwn(counts, kind)) counts[kind] += 1;
    return result;
};
syncBuiltinESMExports();
const delay = monitorEventLoopDelay({ resolution: 10 });
try {
    delay.enable();
    await wait(25);
    const start = performance.now();
    const control = new Promise(resolve => setTimeout(() => resolve(performance.now() - start), 0));
    let sequence = 0;
    const event = (name, data = {}) => service.onEvent({ event: name, worker_id: 'worker',
        request_id: 'request', run_id: 'run', sequence: ++sequence,
        received_at: `observation-${sequence}`, data }, connection);
    conversation.trackInput('request', [{ type: 'text', modality: 'text', raw: 'task' }]);
    event('input_committed');
    for (let index = 0; index < 100; index += 1) {
        event('model_response', { content: [{ type: 'text', modality: 'text', raw: `answer ${index}: ` + 'x'.repeat(8192) }] });
        event('tool_calls');
    }
    const burstMs = performance.now() - start;
    const controlMs = await control;
    await wait(250);
    console.log(JSON.stringify({ events: sequence, answerBytes: 8192, ...counts, broadcasts,
        burstMs: Number(burstMs.toFixed(2)), controlMs: Number(controlMs.toFixed(2)),
        maxEventLoopDelayMs: Number((delay.max / 1e6).toFixed(2)),
        retainedSteps: conversation.value.turns[0].steps.length }, null, 2));
} finally {
    delay.disable();
    await service.shutdown();
    fs.fsyncSync = originalFsync;
    fs.renameSync = originalRename;
    syncBuiltinESMExports();
    fs.rmSync(config.dataDir, { recursive: true, force: true });
}
