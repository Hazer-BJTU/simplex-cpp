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
import { randomUUID } from 'node:crypto';
import { captureStartup, resolvedStartupLaunch } from '../subagents/fork.ts';
import { captureDockerManagement, restoreDockerManagement, dockerRunning, signalContainer, dockerName } from '../subagents/docker.ts';
import type { DockerManagement } from '../subagents/docker.ts';
import { sessionLaunch, launchEndpoints } from '../configurations/session.ts';
import { createLauncher } from './launcher.ts';
import { spawn } from 'node:child_process';
import type { ChildProcess } from 'node:child_process';
import { createWriteStream, existsSync, mkdirSync, readFileSync, renameSync, statSync }
    from 'node:fs';
import { join } from 'node:path';
import { LineSplitter, RingBuffer } from '../util/ring.ts';
import type { SplitLine } from '../util/ring.ts';
import { logPreview } from '../util/log-preview.ts';
import { BoundedWriter } from '../util/bounded-writer.ts';
import { sessionDir, workerConfigPath } from './config-render.ts';
import { prepareSessionConfig } from './config-file.ts';
import type { NormalizedSpec } from './spec.ts';
import type { Launcher, InvocationContext } from './launcher.ts';
import type { LauncherInvocation, WorkerEndpoints } from './invocation.ts';
import type { Session, SessionRegistry } from '../state/registry.ts';
import type { HubConfig } from '../config.ts';
import type { Logger } from '../log.ts';
import type { ProcessDescription } from '../../shared/protocol.ts';

/** Process lifecycle as the panel sees it. */
export const PROCESS_STATE = {
    stopped: 'stopped',
    starting: 'starting',
    running: 'running',
    stopping: 'stopping',
    exited: 'exited',
    failed: 'failed',
} as const;

/** One process lifecycle state. */
export type ProcessState = (typeof PROCESS_STATE)[keyof typeof PROCESS_STATE];

/** Delay between exit checks while waiting for a stop to complete. */
const EXIT_POLL_MS = 25;

/** How a stop ended. */
export interface StopResult {
    ok: boolean;
    how: string;
    forced: boolean;
}

/**
 * How a start ended.
 *
 * `config` is the spec that was rendered, or the raw one when rendering was not
 * reached. It is optional because one refusal — "a worker is already running" —
 * happens before any spec is looked at, and has nothing to report.
 */
export interface StartResult {
    ok: boolean;
    pid?: number | undefined;
    error?: string | undefined;
    config?: unknown;
}

/** The write side of a worker log. The adopted path substitutes a no-op. */
export interface LogStream {
    write(chunk: string): unknown;
    end(): unknown;
    readonly closed?: Promise<void>;
    readonly droppedRecords?: number;
    readonly failed?: boolean;
}

/**
 * Read field 22 (`starttime`) of `/proc/<pid>/stat`.
 *
 * Pids are reused, so a recorded pid alone is not proof that the process the
 * hub spawned is still the process it will signal. The boot-relative start
 * time, recorded at spawn, distinguishes them.
 *
 * @returns raw tick value, or null when unavailable.
 */
export function readProcessStartTime(pid: number): string | null {
    try {
        return processStartTime(pid);
    } catch {
        return null;
    }
}

function processStartTime(pid: number): string | null {
    const stat = readFileSync(`/proc/${pid}/stat`, 'utf8');
    const after = stat.slice(stat.lastIndexOf(')') + 1).trim();
    return after.split(/\s+/)[19] ?? null;
}

/** Unavailable identity is not evidence of death; absence/reuse on Linux is. */
export function processIdentity(pid: unknown, startTime: unknown): 'same' | 'gone' | 'unknown' {
    if (process.platform !== 'linux' || typeof pid !== 'number'
        || !Number.isInteger(pid) || pid <= 0 || typeof startTime !== 'string'
        || !/^\d+$/.test(startTime)) return 'unknown';
    try {
        const current = processStartTime(pid);
        return current === null ? 'unknown' : current === startTime ? 'same' : 'gone';
    } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== 'ENOENT') return 'unknown';
        // Missing /proc entries can mean an unavailable/hidden procfs, not a
        // dead worker. Require kernel-confirmed absence before deleting data.
        try { process.kill(pid, 0); }
        catch (probe) { return (probe as NodeJS.ErrnoException).code === 'ESRCH' ? 'gone' : 'unknown'; }
        return 'unknown';
    }
}

