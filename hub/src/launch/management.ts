/** Target-local management protocol and uncapped asynchronous pipe transport. */
import { spawn } from 'node:child_process';
import { TextDecoder } from 'node:util';

/** Stable storage identity. Paths are POSIX paths on the worker target, not Hub paths. */
export interface AllocationReference {
    allocationId: string;
    directory: string;
}

/** Only these fields may be refreshed without replacing the source configuration. */
export interface ConnectionRefresh {
    client_endpoint?: string;
    confirmation_endpoint?: string;
    tools_endpoint?: string;
}

/** Operations supported by the current manager; clean-fork is a later extension. */
export type ManagementOperation =
    | { operation: 'prepare'; configuration?: string; refresh?: ConnectionRefresh; expected_revision?: number }
    | { operation: 'inspect' }
    | { operation: 'capture_startup'; lifecycle_id: string; expected_revision: number }
    | { operation: 'read_state' }
    | { operation: 'remove' };

/** Complete wire request. IDs must remain stable when reconciling uncertain mutations. */
export type ManagementRequest = ManagementOperation & {
    version: 1;
    request_id: string;
    allocation_id: string;
    directory: string;
};

/** An explicit transport command; cwd and env always belong to the Hub-side process. */
export interface ManagementInvocation {
    command: string;
    args: string[];
    cwd: string;
    env: Record<string, string>;
}

/**
 * A trusted manager's completed result. Do not log this object: inspection can
 * return credentials and complete state. Operation consumers validate result DTOs.
 */
export interface ManagementReply {
    result: Record<string, unknown>;
    /** Captured separately from stdout; private diagnostics, never public event data. */
    diagnostics: string;
}

/**
 * Transport/protocol errors have an uncertain mutation outcome. A process may
 * publish successfully and lose its reply. Never infer absence or retry with a
 * new allocation ID from these errors. Error.message deliberately excludes argv,
 * raw stdout, stderr and peer-provided text, which may contain credentials.
 */
export class ManagementError extends Error {
    readonly kind: 'transport' | 'protocol' | 'manager';
    readonly code: string;
    readonly outcome: 'unknown' | 'reported';
    readonly published: boolean;
    /** Private peer diagnostic, available for deliberate operator inspection only. */
    readonly detail: string | null;
    readonly diagnostics: string;

    constructor(
        kind: 'transport' | 'protocol' | 'manager',
        code: string,
        diagnostics = '',
        detail: string | null = null,
        published = false,
    ) {
        super(kind === 'manager' ? `worker management failed (${code})`
            : `worker management ${kind} failure (${code}); operation outcome is unknown`);
        this.name = 'ManagementError';
        this.kind = kind;
        this.code = code;
        this.outcome = kind === 'manager' ? 'reported' : 'unknown';
        this.published = published;
        this.detail = detail;
        this.diagnostics = diagnostics;
    }
}

/** Reject malformed identities before transmitting secrets to the configured peer. */
export function validateAllocation(reference: AllocationReference): void {
    if (!/^[A-Za-z0-9_-]{1,128}$/.test(reference.allocationId)) {
        throw new TypeError('invalid allocation identity');
    }
    const path = reference.directory;
    if (!path.startsWith('/') || path === '/' || path.includes('\0')
        || path.split('/').slice(1).some(part => !part || part === '.' || part === '..')) {
        throw new TypeError('allocation directory must be a normalized absolute target path');
    }
}

/** Encode exactly one UTF-8 JSON request for argv; never log the returned string. */
export function encodeManagementRequest(request: ManagementRequest): string {
    validateAllocation({ allocationId: request.allocation_id, directory: request.directory });
    if (request.version !== 1 || !/^[A-Za-z0-9_-]{1,128}$/.test(request.request_id)) {
        throw new TypeError('invalid management request identity or version');
    }
    return Buffer.from(JSON.stringify(request), 'utf8').toString('base64');
}

