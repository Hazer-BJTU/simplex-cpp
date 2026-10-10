import { readAnswer } from '../worker/answers.ts';
import { answerQuery } from '../../shared/answers.ts';
/** Session-level family supervisor and the fixed subagent remote operations. */
import { createHash, randomUUID } from 'node:crypto';
import type { HubConfig } from '../config.ts';
import type { Logger } from '../log.ts';
import type { Session, SessionRegistry } from '../state/registry.ts';
import { HubState } from '../state/persist.ts';
import { processIdentity } from '../launch/supervisor.ts';
import type { WorkerSupervisor, ProcessRecord, StopResult } from '../launch/supervisor.ts';
import type { ForwardedEnvelope, WorkerConnection } from '../worker/connection.ts';
import type { ToolContext } from '../worker/tool-context.ts';
import { ToolFailure } from '../worker/tool-context.ts';
import { newRequestId, buildPayload } from '../protocol/messages.ts';
import { childDirectories, ownedPath, readPrivate, writePrivate, removeChildDirectory } from './storage.ts';
import { dockerName, dockerRunning } from './docker.ts';
import { cleanFork } from './fork.ts';
import { ConversationProjection } from './conversation.ts';
import { ProjectionFlush } from './projection-flush.ts';
import { isValidSessionId } from '../state/session-id.ts';

interface ParentRef { session_id: string; lifecycle_id: string; worker_id: string }
/** One startup/timer pass shares its clock and attempt set across the family. */
interface AutomaticCleanup {
    now: number;
    attempted: Set<string>;
}
interface ChildRecord {
    session: Session;
    parent: ParentRef;
    conversation: ConversationProjection | null;
    removed: boolean;
    startup: Promise<void> | null;
    startupTimer: NodeJS.Timeout | null;
    terminalTimer: NodeJS.Timeout | null;
    error: string;
    uncertainStart: boolean;
    /** Durable evidence or explicit operator attestation, never inferred from no record. */
    terminationConfirmed: boolean;
    cleanupAttempts: number;
    retryAt: number;
}
interface Receipt {
    key: string;
    digest: string;
    route: string;
    at: number;
    result: Record<string, unknown>;
}
interface Operation {
    request_id: string;
    operation: string;
    state: 'intent' | 'sent' | 'admitted' | 'rejected' | 'unknown' | 'finished';
    run_id: string;
    status?: string;
    detail?: string;
    summary?: string;
    at: string;
}
interface Operations { receipts: Receipt[]; requests: Operation[] }
const MAX_OPERATION_BYTES = 512 * 1024;
const MAX_RPC_BYTES = 256 * 1024;
const MAX_CLEANUP_ATTEMPTS = 3;
const object = (value: unknown): Record<string, unknown> | null =>
    value && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : null;

/** Canonical JSON keys make equivalent object key order reuse the same receipt. */
function canonical(value: unknown): string {
    if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
    if (value !== null && typeof value === 'object') {
        return `{${Object.entries(value).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)
            .map(([key, item]) => `${JSON.stringify(key)}:${canonical(item)}`).join(',')}}`;
    }
    return JSON.stringify(value);
}
function exact(args: Record<string, unknown>, keys: string[]): void {
    if (Object.keys(args).some(key => !keys.includes(key))) throw new ToolFailure('invalid_arguments', 'unexpected subagent argument');
}

export interface SubagentServiceOptions {
    config: HubConfig;
    registry: SessionRegistry;
    supervisor: WorkerSupervisor;
    log: Logger;
    changed: (session: Session) => void;
    removed: (id: string) => void;
}

/**
 * All admission/receipt commits run synchronously on the Hub event loop.
 * Transport cancellation before commit prevents work; after commit lifecycle
 * work belongs to this supervisor and is observable through receive.
 */
export class SubagentService {
    readonly children = new Map<string, ChildRecord>();
    private operations = new Map<string, Operations>();
    private stops = new Map<string, Promise<StopResult>>();
    private closing = false;
    private recovering = false;
    private recoveryBlocked = false;
    private retryTimer: NodeJS.Timeout | null = null;
    private readonly metadataFlushes = new Map<string, ProjectionFlush>();
    private readonly metadataStorageFailures = new Set<string>();
    readonly options: SubagentServiceOptions;
    constructor(options: SubagentServiceOptions) {
        this.options = options;
        options.supervisor.cascade = (session, stopSelf) => this.cascade(session, stopSelf);
        options.supervisor.beforeStart = session => {
            if (this.recoveryBlocked) throw new Error('subagent ownership recovery is blocked; inspect managed storage before starting workers');
            const childrenRemain = session.closing && [...this.children.values()].some(record =>
                !record.removed && record.parent.session_id === session.id);
            if (this.closing || this.stops.has(session.id) || childrenRemain) {
                throw new Error('session lifecycle is stopping or requires descendant cleanup');
            }
        };
    }

    /** Lifecycle/ownership/policy changes supersede any queued observation immediately. */
    private publish(record: ChildRecord): void {
        const { session } = record;
        this.metadataFlushes.get(session.id)?.cancel();
        if (!record.removed) {
            const stored = new HubState({ config: this.options.config, log: this.options.log })
                .sessionDocument(session);
            writePrivate(ownedPath(this.options.config.dataDir, session.id, 'metadata.json'), {
                ...stored, kind: 'headless', cascading_parent: record.parent,
                lifecycle_id: session.lifecycleId, subagent: session.subagent,
                error: record.error, uncertain_start: record.uncertainStart,
                termination_confirmed: record.terminationConfirmed === true,
                cleanup_attempts: record.cleanupAttempts ?? 0, retry_at: record.retryAt ?? 0,
            }, MAX_OPERATION_BYTES);
            this.metadataStorageFailures.delete(session.id);
        }
        this.options.changed(session);
    }

