/**
 * Explicit target binding shared by worker-start and management invocations.
 * This adapter does not infer remote access from arbitrary command templates.
 * Session supervision switches to it only with durable allocation/lifecycle
 * ownership; legacy launchers remain unchanged until that integration lands.
 */
import { encodeManagementRequest, executeManagement, validateAllocation } from './management.ts';
import type {
    AllocationReference, ManagementInvocation, ManagementOperation, ManagementReply,
    ManagementRequest,
} from './management.ts';

/** Hub endpoint channels a Docker preflight can inspect. */
export const DOCKER_PREFLIGHT_CHANNELS = ['events', 'confirm', 'tools'] as const;

/** One advertised Hub channel. */
export type DockerPreflightChannel = (typeof DOCKER_PREFLIGHT_CHANNELS)[number];

/**
 * Checks performed against an operator-retained container before the first
 * target mutation. They never create, start, stop or remove a container.
 */
export interface DockerPreflightPolicy {
    /** False skips every check below; the target is then used as configured. */
    enabled: boolean;
    /** Deadline for one Docker CLI call, including a stalled daemon. */
    timeoutMs: number;
    /** Require the container to be running; existence is always checked. */
    container: boolean;
    /** Require `management.executable --version` to succeed inside it. */
    executable: boolean;
    /** Inspect whether the advertised Hub endpoints are usable from its network. */
    endpoint: { enabled: boolean; channels: DockerPreflightChannel[] };
}

/** Defaults: every check on, one advertised channel, a five-second deadline. */
export const DEFAULT_DOCKER_PREFLIGHT: DockerPreflightPolicy = {
    enabled: true,
    timeoutMs: 5000,
    container: true,
    executable: true,
    endpoint: { enabled: true, channels: ['events'] },
};

function preflightObject(value: unknown): Record<string, unknown> {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
        throw new TypeError('docker preflight must be a mapping');
    }
    return value as Record<string, unknown>;
}

function preflightBoolean(value: unknown, name: string, fallback: boolean): boolean {
    if (value === undefined) return fallback;
    if (typeof value !== 'boolean') throw new TypeError(`docker preflight.${name} must be a boolean`);
    return value;
}

function preflightKeys(value: Record<string, unknown>, allowed: readonly string[], name: string): void {
    for (const key of Object.keys(value)) {
        if (!allowed.includes(key)) throw new TypeError(`unknown docker preflight field: ${name}${key}`);
    }
}

/**
 * Validate one operator policy and fill omitted fields with the defaults.
 * Known fields are normalized so a restarted Hub reads the same checks.
 */
export function normalizeDockerPreflight(value: unknown): DockerPreflightPolicy {
    if (value === undefined) return structuredClone(DEFAULT_DOCKER_PREFLIGHT);
    const policy = preflightObject(value);
    preflightKeys(policy, ['enabled', 'timeoutMs', 'container', 'executable', 'endpoint'], '');
    const timeoutMs = policy.timeoutMs ?? DEFAULT_DOCKER_PREFLIGHT.timeoutMs;
    if (!Number.isSafeInteger(timeoutMs) || (timeoutMs as number) < 1 || (timeoutMs as number) > 2147483647) {
        throw new TypeError('docker preflight.timeoutMs must be an integer between 1 and 2147483647');
    }
    let endpoint = structuredClone(DEFAULT_DOCKER_PREFLIGHT.endpoint);
    if (policy.endpoint !== undefined) {
        const source = preflightObject(policy.endpoint);
        preflightKeys(source, ['enabled', 'channels'], 'endpoint.');
        endpoint = {
            enabled: preflightBoolean(source.enabled, 'endpoint.enabled', true),
            channels: structuredClone(endpoint.channels),
        };
        if (source.channels !== undefined) {
            if (!Array.isArray(source.channels) || source.channels.length === 0
                || source.channels.some(channel => !(DOCKER_PREFLIGHT_CHANNELS as readonly unknown[]).includes(channel))) {
                throw new TypeError(`docker preflight.endpoint.channels must list ${DOCKER_PREFLIGHT_CHANNELS.join(', ')}`);
            }
            const channels = source.channels as DockerPreflightChannel[];
            if (new Set(channels).size !== channels.length) {
                throw new TypeError('docker preflight.endpoint.channels must not repeat a channel');
            }
            endpoint.channels = [...channels];
        }
    }
    return {
        enabled: preflightBoolean(policy.enabled, 'enabled', true),
        timeoutMs: timeoutMs as number,
        container: preflightBoolean(policy.container, 'container', true),
        executable: preflightBoolean(policy.executable, 'executable', true),
        endpoint,
    };
}

