/**
 * Docker target preflight.
 *
 * A docker management target never creates, starts, stops or removes a
 * container: the operator retains one and Hub runs `docker exec`. These checks
 * run before the first target mutation so a typo (stopped container, missing
 * installation, unreachable Hub address) fails as an actionable start error
 * instead of an allocation that can never launch.
 *
 * The checks are read-only and use only the Docker CLI. Public messages name the
 * container, the exit status and the configured deadline. They never echo CLI
 * stdout/stderr, argv, environment values or an endpoint URL with its session
 * token. The first CLI diagnostic line is retained privately on the error for
 * deliberate inspection and is never part of the public message or default logs.
 */
import { spawn } from 'node:child_process';
import { normalizeDockerPreflight } from './managed-launcher.ts';
import type { DockerPreflightChannel, DockerPreflightPolicy, ManagementTarget } from './managed-launcher.ts';
import type { WorkerEndpoints } from './invocation.ts';

/** One Docker management target. */
export type DockerTarget = Extract<ManagementTarget, { kind: 'docker' }>;

/** Stable preflight failure identity. */
export type DockerPreflightCode =
    | 'docker-unavailable'
    | 'inspect-timeout'
    | 'inspect-failed'
    | 'container-missing'
    | 'container-not-running'
    | 'executable-timeout'
    | 'executable-missing'
    | 'executable-failed'
    | 'endpoint-network-none'
    | 'endpoint-loopback'
    | 'endpoint-host-mapping';

/** A start refusal caused by an unusable Docker target. */
export class DockerPreflightError extends Error {
    readonly code: DockerPreflightCode;
    /**
     * Private CLI diagnostic, kept out of the public message and default logs.
     * It may contain operator credentials; inspect it deliberately.
     */
    readonly detail: string | null;

    constructor(code: DockerPreflightCode, message: string, detail: string | null = null) {
        super(message);
        this.name = 'DockerPreflightError';
        this.code = code;
        this.detail = detail;
    }
}

/** Everything one preflight needs; only Hub-local values describe this process. */
export interface DockerPreflightOptions {
    target: DockerTarget;
    /** Target installation used for both `run` and `manage`. */
    executable: string;
    /** Hub-side working directory and environment for the Docker CLI. */
    cwd: string;
    env: Record<string, string>;
    /** Effective advertised endpoints, after connectHost/origin overrides. */
    endpoints: WorkerEndpoints;
}

interface DockerCommandResult {
    code: number | null;
    signal: NodeJS.Signals | null;
    stdout: string;
    stderr: string;
    timedOut: boolean;
}

/** Keep diagnostics bounded; a Docker daemon can produce arbitrary output. */
const DIAGNOSTIC_BYTES = 8192;

/**
 * Time allowed after the deadline for a killed command's pipes to close.
 *
 * A wrapper can leave a descendant holding the inherited stdout/stderr, so the
 * `close` event may never arrive. The call settles anyway once this allowance
 * expires; the promise never waits on a pipe another process owns.
 */
export const DOCKER_PREFLIGHT_CLEANUP_MS = 250;

/**
 * Private CLI diagnostic: one line, no control characters, at most 200
 * characters. It never enters a public message or the default log.
 */
function diagnosticLine(text: string): string | null {
    const line = text.split('\n').map(part => part.trim()).find(Boolean) ?? '';
    return line ? line.replace(/[\u0000-\u001f\u007f]/g, ' ').slice(0, 200) : null;
}

/** Public CLI failure identity: exit status only, never CLI output. */
function failure(result: DockerCommandResult): string {
    if (result.signal) return `terminated by ${result.signal}`;
    return `exit ${result.code ?? 'unknown'}`;
}

/**
 * Controlled public hint for a common CLI refusal. The decision reads stderr,
 * but only a fixed sentence reaches the message; the raw line stays private.
 */
function failureHint(result: DockerCommandResult): string {
    if (/permission denied/i.test(result.stderr)) return '; check Docker CLI permissions';
    if (/(?:cannot connect|connection refused|is the docker daemon running)/i.test(result.stderr)) {
        return '; check that the Docker daemon is reachable';
    }
    return '';
}

/**
 * Run one Docker CLI command without a shell, bounded and with a deadline.
 *
 * The deadline kills the direct child; a fixed cleanup allowance then settles
 * the call even when a descendant keeps the pipes open. `error`, `close` and
 * both timers race only into the first settlement.
 */