    /** Event observations are reconstructible; batch their disk copy and list broadcast. */
    private schedulePublication(record: ChildRecord): void {
        if (this.closing || record.removed || record.session.closing) return;
        const id = record.session.id;
        let flush = this.metadataFlushes.get(id);
        if (!flush) {
            flush = new ProjectionFlush(() => {
                if (!record.removed) this.publish(record);
            }, () => {
                this.metadataStorageFailures.add(id);
                record.session.subagent!.health = 'degraded';
                record.session.subagent!.reason = record.conversation?.storageFailed
                    ? 'primary conversation storage is unavailable' : 'subagent metadata storage is unavailable';
                this.options.log.warn(`subagent ${id}: metadata storage failed`);
                this.options.changed(record.session);
            });
            this.metadataFlushes.set(id, flush);
        }
        flush.schedule();
    }

    /** Report degraded storage without depending on another successful disk write. */
    private projectionStorageFailed(session: Session): void {
        if (session.closing || this.children.get(session.id)?.removed) return;
        session.subagent!.health = 'degraded';
        session.subagent!.reason = 'primary conversation storage is unavailable';
        this.options.log.warn(`subagent ${session.id}: primary conversation storage failed`);
        this.options.changed(session);
    }

    private ops(session: Session): Operations {
        const existing = this.operations.get(session.id);
        if (existing) return existing;
        const saved = object(readPrivate(ownedPath(this.options.config.dataDir, session.id, 'operations.json'), MAX_OPERATION_BYTES));
        const result: Operations = { receipts: [], requests: [] };
        if (saved) {
            if (!Array.isArray(saved.receipts) || !Array.isArray(saved.requests)) throw new Error('invalid operations state');
            for (const raw of saved.receipts.slice(-this.options.config.subagents.maxReceipts)) {
                const receipt = object(raw);
                if (!receipt || typeof receipt.key !== 'string' || typeof receipt.digest !== 'string'
                    || typeof receipt.route !== 'string' || typeof receipt.at !== 'number' || !object(receipt.result)) {
                    throw new Error('invalid operation receipt');
                }
                result.receipts.push(receipt as unknown as Receipt);
            }
            for (const raw of saved.requests.slice(-200)) {
                const entry = object(raw);
                if (!entry || typeof entry.request_id !== 'string' || typeof entry.operation !== 'string'
                    || typeof entry.run_id !== 'string' || typeof entry.at !== 'string') throw new Error('invalid request state');
                const request = entry as unknown as Operation;
                if (['intent', 'sent', 'admitted'].includes(request.state)) request.state = 'unknown';
                result.requests.push(request);
            }
        }
        this.operations.set(session.id, result);
        return result;
    }

    private saveOps(session: Session, operations = this.ops(session)): void {
        const child = this.children.get(session.id);
        if (child?.removed) return;
        // Whole older request outcomes expire before the file can grow unbounded.
        while (Buffer.byteLength(JSON.stringify(operations)) >= MAX_OPERATION_BYTES && operations.requests.length > 1) {
            operations.requests.shift();
        }
        writePrivate(ownedPath(this.options.config.dataDir, session.id, 'operations.json'), operations, MAX_OPERATION_BYTES);
    }

    private receipt(context: ToolContext): { operations: Operations; key: string; digest: string; reused?: Record<string, unknown> } {
        const { caller, request } = context;
        const operations = this.ops(caller);
        const key = createHash('sha256').update(canonical([caller.lifecycleId, request.worker_id,
            request.run_id, request.request_id])).digest('hex');
        const digest = createHash('sha256').update(canonical([context.route, request.arguments])).digest('hex');
        const now = Date.now();
        operations.receipts = operations.receipts.filter(entry => now - entry.at < this.options.config.subagents.receiptTtlMs);
        const previous = operations.receipts.find(entry => entry.key === key);
        if (previous && previous.digest !== digest) throw new ToolFailure('request_conflict', 'request ID was reused with different arguments or route');
        return { operations, key, digest, ...(previous ? { reused: { ...previous.result, replayed: true } } : {}) };
    }

    private commitReceipt(context: ToolContext, result: Record<string, unknown>,
        receipt: ReturnType<SubagentService['receipt']>): void {
        const previous = [...receipt.operations.receipts];
        receipt.operations.receipts.push({ key: receipt.key, digest: receipt.digest,
            route: context.route, at: Date.now(), result });
        while (receipt.operations.receipts.length > this.options.config.subagents.maxReceipts) receipt.operations.receipts.shift();
        try { this.saveOps(context.caller, receipt.operations); }
        catch (error) { receipt.operations.receipts = previous; throw error; }
    }

    /** Routes are fixed in the dispatcher, and arguments never select executable code. */
    dispatch(context: ToolContext): Record<string, unknown> | Promise<Record<string, unknown>> {
        context.validate();
        if (Buffer.byteLength(canonical(context.request.arguments)) > 64 * 1024) {
            throw new ToolFailure('invalid_arguments', 'subagent arguments exceed 64 KiB');
        }
        switch (context.route) {
            case 'subagent/clean-fork': return this.fork(context);
            case 'subagent/receive': return this.receive(context);
            case 'subagent/send': return this.send(context);
            default: throw new ToolFailure('not_implemented', 'remote tool route is not implemented');
        }
    }