/**
 * True when `pid` is the same process incarnation that was recorded.
 *
 * Without a recorded start time this fails closed: a pid alone cannot
 * distinguish the worker from an unrelated process that later reused it, and
 * the cost of being wrong is signalling a stranger.
 */
export function isSameProcess(pid: unknown, startTime: unknown): boolean {
    return processIdentity(pid, startTime) === 'same';
}

/** Sleep helper. */
function delay(ms: number): Promise<void> {
    return new Promise((resolve) => {
        const timer = setTimeout(resolve, ms);
        timer.unref?.();
    });
}

/** Read a pid from a pid file, or null when there is nothing usable in it. */
function readPidFile(path: string | null): number | null {
    if (!path) return null;
    try {
        const value = Number.parseInt(readFileSync(path, 'utf8').trim(), 10);
        return Number.isInteger(value) && value > 0 ? value : null;
    } catch {
        return null;
    }
}

/**
 * Trust a pid only while its recorded `/proc` start time still matches.
 *
 * Pids are reused, so a pid alone cannot distinguish the worker from an
 * unrelated process that later took its number. When no start time was recorded
 * — `/proc` is unavailable off Linux — the pid is used as-is rather than
 * refusing to stop a worker the hub did start.
 *
 * @returns the pid to signal, or null when nothing may be signalled.
 */
function verifiedTarget(
    record: ProcessRecord,
    pid: number | null,
    startTime: string | null,
    log: Logger,
): number | null {
    if (pid === null || !Number.isInteger(pid) || pid <= 0) return null;
    if (typeof startTime !== 'string' || startTime.length === 0) return pid;
    if (isSameProcess(pid, startTime)) return pid;
    log.warn(`session ${record.sessionId}: pid ${pid} is no longer the process recorded`
        + ' for it; refusing to signal it');
    return null;
}

/** Rotate a log file when it has grown past its budget. */
function rotateLog(path: string, { logBytes, logFiles }: { logBytes: number; logFiles: number }): void {
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

/** Everything `new ProcessRecord` needs. */
export interface ProcessRecordOptions {
    sessionId: string;
    invocation: LauncherInvocation;
    logPath: string | null;
    logStream: LogStream;
    logs: RingBuffer<string>;
}

/** One supervised worker process, plus its captured output. */
export class ProcessRecord {
    stopPolicy?: Pick<HubConfig['worker'], 'stopTimeoutMs' | 'sigtermGraceMs' | 'sigkillGraceMs'>;
    /** Fence covering both output pipes and the optional disk writer. */
    ownedIo: Promise<void> = Promise.resolve();
    dockerManagement: DockerManagement | null = null;
    readonly sessionId: string;
    state: ProcessState;
    pid: number | null;
    pidStartTime: string | null;
    startedAt: string;
    exitedAt: string | null;
    exitCode: number | null;
    signal: string | null;
    error: string | null;
    stopRequested: boolean;
    readonly command: string;
    readonly args: string[];
    readonly cwd: string;
    readonly pidFile: string | null;
    /**
     * Pid last read from `launcher.pidFile` and the `/proc` start time that was
     * observed for it. A daemonizing launcher's pid is not the pid the hub
     * spawned, so it has to be verified on its own terms.
     */
    pidFilePid: number | null;
    pidFileStartTime: string | null;
    processGroupKilled: boolean;
    readonly logPath: string | null;
    readonly logStream: LogStream;
    readonly logs: RingBuffer<string>;
    /** Independent decoder and partial-line state for each child output pipe. */
    readonly outputSplitters: LineSplitter[] = [];
    /** Content removed only from memory previews; never count annotations as content. */
    private logPreviewTruncatedBytes = 0;
    child: ChildProcess | null;
    /** True when this record was reconstructed from a previous hub run. */
    adopted: boolean;
    /** Interval watching an adopted process, when one was needed. */
    monitor: NodeJS.Timeout | null;
    readonly exited: Promise<ProcessRecord>;
    /** Assigned by the `exited` initializer below. */
    resolveExit!: (record: ProcessRecord) => void;

    constructor({ sessionId, invocation, logPath, logStream, logs }: ProcessRecordOptions) {
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
        this.pidFilePid = null;
        this.pidFileStartTime = null;
        this.processGroupKilled = false;
        this.logPath = logPath;
        this.logStream = logStream;
        this.logs = logs;
        this.child = null;
        this.adopted = false;
        this.monitor = null;
        this.exited = new Promise((resolve) => { this.resolveExit = resolve; });
    }

    /** Serializable description for the panel. */
    describe(): ProcessDescription {
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
            log_truncated_bytes: this.logPreviewTruncatedBytes + this.outputSplitters.reduce(
                (total, splitter) => total + splitter.truncatedBytes, 0),
            file_log_dropped: this.logStream.droppedRecords ?? 0,
            file_log_failed: this.logStream.failed ?? false,
        };
    }

    /** Admit a useful bounded memory preview independently of the optional disk copy. */
    captureLine(line: string, details: SplitLine): void {
        const preview = logPreview(details.prefix, details.omittedBytes, this.logs.byteLimit);
        this.logPreviewTruncatedBytes += preview.truncatedBytes;
        this.logs.push(preview.text);
        this.logStream.write(`${line}\n`);
    }
}

