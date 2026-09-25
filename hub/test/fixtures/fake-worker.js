#!/usr/bin/env node
/**
 * @file stand-in worker process for supervisor tests.
 *
 * It accepts the same command line as simplex_worker, reads the hub-generated
 * configuration, connects back to the event endpoint, reports one status event,
 * and exits cleanly when the hub sends the shutdown signal. That is enough to
 * exercise spawning, configuration rendering, endpoint wiring, log capture, and
 * every stop path without building or running the C++ worker.
 */
import { readFileSync } from 'node:fs';
import { WebSocket } from 'ws';

const argv = process.argv.slice(2);

/** Read `--flag value` from the command line. */
function option(flag, fallback = '') {
    const index = argv.indexOf(flag);
    return index === -1 || index + 1 >= argv.length ? fallback : argv[index + 1];
}

const session = option('--session', 'fixture');
const configPath = option('--config');
const config = configPath ? JSON.parse(readFileSync(configPath, 'utf8')) : {};
const endpoint = config.client?.endpoint ?? '';
const workerId = `fixture-${process.pid}`;
let sequence = 0;

process.stdout.write(`fixture: session=${session} pid=${process.pid}\n`);
process.stdout.write(`fixture: config=${configPath}\n`);
process.stderr.write('fixture: stderr line\n');

if (argv.includes('--hang')) {
    // Ignores SIGTERM so the supervisor's escalation can be tested.
    process.on('SIGTERM', () => {});
    setInterval(() => {}, 1000);
}
const ignoreShutdown = argv.includes('--ignore-shutdown');

if (endpoint) {
    const socket = new WebSocket(endpoint);
    const send = (value) => {
        if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(value));
    };
    const event = (name, data = {}) => send({
        type: 'event',
        event: name,
        session_id: session,
        worker_id: workerId,
        request_id: '',
        run_id: '',
        sequence: (sequence += 1),
        data,
    });
    socket.on('open', () => {
        process.stdout.write('fixture: connected\n');
        event('ready', { active: false });
    });
    socket.on('message', (raw) => {
        const message = JSON.parse(raw.toString('utf8'));
        if (message.type === 'signal' && message.data?.operation === 'status') {
            event('status', { active: false, stopping: false, storage_failed: false, rejected_payloads: 0 });
        }
        if (message.type === 'signal' && message.data?.operation === 'shutdown') {
            if (ignoreShutdown) {
                process.stdout.write('fixture: shutdown ignored\n');
                return;
            }
            process.stdout.write('fixture: shutdown signal\n');
            socket.close();
            setTimeout(() => process.exit(0), 20);
        }
        if (message.type === 'payload') {
            event('input_admitted');
        }
    });
    socket.on('error', (error) => {
        process.stderr.write(`fixture: socket error ${error.message}\n`);
    });
}

if (!argv.includes('--hang')) {
    process.on('SIGTERM', () => {
        process.stdout.write('fixture: SIGTERM\n');
        process.exit(0);
    });
    process.on('SIGINT', () => {
        process.stdout.write('fixture: SIGINT\n');
        process.exit(0);
    });
}