    private fork(context: ToolContext): Record<string, unknown> {
        exact(context.request.arguments, []);
        const receipt = this.receipt(context);
        if (receipt.reused) return receipt.reused;
        const parent = context.caller;
        if (this.recoveryBlocked) throw new ToolFailure('recovery_required', 'unverified child metadata must be inspected before creating more workers');
        if (this.closing || parent.closing || !this.options.supervisor.isRunning(parent)) {
            throw new ToolFailure('lifecycle_closed', 'parent lifecycle is not running');
        }
        const live = [...this.children.values()].filter(record => !record.removed);
        let depth = 1;
        let ancestor = this.children.get(parent.id);
        const seen = new Set<string>();
        while (ancestor) {
            if (seen.has(ancestor.session.id)) throw new ToolFailure('invalid_parent', 'parent graph is cyclic');
            seen.add(ancestor.session.id);
            depth += 1;
            ancestor = this.children.get(ancestor.parent.session_id);
        }
        if (live.length >= this.options.config.subagents.maxLive
            || live.filter(record => record.parent.session_id === parent.id).length >= this.options.config.subagents.maxChildren
            || depth > this.options.config.subagents.maxDepth) {
            throw new ToolFailure('limit_exceeded', 'subagent capacity or depth limit reached');
        }
        const id = `subagent-${randomUUID()}`;
        const session = this.options.registry.create(id, {});
        session.kind = 'headless';
        session.subagent = { parent: parent.id, lifecycle: 'preparing', policy: 'ask', health: 'unknown',
            reason: 'waiting for worker startup', observed_at: null, active: false };
        const record: ChildRecord = {
            session, parent: { session_id: parent.id, lifecycle_id: parent.lifecycleId,
                worker_id: context.request.worker_id }, conversation: null,
            removed: false, startup: null, startupTimer: null, terminalTimer: null, error: '', uncertainStart: false,
            terminationConfirmed: false, cleanupAttempts: 0, retryAt: 0,
        };
        this.children.set(id, record); // quota reservation precedes any asynchronous startup
        let committed = false;
        try {
            cleanFork(this.options.config, parent, session);
            context.validate();
            this.publish(record);
            const result = { subagent_id: id, lifecycle: 'preparing' };
            this.commitReceipt(context, result, receipt);
            committed = true;
            record.conversation = new ConversationProjection(
                session, ownedPath(this.options.config.dataDir, id, 'conversation.json'),
                this.options.config.subagents.conversationBytes,
                () => this.projectionStorageFailed(session),
            );
            // Startup belongs to the supervisor once the durable receipt exists.
            record.startup = Promise.resolve().then(() => this.start(record)).finally(() => { record.startup = null; });
            void record.startup.catch(() => this.fail(record, 'worker startup failed'));
            return result;
        } catch (error) {
            if (!committed) {
                this.children.delete(id);
                this.options.registry.remove(id);
                try { removeChildDirectory(this.options.config.dataDir, id); } catch { /* safe orphan recovery */ }
            } else this.fail(record, 'committed startup requires cleanup');
            throw error;
        }
    }

    /** A recovered starting process also has a finite identity/status deadline. */
    private armStartupDeadline(record: ChildRecord): void {
        if (record.startupTimer) clearTimeout(record.startupTimer);
        record.startupTimer = setTimeout(() => {
            if (record.session.subagent?.lifecycle === 'starting') this.fail(record, 'worker startup timed out');
        }, this.options.config.subagents.startupTimeoutMs);
        record.startupTimer.unref();
    }

    private async start(record: ChildRecord): Promise<void> {
        if (record.session.closing || record.removed) return;
        record.session.subagent!.lifecycle = 'starting';
        this.publish(record);
        this.armStartupDeadline(record);
        const result = await this.options.supervisor.start(record.session);
        if (!result.ok) this.fail(record, 'worker could not start');
        else this.publish(record);
    }

    private child(context: ToolContext, id: unknown): ChildRecord {
        if (typeof id !== 'string') throw new ToolFailure('invalid_arguments', 'subagent_id is required');
        const record = this.children.get(id);
        if (!record || record.parent.session_id !== context.caller.id
            || record.parent.lifecycle_id !== context.caller.lifecycleId
            || record.parent.worker_id !== context.request.worker_id) {
            throw new ToolFailure('unauthorized', 'target is not a direct child of this worker lifecycle');
        }
        return record;
    }

    private status(record: ChildRecord): Record<string, unknown> {
        const session = record.session;
        return { subagent_id: session.id, ...session.subagent,
            process_state: session.process?.state ?? 'not-started', connected: session.connected,
            run_id: session.activeRunId || null, pending_approvals: session.prompts.size,
            error: record.error || null };
    }

    private receive(context: ToolContext): Record<string, unknown> | Promise<Record<string, unknown>> {
        const args = context.request.arguments;
        exact(args, ['subagent_id', 'cursor', 'limit', 'answer']);
        if (args.subagent_id === undefined) {
            if (args.cursor !== undefined || args.limit !== undefined || args.answer !== undefined) throw new ToolFailure('invalid_arguments', 'pagination requires a target');
            return { subagents: [...this.children.values()].filter(record =>
                record.parent.session_id === context.caller.id && record.parent.lifecycle_id === context.caller.lifecycleId
                && record.parent.worker_id === context.request.worker_id).map(record => this.status(record)) };
        }
        const record = this.child(context, args.subagent_id);
        if (args.answer !== undefined) {
            if (args.cursor !== undefined || args.limit !== undefined) throw new ToolFailure('invalid_arguments', 'answer cannot carry turn pagination');
            if (!answerQuery(args.answer)) throw new ToolFailure('invalid_arguments',
                'answer requires a complete source including fingerprint, part and offset');
            return readAnswer(record.session, args.answer, context.signal).then(answer => {
                context.validate();
                this.child(context, args.subagent_id);
                return { ...this.status(record), requests: [], requests_truncated: false,
                    conversation: null, answer };
            }).catch(error => { throw error instanceof ToolFailure ? error
                : new ToolFailure('answer_unavailable', 'answer source expired, disconnected or cursor invalid'); });
        }
        const cursor = args.cursor ?? 0;
        const limit = args.limit ?? 5;
        if (!Number.isSafeInteger(cursor) || (cursor as number) < 0 || !Number.isSafeInteger(limit)
            || (limit as number) < 1 || (limit as number) > 10) throw new ToolFailure('invalid_arguments', 'cursor must be nonnegative and limit 1-10');
        const conversation = record.conversation?.value;
        const turns = conversation?.turns ?? [];
        const page = turns.slice(cursor as number, (cursor as number) + (limit as number));
        const operations = (this.operations.get(record.session.id) ?? this.ops(record.session)).requests.slice(-20);
        const result = { ...this.status(record), requests: operations, requests_truncated: false,
            conversation: conversation ? { ...conversation, turns: page, cursor,
                next: (cursor as number) + page.length, total: turns.length } : null };
        if (Buffer.byteLength(JSON.stringify(result)) > MAX_RPC_BYTES) {
            // Keep whole turns; a single oversized turn may be represented as omitted.
            while (page.length > 1 && Buffer.byteLength(JSON.stringify(result)) > MAX_RPC_BYTES - 4096) page.pop();
            if (page.length && Buffer.byteLength(JSON.stringify(result)) > MAX_RPC_BYTES - 4096) {
                // Keep the source of a large answer actionable instead of skipping its turn.
                const turn = structuredClone(page[0]!);
                turn.user = [];
                turn.steps = turn.steps.slice(-1).map(step => ({ ...step,
                    content: step.content.map(part => ({ ...part, raw: '', truncated: true,
                        bytes: part.bytes ?? Buffer.byteLength(part.raw) })) }));
                page[0] = turn;
            }
            if (result.conversation) {
                result.conversation.next = (cursor as number) + page.length;
                result.conversation.truncated = true;
                if (!page.length && (cursor as number) < turns.length) result.conversation.next += 1;
            }
        }
        while (operations.length && Buffer.byteLength(JSON.stringify(result)) > MAX_RPC_BYTES - 2048) {
            operations.shift();
            result.requests_truncated = true;
        }
        return result;
    }

