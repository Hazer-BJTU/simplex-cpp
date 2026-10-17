/** Durable Hub-side references to manager-owned target allocations. */
import { randomUUID } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, realpathSync, openSync, closeSync, fsyncSync, writeFileSync, renameSync, rmSync } from 'node:fs';
import { dirname, join, resolve, sep, isAbsolute } from 'node:path';
import { ManagedLauncher } from './managed-launcher.ts';
import type { ManagedLauncherOptions } from './managed-launcher.ts';
import { ManagementError, validateAllocation } from './management.ts';
import type { AllocationReference, ManagementOperation, ForkSource } from './management.ts';
import { sessionDir } from './config-render.ts';
import type { HubConfig } from '../config.ts';
import type { WorkerEndpoints } from './invocation.ts';

/** Operator-owned launch policy. The root belongs to the target, not the Hub. */
export interface ManagementPolicy extends ManagedLauncherOptions {
    root: string;
    /** Deadline per management subprocess; defaults to 30 seconds, never a byte budget. */
    timeoutMs?: number;
}

/** Immutable target/source binding, published locally before any target mutation. */
export interface ManagedSessionRecord {
    version: 1;
    reference: AllocationReference;
    launcher: ManagedLauncherOptions;
    configuration: string;
    /** Published before dispatch; target derives the immutable child configuration. */
    forkSource?: ForkSource;
    timeoutMs: number;
    /** Last captured lifecycle; used as an exclusive manager lease probe on remote stop. */
    lifecycle?: { id: string; revision: number; dispatch?: 'prepared' | 'pending' | 'completed' };
    /** Persist before removal; failures keep this record available for retry. */
    removing: boolean;
}