/** Operator-controlled execution target. Remote paths are never resolved on the Hub. */
export type ManagementTarget =
    | { kind: 'local' }
    | {
        kind: 'ssh';
        /** SSH executable and connection options (identity, port, host-key policy). */
        command: string;
        args: string[];
        host: string;
    }
    | {
        kind: 'docker';
        /** Docker executable and global context/host options, shared with worker start. */
        command: string;
        args: string[];
        /** Existing running container, retained through worker shutdown and removal. */
        container: string;
        /** Startup checks; omitted means the defaults above. */
        preflight?: DockerPreflightPolicy;
    };

/** Serializable operator policy; keep credentials out of public session metadata. */
export interface ManagedLauncherOptions {
    target: ManagementTarget;
    /** Public simplex executable on the target; both run and manage use this installation. */
    executable: string;
    /** Working directory of the transport process on the Hub, never the allocation root. */
    cwd: string;
    /** Hub-side environment for local execution or transport credentials/context. */
    env: Record<string, string>;
}

/** Quote one argument for the POSIX shell used by the SSH server. */
function shellQuote(value: string): string {
    return `'${value.replaceAll("'", "'\\''")}'`;
}

function validArgument(value: string): boolean {
    return value.length > 0 && !value.includes('\0');
}

/**
 * Low-level launcher adapter. Hub never creates, starts, stops or removes a
 * container; SSH worker exit verification remains a supervisor concern, and
 * transport process exit alone is never proof of remote worker death.
 */
export class ManagedLauncher {
    private readonly options: ManagedLauncherOptions;

    constructor(options: ManagedLauncherOptions) {
        const target = options.target;
        if (!validArgument(options.executable) || options.executable.startsWith('-')
            || !validArgument(options.cwd)) throw new TypeError('invalid managed launcher paths');
        if (target.kind !== 'local') {
            const destination = target.kind === 'ssh' ? target.host : target.container;
            if (!validArgument(target.command) || !validArgument(destination) || destination.startsWith('-')
                || target.args.some(arg => arg.includes('\0'))) {
                throw new TypeError('invalid management transport');
            }
        }
        // Reject an unusable startup policy here, before a session can bind an
        // allocation that it later refuses to launch.
        if (target.kind === 'docker' && target.preflight !== undefined) normalizeDockerPreflight(target.preflight);
        // Retain a private copy so later config/UI changes cannot switch targets
        // halfway through a prepare/capture/run/remove lifecycle.
        this.options = structuredClone(options);
    }

    /** Build one no-PTY invocation, using exactly the same target for both commands. */
    private invocation(args: string[]): ManagementInvocation {
        const { target, executable, cwd, env } = this.options;
        let command = executable;
        let arguments_ = args;
        if (target.kind === 'ssh') {
            command = target.command;
            // Final -T overrides earlier tty options. Shell expansion is disabled
            // for every remote argv element, including executable paths with spaces.
            arguments_ = [...target.args, '-T', '--', target.host,
                [executable, ...args].map(shellQuote).join(' ')];
        } else if (target.kind === 'docker') {
            command = target.command;
            arguments_ = [...target.args, 'exec', target.container, executable, ...args];
        }
        return { command, args: arguments_, cwd, env: { ...env } };
    }

    /** Build a management command without inspecting any target file on the Hub. */
    buildManagementInvocation(request: ManagementRequest): ManagementInvocation {
        return this.invocation(['manage', '--request', encodeManagementRequest(request)]);
    }

    /** Execute one request; callers retain the reference/request identity for reconciliation. */
    manage(
        reference: AllocationReference, requestId: string, operation: ManagementOperation, signal?: AbortSignal,
    ): Promise<ManagementReply> {
        const request: ManagementRequest = {
            ...operation,
            version: 1,
            request_id: requestId,
            allocation_id: reference.allocationId,
            directory: reference.directory,
        };
        return executeManagement(this.buildManagementInvocation(request), requestId, signal);
    }

    /**
     * Build worker start from the manager's returned config path. Target paths
     * stay literal, including SSH metacharacters. The supervisor validates and
     * captures the prepared allocation before using this invocation.
     */
    buildWorkerInvocation(reference: AllocationReference, configPath: string, args: string[]): ManagementInvocation {
        validateAllocation(reference);
        if (configPath !== `${reference.directory}/config/config.yaml`) {
            throw new TypeError('worker configuration does not belong to the allocation');
        }
        if (args.some(arg => arg.includes('\0') || arg.startsWith('-c')
            || /^--(?:c|co|con|conf|confi|config)(?:=|$)/.test(arg))) {
            throw new TypeError('managed worker arguments must not override its configuration');
        }
        return this.invocation(['run', '--config', configPath, ...args]);
    }
}