    private send(context: ToolContext): Record<string, unknown> {
        const args = context.request.arguments;
        const operation = args.operation;
        if (!['message', 'continue', 'compact', 'stop'].includes(String(operation))) throw new ToolFailure('invalid_arguments', 'operation must be message, continue, compact or stop');
        exact(args, operation === 'message' ? ['subagent_id', 'operation', 'content', 'options']
            : operation === 'stop' ? ['subagent_id', 'operation'] : ['subagent_id', 'operation', 'options']);
        const record = this.child(context, args.subagent_id);
        const receipt = this.receipt(context);
        if (receipt.reused) return receipt.reused;
        const session = record.session;
        if (operation === 'stop') {
            const result = { subagent_id: session.id, operation_id: newRequestId(), operation: 'stop', state: 'stopping' };
            this.commitReceipt(context, result, receipt);
            void this.options.supervisor.stop(session).catch(() => this.fail(record, 'stop requires cleanup'));
            return result;
        }
        if (this.closing || session.closing || record.removed) throw new ToolFailure('lifecycle_closed', 'child lifecycle is stopping or stopped');
        if (!session.connected || session.identity.state !== 'live') throw new ToolFailure('disconnected', 'child worker is not connected');
        if (operation === 'compact' && (session.workerCapabilities?.workerId !== session.identity.workerId
            || !session.workerCapabilities.names.includes('context-compact'))) throw new ToolFailure('unsupported_operation', 'child has not advertised context-compact');
        const options = object(args.options);
        if (args.options !== undefined && !options) throw new ToolFailure('invalid_arguments', 'options must be an object');
        if (options && Object.hasOwn(options, 'confirmation')) throw new ToolFailure('policy_forbidden', 'only the operator may set headless confirmation policy');
        const requestId = newRequestId();
        let payload;
        try {
            payload = buildPayload({ operation: operation as 'message' | 'continue' | 'compact', requestId,
                content: args.content, options: { ...options, confirmation: { mode: 'ask' } } });
        } catch { throw new ToolFailure('invalid_arguments', 'invalid child content or options'); }
        context.validate();
        const operations = this.ops(session);
        const entry: Operation = { request_id: requestId, operation: operation as string, state: 'intent',
            run_id: '', at: new Date().toISOString() };
        operations.requests.push(entry);
        while (operations.requests.length > 200) operations.requests.shift();
        this.saveOps(session, operations);
        const result = { subagent_id: session.id, request_id: requestId, operation, state: 'unknown' };
        // Persist the retry receipt before the irreversible socket write. A crash
        // in between remains unknown, and never causes automatic retransmission.
        this.commitReceipt(context, result, receipt);
        session.trackRequest(requestId, operation as string);
        if (operation === 'message') record.conversation?.trackInput(requestId, payload.data.content);
        let sent;
        try { sent = session.connection!.sendPayload(payload); }
        catch {
            entry.state = 'unknown';
            entry.detail = 'transport failed after dispatch intent; delivery is uncertain';
            try { this.saveOps(session, operations); } catch { /* durable intent remains uncertain */ }
            throw new ToolFailure('delivery_unknown', 'payload delivery is uncertain; inspect receive instead of resending');
        }
        entry.state = sent.ok ? 'sent' : 'rejected';
        if (!sent.ok) { entry.detail = 'payload was not enqueued'; session.noteRequestRejected(requestId, entry.detail); }
        try { this.saveOps(session, operations); } catch { /* receipt already records unknown delivery */ }
        return { ...result, state: entry.state };
    }

    setPolicy(session: Session, policy: unknown): void {
        if (!['ask', 'deny', 'approve'].includes(String(policy))) throw new ToolFailure('invalid_arguments', 'policy must be ask, deny or approve');
        const record = this.children.get(session.id);
        if (!record || record.removed || session.closing) throw new ToolFailure('lifecycle_closed', 'subagent is no longer actionable');
        const previous = session.subagent!.policy;
        session.subagent!.policy = policy as 'ask' | 'deny' | 'approve';
        try { this.publish(record); } catch (error) { session.subagent!.policy = previous; throw error; }
    }

