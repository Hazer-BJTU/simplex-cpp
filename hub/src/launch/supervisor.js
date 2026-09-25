/**
 * @file worker process supervision.
 *
 * The hub owns worker lifetimes: it renders a configuration, spawns the
 * configured launcher, captures output, and stops the worker again. Two rules
 * shape everything here.
 *
 * **Stopping is protocol-first.** A graceful stop sends the worker
 * `{"type":"signal","data":{"operation":"shutdown"}}` and waits; only then does
 * the hub escalate to SIGTERM and finally SIGKILL. That order is what makes an
 * orphaned worker controllable: a hub that restarted without its process table
 * can still stop a worker it can reach over the socket, and a launcher that
 * daemonized is still stoppable even though the spawned pid is meaningless.
 *
 * **A session lock is the worker's, not the hub's.** The worker takes an
 * exclusive `flock` on its session directory, so starting a second worker for a
 * running session fails inside the worker with a confusing message. The
 * supervisor therefore refuses to start a session whose recorded process is
 * still alive, and a restart always waits for the exit before spawning.
 */
import { spawn } from 'node:child_process';
import { createWriteStream, existsSync, mkdirSync, readFileSync, renameSync, statSync, writeFileSync }
    from 'node:fs';
import { join } from 'node:path';
import { LineSplitter, RingBuffer } from '../util/ring.js';
import { renderSessionConfig, sessionDir, workerConfigPath } from './config-render.js';

/** Process lifecycle as the panel sees it. */
export const PROCESS_STATE = {
    stopped: 'stopped',
    starting: 'starting',
    running: 'running',
    stopping: 'stopping',
    exited: 'exited',
    failed: 'failed',
};

/** Delay between exit checks while waiting for a stop to complete. */
const EXIT_POLL_MS = 25;

/**
 * Read field 22 (`starttime`) of `/proc/<pid>/stat`.
 *
 * Pids are reused, so a recorded pid alone is not proof that the process the
 * hub spawned is still the process it will signal. The boot-relative start
 * time, recorded at spawn, distinguishes them.
 *
 * @returns {string|null} raw tick value, or null when unavailable.
 */
export function readProcessStartTime(pid) {
    try {
        const stat = readFileSync(`/proc/${pid}/stat`, 'utf8');
        const after = stat.slice(stat.lastIndexOf(')') + 1).trim();
        const fields = after.split(/\s+/);
        return fields[19] ?? null;
    } catch {
        return null;
    }
}

/**
 * True when `pid` is the same process incarnation that was recorded.
 *
 * Without a recorded start time this fails closed: a pid alone cannot
 * distinguish the worker from an unrelated process that later reused it, and
 * the cost of being wrong is signalling a stranger.
 */
export function isSameProcess(pid, startTime) {
    if (!Number.isInteger(pid) || pid <= 0) return false;
    if (typeof startTime !== 'string' || startTime.length === 0) return false;
    const current = readProcessStartTime(pid);
    return current !== null && current === startTime;
}

/** Sleep helper. */
function delay(ms) {
    return new Promise((resolve) => {
        const timer = setTimeout(resolve, ms);
        timer.unref?.();
    });
}

/** Rotate a log file when it has grown past its budget. */
function rotateLog(path, { logBytes, logFiles }) {
    try {
        if (!existsSync(path) || statSync(path).size < logBytes) return;
        for (let index = logFiles - 1; index >= 1; index -= 1) {
            const from = index === 1 ? path : `${path}.${index - 1}`;
            const to = `${path}.${index}`;
            if (existsSync(from)) renameSync(from, to);
        }
    } catch {
        // A failed rotation must not prevent the worker from starting.
    }
}

/** One supervised worker process, plus its captured output. */
export class ProcessRecord {
    constructor({ sessionId, invocation, logPath, logStream, logs }) {
        this.sessionId = sessionId;
        this.state = PROCESS_STATE.starting;
        this.pid = null;
        this.pidStartTime = null;
        this.startedAt = new Date().toISOString();
        this.exitedAt = null;
        this.exitCode = null;
        this.signal = null;
        this.error = null;
        this.stopRequested = false;
        this.command = invocation.command;
        this.args = invocation.args;
        this.cwd = invocation.cwd;
        this.pidFile = invocation.pidFile;
        this.processGroupKilled = false;
        this.logPath = logPath;
        this.logStream = logStream;
        this.logs = logs;
        this.child = null;
        this.exited = new Promise((resolve) => { this.resolveExit = resolve; });
    }