function object(value: unknown): value is Record<string, unknown> {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

/** Validate the envelope only after exit and pipe EOF, never on a partial success line. */
function decodeReply(
    bytes: Buffer, diagnostics: string, requestId: string, exitCode: number | null,
    signal: NodeJS.Signals | null,
): ManagementReply {
    if (signal || exitCode === null) throw new ManagementError('transport', 'terminated', diagnostics);
    let envelope: unknown;
    try {
        envelope = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
    } catch {
        throw new ManagementError(exitCode === 0 ? 'protocol' : 'transport', 'invalid_response', diagnostics);
    }
    if (!object(envelope) || envelope.version !== 1 || envelope.request_id !== requestId
        || typeof envelope.ok !== 'boolean') {
        throw new ManagementError('protocol', 'invalid_envelope', diagnostics);
    }
    if (envelope.ok) {
        if (exitCode !== 0) throw new ManagementError('transport', 'nonzero_exit', diagnostics);
        if (!object(envelope.result) || 'error' in envelope) {
            throw new ManagementError('protocol', 'invalid_result', diagnostics);
        }
        return { result: envelope.result, diagnostics };
    }
    const error = envelope.error;
    if (exitCode === 0 || !object(error) || typeof error.code !== 'string'
        || !/^[a-z][a-z0-9_]{0,63}$/.test(error.code) || typeof error.message !== 'string'
        || ('published' in error && typeof error.published !== 'boolean') || 'result' in envelope) {
        throw new ManagementError('protocol', 'invalid_error', diagnostics);
    }
    throw new ManagementError('manager', error.code, diagnostics, error.message, error.published === true);
}

/**
 * Spawn without a shell or PTY and drain both pipes concurrently through close.
 * There is deliberately no byte limit, maxBuffer, truncation, automatic retry,
 * default timeout, or local-filesystem fallback. A supplied abort signal kills
 * only the transport and leaves the remote outcome explicitly uncertain. The
 * configured peer is trusted. The owner must reconcile an interrupted mutation
 * before proceeding with lifecycle changes.
 */
export async function executeManagement(
    invocation: ManagementInvocation, requestId: string, signal?: AbortSignal,
): Promise<ManagementReply> {
    return new Promise((resolve, reject) => {
        const stdout: Buffer[] = [];
        const stderr: Buffer[] = [];
        let failure: 'spawn_failed' | 'pipe_failed' | 'cancelled' | null = null;
        if (signal?.aborted) { reject(new ManagementError('transport', 'cancelled')); return; }
        let child;
        try {
            child = spawn(invocation.command, invocation.args, {
                cwd: invocation.cwd,
                env: { ...process.env, ...invocation.env },
                stdio: ['ignore', 'pipe', 'pipe'],
                shell: false,
            });
        } catch {
            reject(new ManagementError('transport', 'spawn_failed'));
            return;
        }
        const abort = () => {
            failure = 'cancelled';
            // Terminate only our transport. A remote mutation may still finish;
            // destroying pipes cannot imply rollback or authorize worker deletion.
            child.kill('SIGKILL');
            child.stdout.destroy();
            child.stderr.destroy();
        };
        signal?.addEventListener('abort', abort, { once: true });
        if (signal?.aborted) abort();
        child.on('error', () => { if (!failure) failure = 'spawn_failed'; });
        child.stdout.on('data', (chunk: Buffer) => stdout.push(chunk));
        child.stderr.on('data', (chunk: Buffer) => stderr.push(chunk));
        for (const pipe of [child.stdout, child.stderr]) {
            pipe.on('error', () => { failure = 'pipe_failed'; });
        }
        child.once('close', (code, exitSignal) => {
            signal?.removeEventListener('abort', abort);
            const diagnostics = Buffer.concat(stderr).toString('utf8');
            if (failure) {
                reject(new ManagementError('transport', failure, diagnostics));
                return;
            }
            try {
                resolve(decodeReply(Buffer.concat(stdout), diagnostics, requestId, code, exitSignal));
            } catch (error) {
                reject(error);
            }
        });
    });
}