    onEvent(envelope: ForwardedEnvelope, connection: WorkerConnection): void {
        const record = this.children.get(connection.session.id);
        for (const descendant of this.children.values()) {
            if (!descendant.removed && descendant.parent.session_id === connection.session.id
                && descendant.parent.worker_id !== envelope.worker_id) {
                this.fail(descendant, 'parent worker incarnation changed');
            }
        }
        if (!record || record.removed || connection.session.closing) return;
        const session = record.session;
        const state = session.subagent!;
        const previousLifecycle = state.lifecycle;
        state.observed_at = envelope.received_at ?? null;
        state.active = !!session.activeRunId;
        const dataStatus = object(envelope.data);
        state.health = record.conversation?.storageFailed || this.metadataStorageFailures.has(session.id)
            || envelope.issues?.length || (envelope.event === 'status' && dataStatus?.storage_failed === true)
            ? 'degraded' : 'healthy';
        state.reason = record.conversation?.storageFailed ? 'primary conversation storage is unavailable'
            : this.metadataStorageFailures.has(session.id) ? 'subagent metadata storage is unavailable'
            : state.health === 'healthy' ? 'live identified worker event channel'
            : 'worker reported storage or protocol diagnostics';
        if (state.lifecycle === 'starting' && ['ready', 'status'].includes(envelope.event)) {
            state.lifecycle = 'ready';
            if (record.startupTimer) clearTimeout(record.startupTimer);
            record.startupTimer = null;
        }
        const operations = this.ops(session);
        const data = object(envelope.data);
        const id = envelope.event === 'input_rejected' ? data?.request_id : envelope.request_id;
        const entry = operations.requests.find(request => request.request_id === id);
        if (entry) {
            let changed = false;
            if (envelope.event === 'input_admitted' && ['intent', 'sent', 'unknown'].includes(entry.state)) {
                entry.state = 'admitted';
                entry.run_id = envelope.run_id;
                session.noteRequestAdmitted(entry.request_id);
                changed = true;
            }
            if (envelope.event === 'input_rejected' && ['intent', 'sent', 'unknown'].includes(entry.state)) {
                entry.state = 'rejected';
                entry.detail = typeof data?.message === 'string' ? data.message.slice(0, 512) : 'worker rejected input';
                session.noteRequestRejected(entry.request_id, entry.detail);
                changed = true;
            }
            if (envelope.event === 'run_finished' && ['admitted', 'unknown'].includes(entry.state)
                && (!entry.run_id || entry.run_id === envelope.run_id)) {
                entry.state = 'finished';
                entry.status = typeof data?.status === 'string' ? data.status.slice(0, 32) : 'unknown';
                changed = true;
            }
            // Worker ingestion has already validated the summary's UTF-8 byte
            // budget. Keep the complete normalized text in the operation receipt.
            if (entry.operation === 'compact' && envelope.event === 'compact_finished' && data?.durable === true
                && typeof data.summary === 'string' && entry.summary !== data.summary) {
                entry.summary = data.summary;
                changed = true;
            }
            // Receipts/outcomes remain synchronous, but unrelated events must
            // not fsync the unchanged operation ledger on every model step.
            if (changed) this.saveOps(session, operations);
        }
        record.conversation?.event(envelope, connection);
        if (state.lifecycle !== previousLifecycle) this.publish(record);
        else this.schedulePublication(record);
    }

    onConnection(session: Session, connection: WorkerConnection | null): void {
        const record = this.children.get(session.id);
        if (!record || record.removed || session.closing) return;
        session.subagent!.health = connection ? 'unknown' : 'degraded';
        session.subagent!.reason = connection ? 'awaiting identified status' : 'worker event channel disconnected';
        record.conversation?.connectionChanged(connection);
        if (!connection) {
            for (const prompt of [...session.prompts.values()]) prompt.retire('disconnected', 'subagent event channel disconnected');
            for (const request of this.ops(session).requests) {
                if (request.state === 'sent' || request.state === 'intent' || request.state === 'admitted') request.state = 'unknown';
            }
            this.saveOps(session);
        }
        this.publish(record);
    }

    onProcess(session: Session, record: ProcessRecord): void {
        const child = this.children.get(session.id);
        if (!this.recovering && ['exited', 'failed'].includes(record.state)) {
            // Initiate cleanup even if saving the process observation fails.
            void this.options.supervisor.stop(session).catch(() => {
                this.options.log.warn(`family cleanup failed for ${session.id}`);
            });
        }
        if (child && !child.removed) {
            try { this.publish(child); }
            catch { this.fail(child, 'process metadata publication failed'); }
        }
    }

    private fail(record: ChildRecord, reason: string): void {
        if (record.removed) return;
        record.error = reason;
        record.session.subagent!.health = 'degraded';
        void this.options.supervisor.stop(record.session).catch(() => {});
    }

    /** Recovery diagnostics contain no launch arguments, tokens or private environment. */
    recoveryStatus(): { blocked: boolean; reason: string | null } {
        return { blocked: this.recoveryBlocked, reason: this.recoveryBlocked
            ? 'unverified child ownership retained; inspect managed storage and restart the Hub' : null };
    }

    /** Inspect one restored child without exposing its private startup snapshot. */
    recoveryState(session: Session): Record<string, unknown> {
        const record = this.children.get(session.id);
        if (!record) throw new Error('unknown headless lifecycle');
        const automaticRetry = !record.removed && !record.uncertainStart
            && session.subagent?.lifecycle === 'cleanup-pending'
            && (record.cleanupAttempts ?? 0) < MAX_CLEANUP_ATTEMPTS;
        return {
            session_id: session.id,
            lifecycle_id: session.lifecycleId,
            lifecycle: session.subagent?.lifecycle,
            uncertain_start: record.uncertainStart,
            termination_confirmed: record.terminationConfirmed === true,
            cleanup_attempts: record.cleanupAttempts ?? 0,
            max_cleanup_attempts: MAX_CLEANUP_ATTEMPTS,
            automatic_retry: automaticRetry,
            retry_at: automaticRetry && record.retryAt ? new Date(record.retryAt).toISOString() : null,
            reason: session.subagent?.reason,
        };
    }

    /** Operator-only recovery. Worker remote routes cannot attest process termination. */
    async recover(session: Session, action: unknown, lifecycleId: unknown): Promise<StopResult> {
        const record = this.children.get(session.id);
        if (!record) throw new Error('unknown headless lifecycle');
        const valid = () => this.children.get(session.id) === record && !record.removed
            && session.subagent?.lifecycle === 'cleanup-pending'
            && lifecycleId === session.lifecycleId && !this.stops.has(session.id) && !this.closing;
        if (!valid() || (action !== 'retry' && action !== 'confirm-terminated')) {
            throw new Error('recovery requires a current cleanup-pending lifecycle and retry or confirm-terminated action');
        }
        if (action === 'confirm-terminated') {
            const process = session.process as ProcessRecord | null;
            const liveProcess = process && processIdentity(process.pid, process.pidStartTime) === 'same';
            const liveContainer = process && dockerName(process)
                && await dockerRunning(process.dockerManagement) === true;
            if (!valid() || liveProcess || liveContainer || (session.connection as WorkerConnection | null)?.isOpen) {
                throw new Error('a live worker or concurrent lifecycle prevents termination confirmation');
            }
        }
        const previous = { uncertain: record.uncertainStart, confirmed: record.terminationConfirmed,
            attempts: record.cleanupAttempts, retryAt: record.retryAt };
        if (action === 'confirm-terminated') {
            record.uncertainStart = false;
            record.terminationConfirmed = true;
        }
        record.cleanupAttempts = 0;
        record.retryAt = 0;
        try { this.publish(record); }
        catch (error) {
            record.uncertainStart = previous.uncertain;
            record.terminationConfirmed = previous.confirmed;
            record.cleanupAttempts = previous.attempts;
            record.retryAt = previous.retryAt;
            throw error;
        }
        this.options.log.warn(`subagent ${session.id}: operator recovery ${action}`);
        return this.options.supervisor.stop(session);
    }