/** Everything `WorkerSupervisor` needs. */
export interface WorkerSupervisorOptions {
    config: HubConfig;
    log: Logger;
    registry: SessionRegistry;
    launcher: Launcher;
    endpointsFor: (sessionId: string, token: string) => WorkerEndpoints;
    /** Resolved mock provider address, read lazily. */
    mockProvider?: (() => { baseUrl: string } | null) | undefined;
    onProcessChange?: ((session: Session, record: ProcessRecord) => void) | undefined;
}

/** Starts, observes, and stops worker processes. */
export class WorkerSupervisor {
    readonly config: HubConfig;
    readonly log: Logger;
    readonly registry: SessionRegistry;
    readonly launcher: Launcher;
    readonly endpointsFor: (sessionId: string, token: string) => WorkerEndpoints;
    readonly mockProvider: (() => { baseUrl: string } | null) | undefined;
    onProcessChange: ((session: Session, record: ProcessRecord) => void) | undefined;

    constructor({
        config, log, registry, launcher, endpointsFor, mockProvider, onProcessChange,
    }: WorkerSupervisorOptions) {
        this.config = config;
        this.log = log;
        this.registry = registry;
        this.launcher = launcher;
        this.endpointsFor = endpointsFor;
        this.mockProvider = mockProvider;
        this.onProcessChange = onProcessChange;
    }

    /** True when a worker process is believed to be alive for this session. */
    isRunning(session: Session): boolean {
        const record = session.process as ProcessRecord | null;
        if (!record) return false;
        return record.state === PROCESS_STATE.starting
            || record.state === PROCESS_STATE.running
            || record.state === PROCESS_STATE.stopping;
    }

    /** Worker configuration path for a session (written even when unused). */
    configPathFor(sessionId: string): string {
        return workerConfigPath(this.config, sessionId);
    }

    /** Installed by the family coordinator; freezes admission before any await. */
    cascade: ((session: Session, stopSelf: () => Promise<StopResult>) => Promise<StopResult>) | undefined;
    beforeStart: ((session: Session) => void) | undefined;

