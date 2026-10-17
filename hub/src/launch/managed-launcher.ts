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
 * Low-level launcher adapter. Docker creation/retirement and SSH worker exit
 * verification remain supervisor responsibilities; transport process exit alone
 * is never proof of remote worker death. No management call creates a container.
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
        reference: AllocationReference, requestId: string, operation: ManagementOperation,
    ): Promise<ManagementReply> {
        const request: ManagementRequest = {
            ...operation,
            version: 1,
            request_id: requestId,
            allocation_id: reference.allocationId,
            directory: reference.directory,
        };
        return executeManagement(this.buildManagementInvocation(request), requestId);
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