    /** Retry only verifiable ownership, with a durable budget and exponential delay. */
    retryCleanup(now = Date.now()): void {
        if (this.closing || this.recovering) return;
        const automatic: AutomaticCleanup = { now, attempted: new Set() };
        for (const record of this.children.values()) {
            if (!record.removed && !record.uncertainStart
                && record.session.subagent?.lifecycle === 'cleanup-pending'
                && (record.cleanupAttempts ?? 0) < MAX_CLEANUP_ATTEMPTS
                && (record.retryAt ?? 0) <= now && !this.stops.has(record.session.id)) {
                void this.cascade(record.session,
                    () => this.options.supervisor.stopProcess(record.session), automatic).catch(() => {});
            }
        }
    }

    /** Resume durable stop intent without granting descendants a new cleanup budget. */
    stopAutomatically(session: Session): Promise<StopResult> {
        return this.cascade(session, () => this.options.supervisor.stopProcess(session), {
            now: Date.now(), attempted: new Set(),
        });
    }

    /** Freeze eligible ownership synchronously, then attempt each eligible cleanup. */
    private cascade(session: Session, stopSelf: () => Promise<StopResult>, automatic?: AutomaticCleanup): Promise<StopResult> {
        const pending = this.stops.get(session.id);
        if (pending) return pending;
        if (this.children.get(session.id)?.removed) {
            return Promise.resolve({ ok: true, how: 'already-stopped', forced: false });
        }
        const child = this.children.get(session.id);
        if (automatic && child) {
            const deferred = this.deferAutomaticCleanup(child, automatic);
            if (deferred) return Promise.resolve({ ok: false, how: deferred, forced: false });
        }
        automatic?.attempted.add(session.id);
        let resolveStop!: (value: StopResult) => void;
        let rejectStop!: (error: unknown) => void;
        const promise = new Promise<StopResult>((resolve, reject) => { resolveStop = resolve; rejectStop = reject; });
        // Publish the coordination slot before synchronous process observers.
        this.stops.set(session.id, promise);
        const descendants = [...this.children.values()].filter(record =>
            record.parent.session_id === session.id && !record.removed);
        session.closing = true;
        const process = session.process as ProcessRecord | null;
        if (process && this.options.supervisor.isRunning(session) && process.state !== 'stopping') {
            process.stopRequested = true;
            process.state = 'stopping';
            // Ordinary hub.json must preserve stop intent while descendants
            // are being stopped, before the parent's shutdown signal is sent.
            this.options.supervisor.notify(session, process);
        }
        for (const record of descendants) this.freeze(record, new Set(), automatic);
        if (child) {
            child.cleanupAttempts = (child.cleanupAttempts ?? 0) + 1;
            this.freezeSession(child);
        }
        const task = Promise.resolve().then(async () => {
            let ok = true;
            // Resource bounds cap concurrency, and siblings never short-circuit.
            for (let index = 0; index < descendants.length; index += 4) {
                const outcomes = await Promise.allSettled(descendants.slice(index, index + 4)
                    .map(record => automatic
                        ? this.cascade(record.session, () => this.options.supervisor.stopProcess(record.session), automatic)
                        : this.options.supervisor.stop(record.session)));
                if (outcomes.some(outcome => outcome.status === 'rejected' || !outcome.value.ok)) ok = false;
            }
            if (child?.startup) await child.startup.catch(() => {});
            let result: StopResult;
            try {
                if (child?.terminationConfirmed) {
                    if (session.process) this.options.supervisor.finish(session.process as ProcessRecord, { exitCode: null });
                    result = { ok: true, how: 'termination-confirmed', forced: false };
                } else result = await stopSelf();
            }
            catch { result = { ok: false, how: 'stop-failed', forced: false }; }
            ok = result.ok && ok;
            if (child) {
                try {
                    // A verified process/container stop resolves a stale uncertain
                    // flag. A missing record's "not-started" result never does.
                    if (result.ok && session.process && result.how !== 'not-started') {
                        child.uncertainStart = false;
                        child.terminationConfirmed = true;
                    }
                    if (!result.ok || child.uncertainStart) throw new Error('worker termination is not confirmed');
                    const io = (session.process as ProcessRecord | null)?.ownedIo ?? Promise.resolve();
                    let timer: NodeJS.Timeout | undefined;
                    try {
                        const joined = await Promise.race([
                            io.then(() => true),
                            new Promise<boolean>(resolve => { timer = setTimeout(() => resolve(false), 5000); }),
                        ]);
                        if (!joined) throw new Error('owned output tasks did not finish');
                    } finally { if (timer) clearTimeout(timer); }
                    // Require descendants to be gone before forgetting the only
                    // durable parent reference used for recovery.
                    if (!ok) throw new Error('descendant cleanup is incomplete');
                    for (const prompt of [...session.prompts.values()]) prompt.retire('shutdown', 'subagent stopped');
                    (session.connection as WorkerConnection | null)?.terminate('subagent stopped');
                    removeChildDirectory(this.options.config.dataDir, session.id);
                    child.removed = true;
                    this.metadataFlushes.delete(session.id);
                    this.metadataStorageFailures.delete(session.id);
                    session.subagent!.lifecycle = 'stopped';
                    session.subagent!.health = 'unknown';
                    session.subagent!.reason = 'worker terminated; persistence removed';
                    session.subagent!.active = false;
                    const terminal = this.operations.get(session.id);
                    if (terminal) {
                        terminal.receipts = [];
                        terminal.requests = terminal.requests.slice(-20).map(entry => {
                            const { summary: _summary, ...outcome } = entry;
                            return outcome;
                        });
                    }
                    // Release log content and invocation/child references too;
                    // terminal retention is status, not a hidden event archive.
                    const process = session.process as ProcessRecord | null;
                    process?.logs.clear();
                    if (process) {
                        process.args.length = 0;
                        process.outputSplitters.length = 0;
                        process.child = null;
                        process.error = null;
                        process.dockerManagement = null;
                    }
                    session.spec = {};
                    session.requests.clear();
                    // Only small in-memory terminal outcomes remain, for one TTL.
                    child.conversation = null;
                    const terminals = [...this.children.values()].filter(entry => entry.removed);
                    if (terminals.length > 128) {
                        const oldest = terminals[0]!;
                        if (oldest.terminalTimer) clearTimeout(oldest.terminalTimer);
                        this.children.delete(oldest.session.id);
                        this.operations.delete(oldest.session.id);
                        this.options.registry.remove(oldest.session.id);
                        this.options.removed(oldest.session.id);
                    }
                    child.terminalTimer = setTimeout(() => {
                        this.children.delete(session.id);
                        this.operations.delete(session.id);
                        this.options.registry.remove(session.id);
                        this.options.removed(session.id);
                    }, this.options.config.subagents.receiptTtlMs);
                    child.terminalTimer.unref();
                } catch {
                    ok = false;
                    session.subagent!.lifecycle = 'cleanup-pending';
                    session.subagent!.health = 'degraded';
                    const retry = !child.uncertainStart && child.cleanupAttempts < MAX_CLEANUP_ATTEMPTS;
                    child.retryAt = retry ? Date.now() + 5000 * 2 ** child.cleanupAttempts : 0;
                    session.subagent!.reason = retry
                        ? `cleanup incomplete; automatic retry ${child.cleanupAttempts}/${MAX_CLEANUP_ATTEMPTS} scheduled`
                        : 'cleanup paused; inspect ownership, then use POST /api/sessions/:id/recover';
                    child.error ||= 'lifetime cleanup incomplete';
                    if (!retry) this.options.log.warn(`subagent ${session.id}: ${session.subagent!.reason}`);
                }
                this.publish(child);
            }
            return { ...result, ok };
        });
        void task.then(resolveStop, rejectStop);
        void promise.finally(() => { if (this.stops.get(session.id) === promise) this.stops.delete(session.id); }).catch(() => {});
        return promise;
    }