    /** Start a worker for a session. */
    async start(session: Session, rawSpec?: unknown): Promise<StartResult> {
        if (this.isRunning(session)) {
            return { ok: false, error: 'a worker process is already running for this session' };
        }
        this.beforeStart?.(session);
        session.closing = false;
        session.lifecycleId = randomUUID();
        const specSource: unknown = rawSpec ?? session.spec ?? {};
        const directory = sessionDir(this.config, session.id);
        // Filesystem failures are answered rather than thrown: this runs from a
        // panel WebSocket message, where a rejection would end the hub.
        try {
            mkdirSync(directory, { recursive: true });
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            return { ok: false, error: `cannot create ${directory}: ${message}`,
                config: specSource };
        }
        const configPath = workerConfigPath(this.config, session.id);
        let launchConfig = this.config;
        let launcher = this.launcher;
        let endpoints = this.endpointsFor(session.id, session.token);
        let launchSpec = specSource;
        let startupLaunch = null;
        let rendered: { spec: NormalizedSpec; document: unknown };
        try {
            const saved = sessionLaunch(this.config, session.id);
            if (saved) {
                startupLaunch = saved.launch;
                launchConfig = saved.config;
                launcher = createLauncher({ config: launchConfig });
                endpoints = launchEndpoints(endpoints, saved.launch);
                launchSpec = { ...(specSource as object), threads: launchConfig.worker.threads, env: saved.launch.env ?? {}, extraArgs: [] };
            }
            rendered = prepareSessionConfig({
                config: launchConfig,
                sessionId: session.id,
                rawSpec: launchSpec,
                endpoints,
                mock: this.mockProvider?.(),
            });
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            return { ok: false, error: `cannot prepare session configuration: ${message}`, config: specSource };
        }
        session.spec = { ...session.spec, ...rendered.spec,
            // Profile credentials stay in launch.jsonc, not public session metadata.
            ...(launcher !== this.launcher ? { env: {} } : {}),
        };

        let invocation: LauncherInvocation;
        try {
            invocation = launcher.buildInvocation({
                sessionId: session.id,
                spec: rendered.spec,
                configPath,
                sessionDir: directory,
                endpoints,
                token: session.token,
            } satisfies InvocationContext);
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            return { ok: false, error: `cannot build the launcher invocation: ${message}`,
                config: session.spec };
        }

        try {
            const launch = resolvedStartupLaunch(launchConfig, startupLaunch, invocation.env);
            launch.worker!.threads = rendered.spec.threads;
            // Preserve per-session appended flags in the immutable launch snapshot.
            launch.launcher.args = [...launch.launcher.args, ...rendered.spec.extraArgs];
            captureStartup(this.config, session, readFileSync(configPath, 'utf8'), launch);
        } catch {
            return { ok: false, error: 'cannot capture authoritative startup snapshot' };
        }

        const logDirectory = join(directory, 'logs');
        try {
            mkdirSync(logDirectory, { recursive: true });
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            return { ok: false, error: `cannot create ${logDirectory}: ${message}`,
                config: session.spec };
        }
        const logPath = join(logDirectory, 'worker.log');
        rotateLog(logPath, this.config.limits);
        const logs = new RingBuffer<string>({
            limit: this.config.limits.logLines,
            byteLimit: this.config.limits.logRingBytes,
            sizeOf: (line) => Buffer.byteLength(line, 'utf8'),
        });
        const logStream = new BoundedWriter({
            open: () => createWriteStream(logPath, { flags: 'a' }),
            omission: ({ records, bytes }) =>
                `[hub: omitted ${records} worker log records (${bytes} UTF-8 bytes)]\n`,
            onDrop: () => this.log.warn(`session ${session.id}: worker log records omitted`
                + ' from disk; see file_log_dropped'),
            onError: (error) => this.log.warn(
                `session ${session.id}: worker log disabled: ${error.message}`),
        });
        const record = new ProcessRecord({ sessionId: session.id, invocation, logPath, logStream, logs });
        record.stopPolicy = { ...launchConfig.worker };
        try { record.dockerManagement = captureDockerManagement(invocation, launchConfig.launcher.dockerManagementEnv); }
        catch { logStream.end(); return { ok: false, error: 'cannot capture Docker management context' }; }
        session.process = record;

        let child: ChildProcess;
        try {
            child = spawn(record.dockerManagement?.executable ?? invocation.command, invocation.args, {
                cwd: record.dockerManagement?.cwd ?? invocation.cwd,
                // Startup still inherits all worker/container credentials. Only
                // later inspect/kill calls use the minimized durable environment.
                env: { ...process.env, ...invocation.env },
                // A dedicated process group makes an explicit force-kill able
                // to reach descendants; it is never used implicitly.
                detached: true,
                stdio: ['ignore', 'pipe', 'pipe'],
            });
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            this.finish(record, { error: message });
            return { ok: false, error: `cannot spawn ${invocation.command}: ${message}`,
                config: session.spec };
        }

        record.child = child;
        if (child.pid === undefined) {
            const failure = await new Promise<Error>((resolve) => {
                child.once('error', resolve);
                // `error` is emitted on the next tick for a failed spawn; a
                // stray immediate resolve keeps this from hanging if it is not.
                setImmediate(() => resolve(new Error('spawn produced no process')));
            });
            this.finish(record, { error: failure.message });
            return { ok: false, error: `cannot spawn ${invocation.command}: ${failure.message}`,
                config: session.spec };
        }
        record.pid = child.pid;
        record.pidStartTime = readProcessStartTime(child.pid);

        for (const pipe of [child.stdout, child.stderr]) {
            if (!pipe) continue;
            const splitter = new LineSplitter((line, details) => record.captureLine(line, details));
            record.outputSplitters.push(splitter);
            pipe.on('data', (chunk: Buffer) => splitter.push(chunk));
            pipe.once('end', () => splitter.flush());
            pipe.once('close', () => splitter.flush());
        }
        const pipesClosed = Promise.all([child.stdout, child.stderr].map(pipe =>
            !pipe || pipe.closed ? Promise.resolve() : new Promise<void>(resolve => pipe.once('close', resolve))));
        record.ownedIo = pipesClosed.then(async () => { await logStream.closed; });
        let outputTimer: NodeJS.Timeout | null = null;
        const closeOutput = () => {
            if (outputTimer) clearTimeout(outputTimer);
            outputTimer = null;
            for (const splitter of record.outputSplitters) splitter.flush();
            child.stdout?.destroy();
            child.stderr?.destroy();
            logStream.end();
        };
        child.once('close', closeOutput);
        child.on('error', (error: Error) => {
            record.error = error.message;
            this.log.error(`session ${session.id}: worker process error: ${error.message}`);
        });
        child.on('exit', (code: number | null, signal: NodeJS.Signals | null) => {
            // Exit can precede pipe EOF. Capture the remaining bytes before
            // closing the file, but do not wait forever for a descendant that
            // inherited stdout/stderr. The process lifecycle still ends at exit.
            outputTimer = setTimeout(closeOutput, 1000);
            outputTimer.unref();
            this.finish(record, { exitCode: code, signal, closeLog: false });
        });

        if (record.pid) record.state = PROCESS_STATE.running;
        this.log.info(
            `session ${session.id}: started ${invocation.command} (pid ${record.pid})`);
        this.notify(session, record);
        return { ok: true, pid: record.pid, config: session.spec };
    }