function mapping(value: unknown): value is Record<string, unknown> {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

/** Validate operator JSON before it can become an executable invocation. */
export function managementPolicy(value: unknown): ManagementPolicy {
    if (!mapping(value) || !mapping(value.target) || typeof value.root !== 'string'
        || typeof value.executable !== 'string' || typeof value.cwd !== 'string'
        || !mapping(value.env) || Object.values(value.env).some(v => typeof v !== 'string')) {
        throw new Error('management requires root, executable, cwd, env and target');
    }
    const target = value.target;
    if (!['local', 'ssh', 'docker'].includes(String(target.kind))) throw new Error('invalid management target kind');
    if (target.kind !== 'local' && (typeof target.command !== 'string' || !Array.isArray(target.args)
        || target.args.some(v => typeof v !== 'string')
        || typeof target[target.kind === 'ssh' ? 'host' : 'container'] !== 'string')) {
        throw new Error('invalid management transport fields');
    }
    const policy = structuredClone(value) as unknown as ManagementPolicy;
    validateAllocation({ allocationId: 'validation', directory: policy.root });
    if (!isAbsolute(policy.cwd)) throw new Error('management.cwd must be an absolute Hub-local directory');
    if (policy.timeoutMs !== undefined && (!Number.isSafeInteger(policy.timeoutMs)
        || policy.timeoutMs < 1 || policy.timeoutMs > 2147483647)) throw new Error('invalid management.timeoutMs');
    new ManagedLauncher(policy);
    return policy;
}

/** Resolve existing ancestors to prevent a local symlink alias from overlapping Hub data. */
function canonical(path: string): string {
    let existing = resolve(path);
    const suffix: string[] = [];
    while (!existsSync(existing)) {
        const parent = dirname(existing);
        suffix.unshift(existing.slice(parent.length).replace(/^[/\\]/, ''));
        existing = parent;
    }
    return join(realpathSync(existing), ...suffix);
}

/** Local managed roots must not contain, or be contained by, the Hub data tree. */
export function separateLocalRoot(config: Pick<HubConfig, 'dataDir'>, policy: ManagementPolicy): void {
    if (policy.target.kind !== 'local') return;
    const hub = canonical(config.dataDir);
    const root = canonical(policy.root);
    if (root === hub || root.startsWith(`${hub}${sep}`) || hub.startsWith(`${root}${sep}`)) {
        throw new Error('management.root must be outside and separate from Hub dataDir');
    }
}

/** This file is Hub bookkeeping. Never pass its parent to manager.remove(). */
export function managedRecordPath(config: Pick<HubConfig, 'dataDir'>, id: string): string {
    return join(sessionDir(config, id), 'allocation.json');
}

/** Missing is distinct from invalid; corrupt references must never select legacy deletion. */
export function readManagedSession(config: Pick<HubConfig, 'dataDir'>, id: string): ManagedSessionRecord | null {
    const path = managedRecordPath(config, id);
    if (!existsSync(path)) return null;
    let value: unknown;
    try { value = JSON.parse(readFileSync(path, 'utf8')); }
    catch { throw new Error('invalid managed session reference; repair before retrying'); }
    if (!mapping(value) || value.version !== 1 || !mapping(value.reference)
        || typeof value.reference.allocationId !== 'string' || typeof value.reference.directory !== 'string'
        || !mapping(value.launcher) || typeof value.configuration !== 'string'
        || typeof value.removing !== 'boolean' || !Number.isSafeInteger(value.timeoutMs)
        || (value.timeoutMs as number) < 1 || (value.timeoutMs as number) > 2147483647) throw new Error('invalid managed session reference');
    const record = value as unknown as ManagedSessionRecord;
    validateAllocation(record.reference);
    const policy = managementPolicy({ ...record.launcher, root: dirname(record.reference.directory) });
    separateLocalRoot(config, policy);
    if (record.lifecycle && (typeof record.lifecycle.id !== 'string'
        || !/^[A-Za-z0-9_-]{1,128}$/.test(record.lifecycle.id)
        || !Number.isSafeInteger(record.lifecycle.revision) || record.lifecycle.revision < 1
        || (record.lifecycle.dispatch !== undefined && !['prepared', 'pending', 'completed'].includes(record.lifecycle.dispatch)))) {
        throw new Error('invalid managed lifecycle reference');
    }
    if (record.forkSource !== undefined) {
        const source: unknown = record.forkSource;
        if (!mapping(source) || typeof source.allocation_id !== 'string'
            || typeof source.directory !== 'string' || typeof source.lifecycle_id !== 'string'
            || !/^[A-Za-z0-9_-]{1,128}$/.test(source.lifecycle_id)
            || !Number.isSafeInteger(source.revision) || (source.revision as number) < 1) {
            throw new Error('invalid managed fork source');
        }
        validateAllocation({ allocationId: source.allocation_id, directory: source.directory });
    }
    return record;
}

function publish(config: Pick<HubConfig, 'dataDir'>, id: string, record: ManagedSessionRecord): void {
    const path = managedRecordPath(config, id);
    mkdirSync(dirname(path), { recursive: true, mode: 0o700 });
    const temporary = `${path}.${randomUUID()}.tmp`;
    try {
        const fd = openSync(temporary, 'wx', 0o600);
        try { writeFileSync(fd, JSON.stringify(record)); fsyncSync(fd); }
        finally { closeSync(fd); }
        renameSync(temporary, path);
        const directory = openSync(dirname(path), 'r');
        try { fsyncSync(directory); }
        finally { closeSync(directory); }
    } finally { rmSync(temporary, { force: true }); }
}

/** Allocate a fresh target identity only once, even when prepare's reply is lost. */
export function bindManagedSession(config: HubConfig, id: string, policy: ManagementPolicy,
    configuration: string, forkSource?: ForkSource): ManagedSessionRecord {
    const previous = readManagedSession(config, id);
    if (previous) {
        if (previous.removing) throw new Error('managed session removal is pending; retry deletion');
        return previous;
    }
    const local = sessionDir(config, id);
    if (existsSync(join(local, 'session.lock')) || existsSync(join(local, 'config', 'startup-worker.yaml'))
        || existsSync(join(local, 'state'))) {
        throw new Error('legacy worker data requires explicit migration; create a fresh managed session');
    }
    separateLocalRoot(config, policy);
    const allocationId = randomUUID();
    const { root, timeoutMs = 30000, ...launcher } = policy;
    const record: ManagedSessionRecord = { version: 1, reference: {
        allocationId, directory: `${root}/${allocationId}`,
    }, launcher, configuration, timeoutMs, removing: false, ...(forkSource ? { forkSource } : {}) };
    publish(config, id, record);
    return record;
}

async function call(record: ManagedSessionRecord, operation: ManagementOperation, timeoutMs = record.timeoutMs): Promise<Record<string, unknown>> {
    return (await new ManagedLauncher(record.launcher).manage(record.reference, randomUUID(), operation, AbortSignal.timeout(timeoutMs))).result;
}

/** Validate operation-specific identity; correlated envelopes alone cannot authorize paths. */
function description(record: ManagedSessionRecord, value: Record<string, unknown>, status: string): void {
    if (value.allocation_id !== record.reference.allocationId || value.directory !== record.reference.directory
        || value.config_path !== `${record.reference.directory}/config/config.yaml` || value.status !== status
        || !Number.isSafeInteger(value.revision) || (value.revision as number) < 1) {
        throw new Error('manager returned an invalid allocation description; outcome is unknown');
    }
}

/** Inspect before refreshing, then capture exactly the configuration revision to launch. */
export async function prepareManagedSession(config: HubConfig, id: string, record: ManagedSessionRecord,
    endpoints: WorkerEndpoints, lifecycleId: string): Promise<Record<string, unknown>> {
    if (record.removing) throw new Error('managed session removal is pending; retry deletion');
    if (managedStartUnconfirmed(record)) throw new Error('previous managed start is unconfirmed; cannot dispatch another worker');
    if (record.forkSource) {
        const forked = await call(record, { operation: 'clean_fork', source: record.forkSource });
        description(record, forked, 'ready');
        if (typeof forked.configuration !== 'string') throw new Error('manager omitted fork configuration');
        record.configuration = forked.configuration;
        publish(config, id, record);
    }
    let revision: number | undefined;
    try {
        const current = await call(record, { operation: 'inspect' });
        description(record, current, current.status === 'preparing' ? 'preparing' : 'ready');
        revision = current.revision as number;
    } catch (error) {
        if (!(error instanceof ManagementError) || error.kind !== 'manager' || error.code !== 'not_found') throw error;
    }
    const prepared = await call(record, { operation: 'prepare', configuration: record.configuration,
        refresh: { client_endpoint: endpoints.events, confirmation_endpoint: endpoints.confirm, tools_endpoint: endpoints.tools },
        ...(revision === undefined ? {} : { expected_revision: revision }),
    });
    description(record, prepared, 'ready');
    if (!mapping(prepared.effective_configuration)) throw new Error('manager omitted effective configuration');
    const lifecycle = { id: lifecycleId, revision: prepared.revision as number, dispatch: 'prepared' as const };
    // Store the lifecycle before capture: a lost capture reply can be reconciled.
    record.lifecycle = lifecycle;
    publish(config, id, record);
    const captured = await call(record, { operation: 'capture_startup', lifecycle_id: lifecycle.id,
        expected_revision: lifecycle.revision });
    description(record, captured, 'ready');
    if (captured.revision !== lifecycle.revision || !mapping(captured.startup)
        || captured.startup.lifecycle_id !== lifecycle.id) throw new Error('manager startup capture does not match this lifecycle');
    return prepared.effective_configuration;
}

/** Missing dispatch state in an older record is conservative: it may already have launched. */
export function managedStartUnconfirmed(record: ManagedSessionRecord): boolean {
    return Boolean(record.lifecycle && record.lifecycle.dispatch !== 'prepared'
        && record.lifecycle.dispatch !== 'completed');
}

/** Persist before spawn, covering a crash before hub.json records the process. */
export function markManagedDispatch(config: HubConfig, id: string, record: ManagedSessionRecord): void {
    if (!record.lifecycle || record.lifecycle.dispatch !== 'prepared') {
        throw new Error('managed lifecycle is not prepared for dispatch');
    }
    record.lifecycle.dispatch = 'pending';
    publish(config, id, record);
}

/**
 * Only call with evidence that the dispatched foreground command has ended.
 * Idle lease probes and transport loss are NOT such evidence. Reject stale
 * callbacks from an earlier lifecycle and never recreate a deleted local record.
 */
export function completeManagedDispatch(config: HubConfig, id: string, record: ManagedSessionRecord): void {
    const current = readManagedSession(config, id);
    if (!current || current.removing || !record.lifecycle || !current.lifecycle
        || current.reference.allocationId !== record.reference.allocationId
        || current.lifecycle.id !== record.lifecycle.id) return;
    current.lifecycle.dispatch = 'completed';
    publish(config, id, current);
    record.lifecycle.dispatch = 'completed';
}

/** Only a present-time lease probe. It never rules out a dispatched command acquiring its lease later. */
export async function managedIdle(record: ManagedSessionRecord, timeoutMs = record.timeoutMs): Promise<boolean> {
    if (!record.lifecycle) return false;
    try {
        const result = await call(record, { operation: 'capture_startup', lifecycle_id: record.lifecycle.id,
            expected_revision: record.lifecycle.revision }, timeoutMs);
        description(record, result, 'ready');
        return true;
    } catch { return false; }
}

/** Backend-neutral complete snapshot. Never open a target file or fall back to a local copy. */
export async function readManagedState(record: ManagedSessionRecord): Promise<{ state: unknown; readable: string | null }> {
    const result = await call(record, { operation: 'read_state' });
    if (typeof result.backend !== 'string' || !(result.state === null || mapping(result.state))
        || !(result.readable === null || typeof result.readable === 'string')) {
        throw new Error('manager returned an invalid state projection');
    }
    return { state: result.state, readable: result.readable };
}

/** Persist cleanup intent first. Keep it on all failures, including lost success replies. */
export async function removeManagedSession(config: HubConfig, id: string, record: ManagedSessionRecord): Promise<void> {
    record.removing = true;
    publish(config, id, record);
    // Finish an uncertain initial preparation before removal. We cannot treat
    // not_found as successful deletion: prepare may have been delayed remotely.
    try {
        await call(record, record.forkSource
            ? { operation: 'clean_fork', source: record.forkSource }
            : { operation: 'prepare', configuration: record.configuration });
    } catch (error) {
        if (!(error instanceof ManagementError) || error.kind !== 'manager' || error.code !== 'retired') throw error;
    }
    const result = await call(record, { operation: 'remove' });
    description(record, result, 'removed');
}