    /** Ancestor retries must not reset a paused descendant's budget or rewrite it. */
    private cleanupPaused(record: ChildRecord): boolean {
        return (record.cleanupAttempts ?? 0) >= MAX_CLEANUP_ATTEMPTS
            || (record.uncertainStart && (record.cleanupAttempts ?? 0) > 0);
    }

    /** Used before both freezing and starting: neither may bypass persisted backoff. */
    private deferAutomaticCleanup(record: ChildRecord, automatic: AutomaticCleanup): string | null {
        if (this.cleanupPaused(record)) return 'operator-recovery-required';
        if ((record.retryAt ?? 0) > automatic.now) return 'cleanup-backoff';
        if (automatic.attempted.has(record.session.id)) return 'cleanup-already-attempted';
        return null;
    }

    private freeze(record: ChildRecord, seen = new Set<string>(), automatic?: AutomaticCleanup): void {
        if (seen.has(record.session.id)) return;
        if (automatic && this.deferAutomaticCleanup(record, automatic)) return;
        seen.add(record.session.id);
        this.freezeSession(record);
        for (const child of this.children.values()) {
            if (!child.removed && child.parent.session_id === record.session.id) this.freeze(child, seen, automatic);
        }
    }

    /** Close one lifecycle before awaiting transport; recursion is owned by freeze. */
    private freezeSession(record: ChildRecord): void {
        record.session.closing = true;
        record.session.subagent!.lifecycle = 'stopping';
        for (const prompt of [...record.session.prompts.values()]) prompt.retire('shutdown', 'subagent lifecycle is stopping');
        record.conversation?.stop();
        // A stopping-intent publication below supersedes the queued metadata.
        // Cancel before closing the scheduler so it cannot recreate storage later.
        const metadata = this.metadataFlushes.get(record.session.id);
        metadata?.cancel();
        metadata?.stop();
        this.metadataFlushes.delete(record.session.id);
        if (record.startupTimer) clearTimeout(record.startupTimer);
        record.startupTimer = null;
        try { this.publish(record); } // best effort: a disk error must not skip termination
        catch { this.options.log.warn(`subagent ${record.session.id}: could not persist stopping intent`); }
    }