    /** Record process termination and release per-process resources. */
    finish(
        record: ProcessRecord,
        { exitCode = null, signal = null, error = null, closeLog = true }:
        {
            exitCode?: number | null;
            signal?: string | null;
            error?: string | null;
            closeLog?: boolean;
        },
    ): void {
        if (record.state === PROCESS_STATE.exited || record.state === PROCESS_STATE.failed) return;
        record.exitedAt = new Date().toISOString();
        record.exitCode = exitCode;
        record.signal = signal;
        if (error) record.error = error;
        record.state = error || (exitCode !== 0 && exitCode !== null && !record.stopRequested)
            ? PROCESS_STATE.failed
            : PROCESS_STATE.exited;
        try {
            if (closeLog) record.logStream.end();
        } catch { /* already closed */ }
        // `finish` is reachable from paths an adopted record's own monitor does
        // not own, so the interval is released here rather than only there.
        if (record.monitor) {
            clearInterval(record.monitor);
            record.monitor = null;
        }
        record.resolveExit(record);
        this.log.info(
            `session ${record.sessionId}: worker process ${record.state}`
            + ` (${signal ? `signal ${signal}` : `code ${exitCode}`})`);
        const session = this.registry.get(record.sessionId);
        if (session) this.notify(session, record);
    }

