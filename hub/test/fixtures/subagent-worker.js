#!/usr/bin/env node
/** Worker-protocol fixture with primary history, rejection and reconnect controls. */
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { parse } from 'yaml';
import { WebSocket } from 'ws';
const args = process.argv.slice(2);
const option = key => args[args.indexOf(key) + 1];
const config = parse(readFileSync(option('--config'), 'utf8'));
const session = option('--session');
const worker = `subagent-fixture-${process.pid}`;
let socket;
let stopping = false;
let sequence = 0;
let active = process.env.SIMPLEX_FIXTURE_ACTIVE_SESSION === session;
let currentRequest = '';
let run = active ? 'parent-run' : '';
let revision = 0;
const turns = [];
const event = (name, data = {}, request = currentRequest) => socket.send(JSON.stringify({
    type: 'event', event: name, session_id: session, worker_id: worker,
    request_id: request, run_id: run, sequence: ++sequence, data,
}));
function connect() {
    socket = new WebSocket(config.client.endpoint);
    socket.on('open', () => {
        event('ready', { active, capabilities: ['session-history', 'context-compact'] });
        if (active) event('run_started');
    });
    socket.on('message', raw => {
        const message = JSON.parse(raw.toString());
        const data = message.data;
        if (message.type === 'signal') {
            if (data.operation === 'status') event('status', { active, capabilities: ['session-history', 'context-compact'] });
            if (data.operation === 'shutdown') { stopping = true; socket.close(); setTimeout(() => process.exit(0), 10); }
            if (data.operation === 'test_disconnect') { socket.close(); }
            if (data.operation === 'test_crash') process.exit(7);
            if (data.operation === 'test_active') { active = true; run = 'parent-run'; event('run_started'); }
            if (data.operation === 'test_gap') { sequence += 4; event('status', { active, capabilities: ['session-history', 'context-compact'] }); }
            return;
        }
        if (message.type !== 'payload') return;
        if (data.operation === 'history') {
            const start = data.start ?? 0;
            event('history', { request_id: data.request_id, revision, start, step: data.step ?? 0,
                next: Math.min(turns.length, start + (data.limit ?? 10)), next_step: 0, total: turns.length,
                turns: turns.slice(start, start + (data.limit ?? 10)) });
            return;
        }
        if (active) { event('input_rejected', { request_id: data.request_id, message: 'fixture is busy' }, ''); return; }
        active = true;
        run = `run-${data.request_id}`;
        currentRequest = data.request_id;
        event('input_admitted', { operation: data.operation });
        event('run_started');
        if (data.operation === 'message') {
            turns.push({ index: turns.length, user: data.content, steps: [], omitted_steps: 0 });
            revision += 1;
            event('input_committed');
        }
        setTimeout(() => {
            const content = [{ type: 'text', modality: 'text', raw: 'fixture answer' }];
            if (data.operation === 'compact') {
                turns.length = 0;
                revision += 1;
                event('model_response', { content });
                event('compact_finished', { summary: 'fixture summary', durable: true, revision, removed_turns: 1 });
            } else {
                const turn = turns.at(-1);
                if (turn) turn.steps.push({ index: turn.steps.length, content, reasoning: { raw: 'SECRET_REASONING' }, tool_calls: 1 });
                revision += 1;
                event('tool_calls', [{ name: 'SECRET_TOOL', arguments: { secret: 'SECRET_ARG' } }]);
                event('tool_results', [{ content: 'SECRET_RESULT' }]);
                event('model_response', { content, reasoning: { raw: 'SECRET_REASONING' }, invokes: [{ name: 'SECRET_TOOL' }] });
            }
            active = false;
            revision += 1;
            event('run_finished', { status: 'completed' });
            run = '';
            currentRequest = '';
        }, 60);
    });
    socket.on('error', () => {});
    socket.on('close', () => { if (!stopping) setTimeout(connect, 100); });
}
connect();
process.on('SIGTERM', () => process.exit(0));