    /** Serializable description for the panel. */
    describe() {
        return {
            state: this.state,
            pid: this.pid,
            started_at: this.startedAt,
            exited_at: this.exitedAt,
            exit_code: this.exitCode,
            signal: this.signal,
            error: this.error,
            stop_requested: this.stopRequested,
            command: this.command,
            args: this.args,
            cwd: this.cwd,
            process_group_killed: this.processGroupKilled,
            log_path: this.logPath,
            log_lines: this.logs.size,
            log_dropped: this.logs.dropped,
        };
    }
}

/** Starts, observes, and stops worker processes. */
export class WorkerSupervisor {
    /**
     * @param {object} options
     * @param {object} options.config hub configuration.
     * @param {object} options.log hub logger.
     * @param {object} options.registry session registry.
     * @param {object} options.launcher launcher from src/launch/launcher.js.
     * @param {(sessionId: string, token: string) => {events: string, confirm: string}} options.endpointsFor
     * @param {() => ({baseUrl: string}|null)} [options.mockProvider] resolved mock address.
     * @param {(session: object, record: ProcessRecord|null) => void} [options.onProcessChange]
     */
    constructor({ config, log, registry, launcher, endpointsFor, mockProvider, onProcessChange }) {
        this.config = config;
        this.log = log;
        this.registry = registry;
        this.launcher = launcher;
        this.endpointsFor = endpointsFor;
        this.mockProvider = mockProvider;
        this.onProcessChange = onProcessChange;
    }

    /** True when a worker process is believed to be alive for this session. */
    isRunning(session) {
        const record = session.process;
        if (!record) return false;
        return record.state === PROCESS_STATE.starting
            || record.state === PROCESS_STATE.running
            || record.state === PROCESS_STATE.stopping;
    }

    /** Worker configuration path for a session (written even when unused). */
    configPathFor(sessionId) {
        return workerConfigPath(this.config, sessionId);
    }

    /**
     * Start a worker for a session.
     *
     * @param {object} session registry session.
     * @param {object} [rawSpec] session spec overrides stored on the session.
     * @returns {Promise<{ok: boolean, pid?: number, error?: string, config: object}>}
     */
    async start(session, rawSpec) {
        if (this.isRunning(session)) {
            return { ok: false, error: 'a worker process is already running for this session' };
        }
        const specSource = rawSpec ?? session.spec ?? {};
        const directory = sessionDir(this.config, session.id);
        mkdirSync(directory, { recursive: true });
        const configPath = workerConfigPath(this.config, session.id);
        let rendered;
        try {
            rendered = renderSessionConfig({
                config: this.config,
                sessionId: session.id,
                rawSpec: specSource,
                endpoints: this.endpointsFor(session.id, session.token),
                mock: this.mockProvider?.(),
            });
        } catch (error) {
            return { ok: false, error: `invalid session spec: ${error.message}`, config: specSource };
        }
        session.spec = rendered.spec;
        // The path is always written, so an operator can inspect what a
        // launcher-owned configuration would have contained.
        writeFileSync(configPath, `${JSON.stringify(rendered.document, null, 2)}\n`);

        let invocation;
        try {
            invocation = this.launcher.buildInvocation({
                sessionId: session.id,
                spec: rendered.spec,
                configPath,
                sessionDir: directory,
                endpoints: this.endpointsFor(session.id, session.token),
                token: session.token,
            });
        } catch (error) {
            return { ok: false, error: `cannot build the launcher invocation: ${error.message}`,
                config: rendered.spec };
        }

        const logPath = join(directory, 'worker.log');
        rotateLog(logPath, this.config.limits);
        const logs = new RingBuffer({
            limit: this.config.limits.logLines,
            byteLimit: this.config.limits.logBytes,
        });
        const logStream = createWriteStream(logPath, { flags: 'a' });
        const record = new ProcessRecord({ sessionId: session.id, invocation, logPath, logStream, logs });
        session.process = record;

        let child;
        try {
            child = spawn(invocation.command, invocation.args, {
                cwd: invocation.cwd,
                env: { ...process.env, ...invocation.env },
                // A dedicated process group makes an explicit force-kill able
                // to reach descendants; it is never used implicitly.
                detached: true,
                stdio: ['ignore', 'pipe', 'pipe'],
            });
        } catch (error) {
            this.finish(record, { error: error.message });
            return { ok: false, error: `cannot spawn ${invocation.command}: ${error.message}`,
                config: rendered.spec };
        }

        record.child = child;
        if (child.pid === undefined) {
            const failure = await new Promise((resolve) => {
                child.once('error', resolve);
                // `error` is emitted on the next tick for a failed spawn; a
                // stray immediate resolve keeps this from hanging if it is not.
                setImmediate(() => resolve(new Error('spawn produced no process')));
            });
            this.finish(record, { error: failure.message });
            return { ok: false, error: `cannot spawn ${invocation.command}: ${failure.message}`,
                config: rendered.spec };
        }
        record.pid = child.pid;
        record.pidStartTime = readProcessStartTime(child.pid);

        const splitter = new LineSplitter((line) => {
            logs.push(line);
            logStream.write(`${line}\n`);
        });
        child.stdout.on('data', (chunk) => splitter.push(chunk));
        child.stderr.on('data', (chunk) => splitter.push(chunk));
        child.on('error', (error) => {
            record.error = error.message;
            this.log.error(`session ${session.id}: worker process error: ${error.message}`);
        });
        child.on('exit', (code, signal) => {
            splitter.flush();
            this.finish(record, { exitCode: code, signal });
        });

        if (record.pid) record.state = PROCESS_STATE.running;
        this.log.info(
            `session ${session.id}: started ${invocation.command} (pid ${record.pid})`);
        this.notify(session, record);
        return { ok: true, pid: record.pid, config: rendered.spec };
    }