    /** Wait for a record to reach a terminal state. */
    async waitForExit(record: ProcessRecord, timeoutMs: number): Promise<boolean> {
        const container = dockerName(record);
        if (container) {
            const until = Date.now() + timeoutMs;
            do {
                if (await dockerRunning(record.dockerManagement) === false) {
                    this.finish(record, { exitCode: null });
                    return true;
                }
                if (Date.now() >= until) return false;
                await delay(Math.min(EXIT_POLL_MS, Math.max(1, until - Date.now())));
            } while (true);
        }
        if (record.adopted) {
            const until = Date.now() + timeoutMs;
            do {
                if (processIdentity(record.pid, record.pidStartTime) === 'gone') {
                    this.finish(record, { exitCode: null });
                    return true;
                }
                if (Date.now() >= until) return false;
                await delay(Math.min(EXIT_POLL_MS, Math.max(1, until - Date.now())));
            } while (true);
        }
        if (record.state === PROCESS_STATE.exited || record.state === PROCESS_STATE.failed) return true;
        const outcome = await Promise.race([
            record.exited.then(() => true),
            delay(timeoutMs).then(() => false),
        ]);
        return outcome;
    }

    /** Send a signal to the worker process (or its pid file when declared). */
    signalProcess(
        record: ProcessRecord,
        signal: NodeJS.Signals,
        { processGroup = false }: { processGroup?: boolean } = {},
    ): boolean {
        const pid = this.targetPid(record);
        if (!pid) {
            this.log.warn(`session ${record.sessionId}: ${signal} not sent: no verified pid to signal`);
            return false;
        }
        try {
            if (processGroup) {
                process.kill(-pid, signal);
                record.processGroupKilled = true;
            } else {
                process.kill(pid, signal);
            }
            return true;
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            this.log.warn(`session ${record.sessionId}: ${signal} failed: ${message}`);
            return false;
        }
    }

    /**
     * Pid that signals should target, or null when nothing may be signalled.
     *
     * A launcher that daemonizes writes its real pid to `launcher.pidFile`; the
     * pid the hub spawned would then name a short-lived wrapper. Either
     * candidate is signalled only while its `/proc` start time still matches
     * what was recorded, so a stale pid file cannot become a signal to whoever
     * reused the pid.
     */
    targetPid(record: ProcessRecord): number | null {
        const declared = readPidFile(record.pidFile);
        if (declared === null) {
            return verifiedTarget(record, record.pid, record.pidStartTime, this.log);
        }
        if (declared !== record.pidFilePid) {
            // First sighting of this pid: record which incarnation it is.
            record.pidFilePid = declared;
            record.pidFileStartTime = readProcessStartTime(declared);
        }
        return verifiedTarget(record, declared, record.pidFileStartTime, this.log);
    }

    /**
     * Stop a worker: protocol first, then SIGTERM, then SIGKILL.
     */
    async stop(session: Session, options: { timeoutMs?: number; processGroup?: boolean } = {}): Promise<StopResult> {
        return this.cascade ? this.cascade(session, () => this.stopProcess(session, options))
            : this.stopProcess(session, options);
    }