function dockerCommand(
    target: DockerTarget,
    args: string[],
    { cwd, env, timeoutMs }: { cwd: string; env: Record<string, string>; timeoutMs: number },
): Promise<DockerCommandResult> {
    return new Promise((resolve, reject) => {
        let child;
        try {
            child = spawn(target.command, [...target.args, ...args], {
                cwd,
                env: { ...process.env, ...env },
                stdio: ['ignore', 'pipe', 'pipe'],
                shell: false,
            });
        } catch (error) {
            const detail = diagnosticLine(error instanceof Error ? error.message : String(error));
            reject(new DockerPreflightError('docker-unavailable',
                'cannot run the configured Docker command', detail));
            return;
        }
        const stdout: Buffer[] = [];
        const stderr: Buffer[] = [];
        let stdoutBytes = 0;
        let stderrBytes = 0;
        let timedOut = false;
        let deadlineExpired = false;
        let exitedBeforeDeadline = false;
        let exitCode: number | null = null;
        let exitSignal: NodeJS.Signals | null = null;
        let settled = false;
        let killTimer: NodeJS.Timeout | undefined;
        let settleTimer: NodeJS.Timeout | undefined;
        const finish = (error?: DockerPreflightError, code: number | null = null,
                        signal: NodeJS.Signals | null = null) => {
            if (settled) return;
            settled = true;
            clearTimeout(killTimer);
            clearTimeout(settleTimer);
            // A descendant may still hold the write ends; closing our read ends
            // is what makes settlement independent of it.
            child.stdout?.destroy();
            child.stderr?.destroy();
            if (error) {
                reject(error);
                return;
            }
            resolve({
                code,
                signal,
                stdout: Buffer.concat(stdout).toString('utf8'),
                stderr: Buffer.concat(stderr).toString('utf8'),
                // A command that exited on its own before the deadline is not a
                // timeout, even when a descendant kept `close` from arriving.
                timedOut: timedOut && !exitedBeforeDeadline,
            });
        };
        child.stdout?.on('data', (chunk: Buffer) => {
            if (stdoutBytes >= DIAGNOSTIC_BYTES) return;
            const slice = chunk.subarray(0, DIAGNOSTIC_BYTES - stdoutBytes);
            stdout.push(slice);
            stdoutBytes += slice.length;
        });
        child.stderr?.on('data', (chunk: Buffer) => {
            if (stderrBytes >= DIAGNOSTIC_BYTES) return;
            const slice = chunk.subarray(0, DIAGNOSTIC_BYTES - stderrBytes);
            stderr.push(slice);
            stderrBytes += slice.length;
        });
        child.once('error', (error: Error) => {
            finish(new DockerPreflightError('docker-unavailable',
                'cannot run the configured Docker command', diagnosticLine(error.message)));
        });
        child.once('exit', (code, signal) => {
            // `exit` does not wait for inherited pipes, so it records whether the
            // command finished on its own before the deadline killed it.
            exitedBeforeDeadline = !deadlineExpired;
            exitCode = code;
            exitSignal = signal;
        });
        child.once('close', (code, signal) => finish(undefined, code, signal));
        killTimer = setTimeout(() => {
            deadlineExpired = true;
            timedOut = true;
            try {
                child.kill('SIGKILL');
            } catch {
                // The allowance below still settles when the signal cannot be sent.
            }
            settleTimer = setTimeout(() => {
                // Reached when no close arrives within the allowance: the pipes
                // are held outside this process, but an exiting command already
                // reported its real status.
                finish(undefined,
                    exitedBeforeDeadline ? exitCode : null,
                    exitedBeforeDeadline ? exitSignal : null);
            }, DOCKER_PREFLIGHT_CLEANUP_MS);
            settleTimer.unref?.();
        }, timeoutMs);
        killTimer.unref?.();
    });
}

interface ContainerState {
    status: string;
    running: boolean;
    networkMode: string;
    extraHosts: string[];
}

/** Docker's closed container-state vocabulary; anything else is not echoed. */
const DOCKER_STATUSES = new Set([
    'created', 'running', 'paused', 'restarting', 'removing', 'exited', 'dead',
]);

/** Echo a status only when it is one Docker defines. */
function dockerStatus(value: string): string {
    return DOCKER_STATUSES.has(value) ? value : 'unknown';
}

/**
 * Echo only the built-in network modes and a syntactically valid container
 * reference. A user-defined network name is replaced by a fixed phrase, so a
 * CLI cannot place arbitrary text in a public message.
 */
function dockerNetworkMode(value: string): string {
    if (value === 'host' || value === 'none' || value === 'bridge' || value === 'default') {
        return value;
    }
    const container = value.startsWith('container:') ? value.slice('container:'.length) : '';
    return /^[A-Za-z0-9_.-]{1,64}$/.test(container) ? value : 'user-defined';
}

/** Parse the compact description produced by `docker inspect --format`. */
function parseContainerState(stdout: string): ContainerState {
    const [line] = stdout.split('\n');
    const fields = (line ?? '').split('\t');
    const status = fields[0]?.trim() ?? '';
    const networkMode = fields[2]?.trim() ?? '';
    if (!status || !networkMode) {
        throw new DockerPreflightError('inspect-failed',
            'Docker returned an unexpected container description', diagnosticLine(stdout));
    }
    let extraHosts: string[] = [];
    if (fields[3]) {
        try {
            const parsed: unknown = JSON.parse(fields[3]);
            if (Array.isArray(parsed)) extraHosts = parsed.filter((entry): entry is string => typeof entry === 'string');
        } catch {
            // Optional field: an unparsable list only disables the host-mapping
            // check, never the container/executable checks.
        }
    }
    return { status, running: fields[1]?.trim() === 'true', networkMode, extraHosts };
}