    /** Restore in two passes: registry/processes first, graph validation second. */
    async restore(): Promise<void> {
        if (this.retryTimer) clearInterval(this.retryTimer);
        this.retryTimer = null;
        this.recovering = true;
        try {
            let directories: string[];
            try { directories = childDirectories(this.options.config.dataDir); }
            catch {
                this.recoveryBlocked = true;
                this.options.log.warn('subagent recovery blocked: cannot safely enumerate managed storage; ownership retained for operator inspection');
                return;
            }
            for (const id of directories) {
                try {
                    const raw = object(readPrivate(ownedPath(this.options.config.dataDir, id, 'metadata.json'), MAX_OPERATION_BYTES));
                    if (!raw) {
                        // Metadata is published before spawn, so an incomplete
                        // reservation without metadata cannot contain a worker.
                        removeChildDirectory(this.options.config.dataDir, id);
                        continue;
                    }
                    const parent = object(raw.cascading_parent);
                    const detail = object(raw.subagent);
                    if (raw.id !== id || raw.kind !== 'headless' || typeof raw.token !== 'string'
                        || !parent || !isValidSessionId(parent.session_id) || typeof parent.lifecycle_id !== 'string'
                        || typeof parent.worker_id !== 'string' || !detail
                        || !['ask', 'deny', 'approve'].includes(String(detail.policy))) throw new Error('invalid child metadata');
                    const session = this.options.registry.create(id, {});
                    session.kind = 'headless';
                    session.token = raw.token;
                    session.createdAt = typeof raw.created_at === 'string' ? raw.created_at : session.createdAt;
                    session.lifecycleId = typeof raw.lifecycle_id === 'string' ? raw.lifecycle_id : '';
                    session.subagent = { parent: parent.session_id as string, policy: detail.policy as 'ask' | 'deny' | 'approve',
                        lifecycle: detail.lifecycle === 'stopping' || detail.lifecycle === 'cleanup-pending'
                            ? 'cleanup-pending' : detail.lifecycle === 'ready' ? 'ready' : 'starting',
                        health: 'unknown', reason: 'restoring supervised family', observed_at: null, active: false };
                    session.closing = session.subagent.lifecycle === 'cleanup-pending';
                    const record: ChildRecord = { session, parent: parent as unknown as ParentRef,
                        conversation: new ConversationProjection(
                            session, ownedPath(this.options.config.dataDir, id, 'conversation.json'),
                            this.options.config.subagents.conversationBytes,
                            () => this.projectionStorageFailed(session),
                        ),
                        removed: false, startup: null, startupTimer: null, terminalTimer: null,
                        uncertainStart: raw.uncertain_start === true || (!raw.process && object(raw.subagent)?.lifecycle !== 'preparing'),
                        terminationConfirmed: raw.termination_confirmed === true,
                        cleanupAttempts: typeof raw.cleanup_attempts === 'number' && Number.isInteger(raw.cleanup_attempts)
                            ? Math.min(MAX_CLEANUP_ATTEMPTS, Math.max(0, raw.cleanup_attempts)) : 0,
                        retryAt: typeof raw.retry_at === 'number' && Number.isSafeInteger(raw.retry_at)
                            && raw.retry_at >= 0 && raw.retry_at <= 8640000000000000 ? raw.retry_at : 0,
                        error: typeof raw.error === 'string' ? raw.error.slice(0, 512) : '' };
                    this.children.set(id, record);
                    this.ops(session);
                    if (record.terminationConfirmed) record.uncertainStart = false;
                    else if (raw.process) {
                        const stored = object(raw.process);
                        const adopted = this.options.supervisor.adopt(session, raw.process);
                        if (!adopted) {
                            // A recorded Linux incarnation that disappeared/reused
                            // is evidence; unreadable or malformed identity is not.
                            record.terminationConfirmed = !!stored && stored.pid_file == null
                                && processIdentity(stored.pid, stored.pid_start_time) === 'gone';
                            record.uncertainStart = !record.terminationConfirmed;
                        }
                    }
                } catch {
                    this.recoveryBlocked = true;
                    this.options.log.warn(`subagent ${id}: invalid recovery state retained for operator inspection`);
                }
            }
            const recovery = [...this.children.values()];
            for (const record of recovery) {
                const parent = this.options.registry.get(record.parent.session_id);
                const seen = new Set([record.session.id]);
                let ancestor = parent;
                let cyclic = false;
                while (ancestor?.kind === 'headless') {
                    if (seen.has(ancestor.id)) { cyclic = true; break; }
                    seen.add(ancestor.id);
                    const next = this.children.get(ancestor.id)?.parent.session_id;
                    ancestor = next ? this.options.registry.get(next) : undefined;
                }
                if (cyclic) {
                    // Break only invalid graph links so stop traversal is finite.
                    record.parent.session_id = '__invalid_parent__';
                }
                if (cyclic || !parent || parent.lifecycleId !== record.parent.lifecycle_id
                    || !this.options.supervisor.isRunning(parent) || !this.options.supervisor.isRunning(record.session)
                    || (record.session.process && dockerName(record.session.process as ProcessRecord)
                        && !(record.session.process as ProcessRecord).dockerManagement)
                    || record.session.subagent!.lifecycle === 'cleanup-pending') {
                    record.session.subagent!.lifecycle = 'cleanup-pending';
                    record.session.closing = true;
                } else {
                    this.publish(record);
                    if (record.session.subagent!.lifecycle === 'starting') this.armStartupDeadline(record);
                }
            }
            const automatic: AutomaticCleanup = { now: Date.now(), attempted: new Set() };
            for (const record of recovery) {
                if (record.session.subagent?.lifecycle !== 'cleanup-pending') continue;
                const deferred = this.deferAutomaticCleanup(record, automatic);
                if (deferred) {
                    record.session.subagent.health = 'degraded';
                    record.session.subagent.reason = deferred === 'operator-recovery-required'
                        ? 'cleanup paused; inspect ownership, then use POST /api/sessions/:id/recover'
                        : `cleanup incomplete; automatic retry ${record.cleanupAttempts}/${MAX_CLEANUP_ATTEMPTS} scheduled`;
                    this.options.changed(record.session);
                } else {
                    await this.cascade(record.session,
                        () => this.options.supervisor.stopProcess(record.session), automatic).catch(() => {});
                }
            }
        } finally { this.recovering = false; }
        this.retryTimer = setInterval(() => this.retryCleanup(), 5000);
        this.retryTimer.unref();
    }

    /** Freeze before shutdown; timers cannot recreate a deleted directory. */
    async shutdown(): Promise<void> {
        this.closing = true;
        if (this.retryTimer) clearInterval(this.retryTimer);
        this.retryTimer = null;
        for (const session of this.options.registry.list()) session.closing = true;
        const results = await Promise.allSettled(this.options.registry.list()
            .filter(session => session.kind !== 'headless').map(session => this.options.supervisor.stop(session)));
        // Orphans/cleanup failures still receive their own best-effort attempt.
        for (const record of this.children.values()) {
            if (!record.removed) await this.options.supervisor.stop(record.session).catch(() => {});
            record.conversation?.stop();
            if (record.terminalTimer) clearTimeout(record.terminalTimer);
            if (record.startupTimer) clearTimeout(record.startupTimer);
        }
        for (const flush of this.metadataFlushes.values()) flush.stop();
        this.metadataFlushes.clear();
        if (results.some(result => result.status === 'rejected' || !result.value.ok)) {
            this.options.log.warn('some worker family shutdowns remain cleanup-pending');
        }
    }
}