    /** Internal transport/process stop; family coordination wraps the public entry. */
    async stopProcess(
        session: Session,
        { timeoutMs = (session.process as ProcessRecord | null)?.stopPolicy?.stopTimeoutMs ?? this.config.worker.stopTimeoutMs, processGroup }:
        { timeoutMs?: number; processGroup?: boolean } = {},
    ): Promise<StopResult> {
        const record = session.process as ProcessRecord | null;
        if (!record) return { ok: true, how: 'not-started', forced: false };
        if ((record.state === PROCESS_STATE.exited || record.state === PROCESS_STATE.failed)
            && (!dockerName(record) || await dockerRunning(record.dockerManagement) === false)) {
            return { ok: true, how: 'already-exited', forced: false };
        }
        record.stopRequested = true;
        record.state = PROCESS_STATE.stopping;
        this.notify(session, record);

        const connection = session.connection as
            { isOpen: boolean; requestShutdown(): { ok: boolean; error?: string } } | null;
        if (connection?.isOpen) {
            const sent = connection.requestShutdown();
            if (!sent.ok) this.log.warn(`session ${session.id}: shutdown signal not sent: ${sent.error}`);
        }
        if (await this.waitForExit(record, timeoutMs)) {
            return { ok: true, how: 'shutdown-signal', forced: false };
        }

        this.log.warn(`session ${session.id}: graceful stop timed out; sending SIGTERM`);
        const container = dockerName(record);
        if (container) await signalContainer(record.dockerManagement, 'TERM');
        else this.signalProcess(record, 'SIGTERM');
        if (await this.waitForExit(record, record.stopPolicy?.sigtermGraceMs ?? this.config.worker.sigtermGraceMs)) {
            return { ok: true, how: 'sigterm', forced: true };
        }

        const group = processGroup ?? this.config.forceKillProcessGroup;
        this.log.warn(
            `session ${session.id}: SIGTERM ignored; sending SIGKILL`
            + (group ? ' to the process group' : ''));
        if (container) await signalContainer(record.dockerManagement, 'KILL');
        else this.signalProcess(record, 'SIGKILL', { processGroup: group });
        const exited = await this.waitForExit(record, record.stopPolicy?.sigkillGraceMs ?? this.config.worker.sigkillGraceMs);
        if (!exited) {
            this.log.error(`session ${session.id}: worker did not exit after SIGKILL`);
        }
        return { ok: exited, how: group ? 'sigkill-process-group' : 'sigkill', forced: true };
    }