function bareHost(hostname: string): string {
    return hostname.replace(/^\[/, '').replace(/\]$/, '').toLowerCase();
}

function loopback(host: string): boolean {
    return host === 'localhost' || host === '::1' || host === '0.0.0.0'
        || host === '::' || /^127\./.test(host);
}

function portOf(url: URL): string {
    return url.port || (url.protocol === 'wss:' ? '443' : '80');
}

/**
 * Inspect whether one advertised channel is usable from the container network.
 * This is a configuration check, not a live connection attempt: it rejects the
 * two mistakes that always fail in practice — a loopback address seen from a
 * private network namespace, and an unmapped `host.docker.internal` name.
 */
function checkEndpoint(
    channel: DockerPreflightChannel,
    raw: string,
    state: ContainerState,
    container: string,
): void {
    let url: URL;
    try {
        url = new URL(raw);
    } catch {
        throw new DockerPreflightError('inspect-failed', `the advertised ${channel} endpoint is not a URL`);
    }
    if (state.networkMode === 'none') {
        throw new DockerPreflightError('endpoint-network-none',
            `container "${container}" has no network (network mode "none");`
            + ` it cannot reach the advertised ${channel} endpoint`);
    }
    const host = bareHost(url.hostname);
    // A container sharing another container's network namespace may also have
    // host loopback, but that namespace is not described here. Accept it as an
    // explicit operator assertion rather than a network this check can verify.
    if (loopback(host) && state.networkMode !== 'host' && !state.networkMode.startsWith('container:')) {
        throw new DockerPreflightError('endpoint-loopback',
            `the advertised ${channel} endpoint uses ${host}:${portOf(url)}, but container`
            + ` "${container}" uses network mode "${dockerNetworkMode(state.networkMode)}";`
            + ` set worker.connectHost or`
            + ` the ${channel} endpoint to an address reachable from the container`);
    }
    if (host === 'host.docker.internal'
        && !state.extraHosts.some(entry => entry.split(':')[0]?.toLowerCase() === host)) {
        throw new DockerPreflightError('endpoint-host-mapping',
            `container "${container}" has no host mapping for host.docker.internal; start it with`
            + ' --add-host=host.docker.internal:host-gateway or use a reachable bridge address');
    }
}

function policyOf(target: DockerTarget): DockerPreflightPolicy {
    return normalizeDockerPreflight(target.preflight);
}

/** Run one check sequence; resolves only when the target is usable. */
export async function preflightDockerTarget({
    target, executable, cwd, env, endpoints,
}: DockerPreflightOptions): Promise<void> {
    const policy = policyOf(target);
    if (!policy.enabled) return;
    const container = target.container;
    const inspect = await dockerCommand(target,
        ['inspect', '--format',
            '{{.State.Status}}\t{{.State.Running}}\t{{.HostConfig.NetworkMode}}\t{{json .HostConfig.ExtraHosts}}',
            container],
        { cwd, env, timeoutMs: policy.timeoutMs });
    if (inspect.timedOut) {
        throw new DockerPreflightError('inspect-timeout',
            `Docker did not answer within ${policy.timeoutMs} ms while inspecting container "${container}"`,
            diagnosticLine(inspect.stderr));
    }
    if (inspect.code !== 0) {
        if (/no such (?:object|container)/i.test(inspect.stderr)) {
            throw new DockerPreflightError('container-missing', `container "${container}" does not exist`);
        }
        throw new DockerPreflightError('inspect-failed',
            `cannot inspect container "${container}" (${failure(inspect)})${failureHint(inspect)}`,
            diagnosticLine(inspect.stderr));
    }
    const state = parseContainerState(inspect.stdout);
    // `container: false` opts out of the running assertion, not out of the
    // process compatibility it implies: an installation probe cannot run in a
    // stopped container, so report that state instead of a misleading CLI error.
    if (!state.running && (policy.container || policy.executable)) {
        throw new DockerPreflightError('container-not-running',
            `container "${container}" is not running (status: ${dockerStatus(state.status)})`);
    }
    if (policy.executable) {
        const probe = await dockerCommand(target, ['exec', container, executable, '--version'],
            { cwd, env, timeoutMs: policy.timeoutMs });
        if (probe.timedOut) {
            throw new DockerPreflightError('executable-timeout',
                `"${executable} --version" did not answer inside container "${container}"`
                + ` within ${policy.timeoutMs} ms`, diagnosticLine(probe.stderr));
        }
        if (probe.code === 126 || probe.code === 127) {
            throw new DockerPreflightError('executable-missing',
                `"${executable}" is not executable inside container "${container}" (${failure(probe)})`,
                diagnosticLine(probe.stderr));
        }
        if (probe.code !== 0) {
            throw new DockerPreflightError('executable-failed',
                `"${executable} --version" failed inside container "${container}" (${failure(probe)})`,
                diagnosticLine(probe.stderr));
        }
    }
    if (policy.endpoint.enabled) {
        for (const channel of policy.endpoint.channels) checkEndpoint(channel, endpoints[channel], state, container);
    }
}