    /** Record process termination and release per-process resources. */
    finish(record, { exitCode = null, signal = null, error = null }) {
        if (record.state === PROCESS_STATE.exited || record.state === PROCESS_STATE.failed) return;
        record.exitedAt = new Date().toISOString();
        record.exitCode = exitCode;
        record.signal = signal;
        if (error) record.error = error;
        record.state = error || (exitCode !== 0 && exitCode !== null && !record.stopRequested)
            ? PROCESS_STATE.failed
            : PROCESS_STATE.exited;
        try {
            record.logStream.end();
        } catch { /* already closed */ }
        record.resolveExit(record);
        this.log.info(
            `session ${record.sessionId}: worker process ${record.state}`
            + ` (${signal ? `signal ${signal}` : `code ${exitCode}`})`);
        const session = this.registry.get(record.sessionId);
        if (session) this.notify(session, record);
    }

    /** Wait for a record to reach a terminal state. */
    async waitForExit(record, timeoutMs) {
        if (record.state === PROCESS_STATE.exited || record.state === PROCESS_STATE.failed) return true;
        const outcome = await Promise.race([
            record.exited.then(() => true),
            delay(timeoutMs).then(() => false),
        ]);
        return outcome;
    }

    /** Send a signal to the worker process (or its pid file when declared). */
    signalProcess(record, signal, { processGroup = false } = {}) {
        const pid = this.targetPid(record);
        if (!pid) return false;
        try {
            if (processGroup) {
                process.kill(-pid, signal);
                record.processGroupKilled = true;
            } else {
                process.kill(pid, signal);
            }
            return true;
        } catch (error) {
            this.log.warn(`session ${record.sessionId}: ${signal} failed: ${error.message}`);
            return false;
        }
    }

    /**
     * Pid that signals should target.
     *
     * A launcher that daemonizes writes its real pid to `launcher.pidFile`; the
     * pid the hub spawned would then name a short-lived wrapper.
     */
    targetPid(record) {
        if (record.pidFile) {
            try {
                const value = Number.parseInt(readFileSync(record.pidFile, 'utf8').trim(), 10);
                if (Number.isInteger(value) && value > 0) return value;
            } catch {
                // Fall back to the spawned pid.
            }
        }
        return record.pid;
    }

    /**
     * Stop a worker: protocol first, then SIGTERM, then SIGKILL.
     *
     * @param {object} session
     * @param {object} [options]
     * @param {number} [options.timeoutMs] graceful budget before SIGTERM.
     * @param {boolean} [options.processGroup] allow a process-group SIGKILL.
     * @returns {Promise<{ok: boolean, how: string, forced: boolean}>}
     */
    async stop(session, { timeoutMs = this.config.worker.stopTimeoutMs, processGroup } = {}) {
        const record = session.process;
        if (!record) return { ok: true, how: 'not-started', forced: false };
        if (record.state === PROCESS_STATE.exited || record.state === PROCESS_STATE.failed) {
            return { ok: true, how: 'already-exited', forced: false };
        }
        record.stopRequested = true;
        record.state = PROCESS_STATE.stopping;
        this.notify(session, record);

        const connection = session.connection;
        if (connection?.isOpen) {
            const sent = connection.requestShutdown();
            if (!sent.ok) this.log.warn(`session ${session.id}: shutdown signal not sent: ${sent.error}`);
        }
        if (await this.waitForExit(record, timeoutMs)) {
            return { ok: true, how: 'shutdown-signal', forced: false };
        }

        this.log.warn(`session ${session.id}: graceful stop timed out; sending SIGTERM`);
        this.signalProcess(record, 'SIGTERM');
        if (await this.waitForExit(record, this.config.worker.sigtermGraceMs)) {
            return { ok: true, how: 'sigterm', forced: true };
        }

        const group = processGroup ?? this.config.forceKillProcessGroup;
        this.log.warn(
            `session ${session.id}: SIGTERM ignored; sending SIGKILL`
            + (group ? ' to the process group' : ''));
        this.signalProcess(record, 'SIGKILL', { processGroup: group });
        const exited = await this.waitForExit(record, this.config.worker.sigkillGraceMs);
        if (!exited) {
            this.log.error(`session ${session.id}: worker did not exit after SIGKILL`);
        }
        return { ok: exited, how: group ? 'sigkill-process-group' : 'sigkill', forced: true };
    }