    /** Stop and start again, waiting for the session lock to be released. */
    async restart(session: Session, rawSpec?: unknown): Promise<StartResult & { stop?: string }> {
        const stopped = await this.stop(session);
        if (!stopped.ok) {
            return { ok: false, error: `could not stop the previous worker (${stopped.how})`,
                config: session.spec };
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
    async forceKill(session: Session, options: { processGroup?: boolean } = {}): Promise<StopResult | { ok: false; error: string }> {
        if (!session.process) return { ok: false, error: 'no worker process is recorded for this session' };
        return this.cascade ? this.cascade(session, () => this.forceKillProcess(session, options))
            : this.forceKillProcess(session, options);
    }

    async forceKillProcess(
        session: Session,
        { processGroup = true }: { processGroup?: boolean } = {},
    ): Promise<StopResult> {
        const record = session.process as ProcessRecord | null;
        if (!record) return { ok: true, how: 'not-started', forced: false };
        if ((record.state === PROCESS_STATE.exited || record.state === PROCESS_STATE.failed)
            && (!dockerName(record) || await dockerRunning(record.dockerManagement) === false)) {
            return { ok: true, how: 'already-exited', forced: false };
        }
        record.stopRequested = true;
        const container = dockerName(record);
        if (container) await signalContainer(record.dockerManagement, 'KILL');
        else this.signalProcess(record, 'SIGKILL', { processGroup });
        const exited = await this.waitForExit(record, record.stopPolicy?.sigkillGraceMs ?? this.config.worker.sigkillGraceMs);
        return {
            ok: exited,
            how: processGroup ? 'sigkill-process-group' : 'sigkill',
            forced: true,
        };
    }

    /** Capture the tail of a session's captured worker output. */
    logs(session: Session, { limit }: { limit?: number } = {}): string[] {
        const lines = (session.process as ProcessRecord | null)?.logs?.toArray() ?? [];
        return typeof limit === 'number' && limit > 0 ? lines.slice(-limit) : lines;
    }

    /**
     * Adopt a process recorded by a previous hub run, when it is still alive.
     *
     * `stored` is the persisted record (snake_case), not a live ProcessRecord:
     * this is the one place the hub reconstructs supervision from disk, so the
     * value is validated rather than trusted.
     */
    adopt(session: Session, stored: unknown): boolean {
        if (typeof stored !== 'object' || stored === null) return false;
        const entry = stored as Record<string, unknown>;
        const container = dockerName({ command: String(entry.command ?? ''), args: Array.isArray(entry.args) ? entry.args : [] });
        if (!isSameProcess(entry.pid, entry.pid_start_time) && !container) return false;
        const record = new ProcessRecord({
            sessionId: session.id,
            invocation: {
                command: typeof entry.command === 'string' ? entry.command : '',
                args: Array.isArray(entry.args) ? entry.args as string[] : [],
                cwd: typeof entry.cwd === 'string' ? entry.cwd : '',
                pidFile: typeof entry.pid_file === 'string' ? entry.pid_file : null,
                // Never read for an adopted record: there is no environment to
                // spawn with, only a process that already exists.
                env: {},
            },
            logPath: typeof entry.log_path === 'string' ? entry.log_path : null,
            logStream: { write() {}, end() {} },
            logs: new RingBuffer<string>({
                limit: this.config.limits.logLines,
                byteLimit: this.config.limits.logRingBytes,
                sizeOf: (line) => Buffer.byteLength(line, 'utf8'),
            }),
        });
        record.dockerManagement = container ? restoreDockerManagement(entry.docker_management, container) : null;
        record.pid = entry.pid as number;
        record.pidStartTime = entry.pid_start_time as string;
        record.startedAt = typeof entry.started_at === 'string' ? entry.started_at : record.startedAt;
        record.state = PROCESS_STATE.running;
        record.adopted = true;
        try { record.stopPolicy = sessionLaunch(this.config, session.id)?.config.worker ?? this.config.worker; }
        catch { this.log.warn(`session ${session.id}: cannot read saved stop policy; using Hub defaults`); }
        record.stopRequested = false;
        // The exit of a process this hub did not spawn can only be observed by
        // polling; it is rare and cheap enough to justify keeping the panel
        // honest about a worker that dies while the hub is running.
        if (container || (typeof record.pidStartTime === 'string' && record.pidStartTime.length > 0)) {
            record.monitor = setInterval(() => {
                if (container) {
                    void dockerRunning(record.dockerManagement).then(running => {
                        if (running === false) this.finish(record, { exitCode: null });
                    });
                    return;
                }
                if (processIdentity(record.pid, record.pidStartTime) === 'gone') {
                    if (record.monitor) clearInterval(record.monitor);
                    record.monitor = null;
                    this.finish(record, { exitCode: null, signal: null });
                }
            }, 2000);
            // Inside the guard: an unconditional unref would throw whenever the
            // monitor was never created.
            record.monitor.unref?.();
        }
        session.process = record;
        this.log.info(`session ${session.id}: adopted worker pid ${record.pid} from a previous hub run`);
        return true;
    }

    /** Stop recorded Docker containers too: a terminal CLI is not container death. */
    async stopAll(
        options?: { timeoutMs?: number; processGroup?: boolean },
    ): Promise<Array<StopResult & { session: string }>> {
        const results: Array<StopResult & { session: string }> = [];
        for (const session of this.registry.list()) {
            if (!this.isRunning(session)
                && !(session.process && dockerName(session.process as ProcessRecord))) continue;
            try { results.push({ session: session.id, ...(await this.stop(session, options)) }); }
            catch { results.push({ session: session.id, ok: false, how: 'cleanup-failed', forced: false }); }
        }
        return results;
    }

    /** Notify the process-change hook, containing its failures. */
    notify(session: Session, record: ProcessRecord): void {
        try {
            this.onProcessChange?.(session, record);
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            this.log.warn(`process-change hook failed: ${message}`);
        }
    }
}