    /** Stop and start again, waiting for the session lock to be released. */
    async restart(session, rawSpec) {
        const stopped = await this.stop(session);
        if (!stopped.ok) {
            return { ok: false, error: `could not stop the previous worker (${stopped.how})` };
        }
        // The next worker takes the session lock; the previous owner releases it
        // when its process exits, which has just been observed.
        const started = await this.start(session, rawSpec);
        return { ...started, stop: stopped.how };
    }

    /**
     * Kill a stuck worker outright.
     *
     * This is the panel's explicit dangerous action: it skips the protocol and
     * SIGTERM, and by default kills the process group, which also reaches
     * descendants the worker itself would not have promised to terminate.
     */
    async forceKill(session, { processGroup = true } = {}) {
        const record = session.process;
        if (!record) return { ok: false, error: 'no worker process is recorded for this session' };
        if (record.state === PROCESS_STATE.exited || record.state === PROCESS_STATE.failed) {
            return { ok: true, how: 'already-exited', forced: false };
        }
        record.stopRequested = true;
        this.signalProcess(record, 'SIGKILL', { processGroup });
        const exited = await this.waitForExit(record, this.config.worker.sigkillGraceMs);
        return {
            ok: exited,
            how: processGroup ? 'sigkill-process-group' : 'sigkill',
            forced: true,
        };
    }

    /** Capture the tail of a session's captured worker output. */
    logs(session, { limit } = {}) {
        const lines = session.process?.logs?.toArray() ?? [];
        return typeof limit === 'number' && limit > 0 ? lines.slice(-limit) : lines;
    }

    /**
     * Adopt a process recorded by a previous hub run, when it is still alive.
     *
     * `stored` is the persisted record (snake_case), not a live ProcessRecord:
     * this is the one place the hub reconstructs supervision from disk.
     */
    adopt(session, stored) {
        if (!stored || !isSameProcess(stored.pid, stored.pid_start_time)) return false;
        const record = new ProcessRecord({
            sessionId: session.id,
            invocation: {
                command: stored.command ?? '',
                args: stored.args ?? [],
                cwd: stored.cwd ?? '',
                pidFile: stored.pid_file ?? null,
            },
            logPath: stored.log_path ?? null,
            logStream: { write() {}, end() {} },
            logs: new RingBuffer({
                limit: this.config.limits.logLines,
                byteLimit: this.config.limits.logBytes,
            }),
        });
        record.pid = stored.pid;
        record.pidStartTime = stored.pid_start_time;
        record.startedAt = stored.started_at ?? record.startedAt;
        record.state = PROCESS_STATE.running;
        record.adopted = true;
        record.stopRequested = false;
        // The exit of a process this hub did not spawn can only be observed by
        // polling; it is rare and cheap enough to justify keeping the panel
        // honest about a worker that dies while the hub is running.
        if (typeof record.pidStartTime === 'string' && record.pidStartTime.length > 0) {
            record.monitor = setInterval(() => {
                if (!isSameProcess(record.pid, record.pidStartTime)) {
                    clearInterval(record.monitor);
                    record.monitor = null;
                    this.finish(record, { exitCode: null, signal: null });
                }
            }, 2000);
        }
        record.monitor.unref?.();
        session.process = record;
        this.log.info(`session ${session.id}: adopted worker pid ${record.pid} from a previous hub run`);
        return true;
    }

    /** Stop every running worker; used by hub shutdown. */
    async stopAll(options) {
        const results = [];
        for (const session of this.registry.list()) {
            if (!this.isRunning(session)) continue;
            results.push({ session: session.id, ...(await this.stop(session, options)) });
        }
        return results;
    }

    /** Notify the process-change hook, containing its failures. */
    notify(session, record) {
        try {
            this.onProcessChange?.(session, record);
        } catch (error) {
            this.log.warn(`process-change hook failed: ${error.message}`);
        }
    }
}
