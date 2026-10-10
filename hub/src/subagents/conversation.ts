import { answerSource, type AnswerSource } from '../../shared/answers.ts';
/** Bounded primary dialogue only. Raw events, reasoning, tools and extras never persist. */
import { newRequestId, buildPayload } from '../protocol/messages.ts';
import type { Session } from '../state/registry.ts';
import type { ForwardedEnvelope, WorkerConnection } from '../worker/connection.ts';
import { readPrivate, writePrivate } from './storage.ts';
import { ProjectionFlush } from './projection-flush.ts';

export interface DialoguePart { type: string; modality: string; raw: string; truncated?: boolean; bytes?: number }
export interface DialogueTurn {
    index: number;
    request_id?: string;
    internal_input?: 'auto_compact_continue';
    source?: { worker_id: string; request_id: string; run_id: string };
    user: DialoguePart[];
    steps: { index: number; content: DialoguePart[]; answer_source?: AnswerSource }[];
}
export interface Conversation {
    revision: number | null;
    worker_id: string | null;
    turns: DialogueTurn[];
    truncated: boolean;
    incomplete: boolean;
    stale: boolean;
    refreshed_at: string | null;
}
interface Refresh {
    connection: WorkerConnection;
    worker: string;
    request: string;
    start: number;
    step: number;
    revision: number | null;
    total: number | null;
    pages: number;
    generation: number;
    turns: DialogueTurn[];
    pending: DialogueTurn | null;
    discovering: boolean;
    truncated: boolean;
    timer: NodeJS.Timeout | null;
}
const object = (value: unknown): Record<string, unknown> | null =>
    value !== null && typeof value === 'object' && !Array.isArray(value)
        ? value as Record<string, unknown> : null;
const MAX_REFRESH_PAGES = 64;
const MAX_TURN_STEPS = 32;
const integer = (value: unknown): value is number => Number.isSafeInteger(value) && (value as number) >= 0;

/** Clip only at UTF-8 boundaries so a byte budget does not corrupt visible text. */
function prefix(raw: string, maxBytes: number): string {
    const bytes = Buffer.from(raw);
    let end = Math.min(bytes.length, maxBytes);
    while (end > 0 && end < bytes.length && (bytes[end]! & 0xc0) === 0x80) end -= 1;
    return bytes.subarray(0, end).toString('utf8');
}

/** Keep visible content, with explicit truncation. Do not pass through extras. */
export function dialogueParts(value: unknown, mark: () => void): DialoguePart[] {
    if (!Array.isArray(value)) return [];
    if (value.length > 128) mark();
    const parts: DialoguePart[] = [];
    for (const raw of value.slice(0, 128)) {
        const part = object(raw);
        if (!part || !['text', 'external_ref'].includes(String(part.type))
            || typeof part.modality !== 'string' || typeof part.raw !== 'string') { mark(); continue; }
        if (part.truncated === true || part.omitted === true) mark();
        const bytes = Buffer.from(part.raw);
        if (bytes.length > 256 * 1024) mark();
        const text = prefix(part.raw, 256 * 1024);
        parts.push({ type: part.type as string, modality: part.modality.slice(0, 32), raw: text,
            ...(text !== part.raw || part.truncated === true ? { truncated: true,
                bytes: typeof part.bytes === 'number' ? part.bytes : bytes.length } : {}) });
    }
    return parts;
}

/**
 * Worker-backed current history with a bounded newest tail. Refreshes validate
 * revision/cursors before publication; failed refreshes preserve known results.
 * Live commits update memory without starting history queries. Reconciliation
 * starts when idle, with three attempts per connection or run settlement.
 * An already running refresh may finish during a run, but cannot publish over
 * subsequently observed commits. It catches up at settlement instead.
 * Storage failure is observable in memory even when its diagnostic cannot be
 * persisted. The optional observer must not be required for safe shutdown.
 */
export class ConversationProjection {
    value: Conversation = {
        revision: null, worker_id: null, turns: [], truncated: false,
        incomplete: true, stale: true, refreshed_at: null,
    };
    private refresh: Refresh | null = null;
    private generation = 0;
    private attempts = 0;
    private active = false;
    private stopped = false;
    private scheduled: NodeJS.Timeout | null = null;
    private latestConnection: WorkerConnection | null = null;
    private pendingUsers = new Map<string, unknown>();
    private lastSequence: number | string | null = null;
    storageFailed = false;
    private readonly onStorageFailure: (error: unknown) => void;
    private readonly storage: ProjectionFlush;
    readonly session: Session;
    readonly path: string;
    readonly maxBytes: number;
    constructor(
        session: Session,
        path: string,
        maxBytes: number,
        onStorageFailure: (error: unknown) => void = () => {},
    ) {
        this.onStorageFailure = onStorageFailure;
        this.session = session;
        this.path = path;
        this.maxBytes = maxBytes;
        this.storage = new ProjectionFlush(() => {
            writePrivate(this.path, this.value, this.maxBytes);
            this.storageFailed = false;
        }, error => {
            this.storageFailed = true;
            this.value.stale = true;
            this.value.incomplete = true;
            this.onStorageFailure(error);
        });
        const saved = readPrivate(path, maxBytes);
        const source = object(saved);
        if (source && Array.isArray(source.turns)) {
            // Never trust disk content to bypass projection filtering.
            let truncated = source.truncated === true || source.turns.length > 100;
            const turns = source.turns.slice(-100).map(raw => this.turn(raw, () => { truncated = true; }))
                .filter((turn): turn is DialogueTurn => !!turn);
            this.value = { ...this.value, turns, truncated,
                worker_id: typeof source.worker_id === 'string' ? source.worker_id : null,
                revision: integer(source.revision) ? source.revision : null,
                refreshed_at: typeof source.refreshed_at === 'string' ? source.refreshed_at : null };
        }
    }

    trackInput(request: string, content: unknown): void {
        if (this.pendingUsers.size >= 200) this.pendingUsers.delete(this.pendingUsers.keys().next().value!);
        this.pendingUsers.set(request, content);
    }

    connectionChanged(connection: WorkerConnection | null): void {
        this.cancelScheduled();
        this.cancelRefresh();
        this.attempts = 0;
        this.active = !!this.session.activeRunId;
        this.lastSequence = null;
        this.latestConnection = connection;
        this.value.stale = true;
        this.value.incomplete = true;
        if (!this.stopped) this.persist();
        if (connection) this.schedule(connection);
    }

    event(envelope: ForwardedEnvelope, connection: WorkerConnection): void {
        if (this.stopped || this.session.closing || this.session.connection !== connection) return;
        // History replies occupy sequence numbers too. Ignoring them here would
        // mistake every successful query for an event gap on the next status.
        const seq = envelope.sequence;
        if (this.lastSequence !== null && seq != null) {
            try {
                if (BigInt(seq) <= BigInt(this.lastSequence)) return;
                if (BigInt(seq) > BigInt(this.lastSequence) + 1n) {
                    this.value.stale = true;
                    this.value.incomplete = true;
                    this.generation += 1;
                    this.schedule(connection);
                }
            } catch { this.value.incomplete = true; }
        }
        if (seq != null) this.lastSequence = seq;
        if (envelope.event === 'history') { this.history(envelope, connection); return; }
        if (envelope.event === 'history_error') {
            if (envelope.worker_id === this.refresh?.worker
                && object(envelope.data)?.request_id === this.refresh?.request) this.failed();
            return;
        }
        // Session.noteEnvelope() has already updated activeRunId. An idle
        // status also catches a settlement whose run_finished event was lost.
        const settled = envelope.event === 'run_finished'
            || (['ready', 'status'].includes(envelope.event)
                && this.active && !this.session.activeRunId);
        this.active = !!this.session.activeRunId;
        if (settled) this.attempts = 0;
        const request = this.session.requests.get(envelope.request_id);
        if (envelope.event === 'input_committed' && request?.operation === 'message') {
            const content = this.pendingUsers.get(envelope.request_id);
            this.pendingUsers.delete(envelope.request_id);
            if (content) {
                this.value.turns.push({ index: (this.value.turns.at(-1)?.index ?? -1) + 1,
                    request_id: envelope.request_id,
                    user: dialogueParts(content, () => { this.value.truncated = true; }), steps: [] });
            } else this.value.incomplete = true;
        } else if (envelope.event === 'input_rejected') {
            const id = object(envelope.data)?.request_id;
            if (typeof id === 'string') this.pendingUsers.delete(id);
        } else if (envelope.event === 'model_response' && request?.operation !== 'compact') {
            const turn = this.value.turns.at(-1);
            const content = dialogueParts(object(envelope.data)?.content, () => { this.value.truncated = true; });
            if (turn) turn.steps.push({ index: (turn.steps.at(-1)?.index ?? -1) + 1, content,
                ...(answerSource(object(envelope.data)?.answer_source)
                    ? { answer_source: object(envelope.data)!.answer_source as AnswerSource } : {}) });
            else this.value.incomplete = true;
        }
        if (['input_committed', 'model_response', 'run_finished', 'compact_finished'].includes(envelope.event)) {
            this.generation += 1;
            this.value.stale = true;
            if (envelope.event === 'compact_finished') {
                // Compact replaces history, so a pre-compact snapshot cannot
                // continue collecting pages from the new history incarnation.
                this.cancelScheduled();
                this.cancelRefresh();
            }
            this.persist();
            if (settled || envelope.event === 'compact_finished') this.schedule(connection);
        } else if (envelope.event === 'ready' || envelope.event === 'status') {
            if (this.value.stale) this.schedule(connection);
        }
    }

    private turn(raw: unknown, mark: () => void): DialogueTurn | null {
        const turn = object(raw);
        if (!turn || !integer(turn.index) || !Array.isArray(turn.user) || !Array.isArray(turn.steps)) return null;
        if (turn.steps.length > MAX_TURN_STEPS) mark();
        const source = object(turn.source);
        const internal = turn.internal_input === 'auto_compact_continue'
            && source && typeof source.worker_id === 'string'
            && typeof source.request_id === 'string' && typeof source.run_id === 'string';
        if (turn.internal_input !== undefined && (!internal || turn.user.length !== 0)) return null;
        return { index: turn.index, user: internal ? [] : dialogueParts(turn.user, mark),
            ...(internal ? { internal_input: 'auto_compact_continue' as const,
                source: source as NonNullable<DialogueTurn['source']>, request_id: source.request_id as string } : {}),
            steps: turn.steps.slice(-MAX_TURN_STEPS).flatMap(rawStep => {
                const step = object(rawStep);
                if (!step || !integer(step.index) || !Array.isArray(step.content)) return [];
                if (step.omitted_parts) mark();
                return [{ index: step.index, content: dialogueParts(step.content, mark),
                    ...(answerSource(step.answer_source) ? { answer_source: step.answer_source } : {}) }];
            }) };
    }

    /** Timer callbacks cannot rely on the Hub's synchronous observer boundary. */
    private callback(task: () => void): void {
        if (this.stopped || this.session.closing) return;
        try { task(); }
        catch { this.failed(); }
    }

    /** One idle reconciliation budget; polling and model commits cannot renew it. */
    private schedule(connection: WorkerConnection): void {
        this.latestConnection = connection;
        if (this.stopped || this.scheduled || this.refresh || this.session.closing
            || this.session.activeRunId || this.attempts >= 3) return;
        this.scheduled = setTimeout(() => this.callback(() => {
            this.scheduled = null;
            const current = this.latestConnection;
            if (!current || this.session.connection !== current || !current.isOpen
                || this.session.closing || this.session.activeRunId) return;
            const capabilities = this.session.workerCapabilities;
            if (capabilities?.workerId !== this.session.identity.workerId || !capabilities?.names.includes('session-history')) return;
            this.attempts += 1;
            this.refresh = { connection: current, worker: this.session.identity.workerId!, request: '',
                start: 0, step: 0, revision: null, total: null, pages: 0, generation: this.generation,
                turns: [], pending: null, discovering: true, truncated: false, timer: null };
            this.query();
        }), 25);
        this.scheduled.unref();
    }

    private query(): void {
        const refresh = this.refresh;
        if (!refresh) return;
        refresh.request = newRequestId();
        refresh.timer = setTimeout(() => this.callback(() => this.failed()), 3000);
        refresh.timer.unref();
        try {
            const result = refresh.connection.sendPayload(buildPayload({ operation: 'history',
                requestId: refresh.request, start: refresh.start, step: refresh.step, limit: 1 }));
            if (!result.ok) this.failed();
        } catch { this.failed(); }
    }

    private history(envelope: ForwardedEnvelope, connection: WorkerConnection): void {
        const refresh = this.refresh;
        const page = object(envelope.data);
        // The envelope identifies the active run. History correlation is the
        // payload request_id, which can differ while a model run is active.
        if (!refresh || refresh.connection !== connection || envelope.worker_id !== refresh.worker
            || page?.request_id !== refresh.request) return;
        if (this.session.identity.workerId !== refresh.worker) { this.failed(); return; }
        if (refresh.timer) clearTimeout(refresh.timer);
        refresh.timer = null;
        if (!integer(page.revision) || !integer(page.total) || !integer(page.next) || !integer(page.next_step)
            || page.start !== refresh.start || page.step !== refresh.step || !Array.isArray(page.turns)
            || page.turns.length > 1 || (refresh.revision !== null && refresh.revision !== page.revision)
            || (refresh.total !== null && refresh.total !== page.total)
            || page.next > page.total) { this.failed(); return; }
        refresh.revision = page.revision;
        refresh.total = page.total;
        refresh.pages += 1;
        if (page.total === 0) {
            if (page.next || page.next_step || page.turns.length) { this.failed(); return; }
            this.finish(refresh, false);
            return;
        }
        const raw = object(page.turns[0]);
        const steps = raw?.steps;
        if (!raw || raw.index !== refresh.start || !Array.isArray(steps)
            || steps.some((rawStep, index) => {
                const step = object(rawStep);
                return !step || step.index !== refresh.step + index || !Array.isArray(step.content);
            }) || (page.next_step === 0 ? page.next !== refresh.start + 1
                : page.next !== refresh.start || !steps.length || page.next_step !== refresh.step + steps.length)) {
            this.failed(); return;
        }
        // Discover the revision/count once, then visit newest turns first.
        // Never publish the discovery page as an old prefix of a large history.
        if (refresh.discovering) {
            refresh.discovering = false;
            if (page.total > 1) {
                refresh.start = page.total - 1;
                this.query();
                return;
            }
        }
        const turn = this.turn(raw, () => { refresh.truncated = true; });
        if (!turn) { this.failed(); return; }
        if (raw.omitted_user_parts) refresh.truncated = true;
        if (page.next_step && refresh.pages >= MAX_REFRESH_PAGES) {
            // An unfinished older turn must not discard the fully validated
            // newest tail. If the latest turn itself is unfinished, keep the
            // prior projection instead. Invalid pages were rejected above.
            if (refresh.turns.length) {
                refresh.pending = null;
                this.finish(refresh, true);
            } else this.failed();
            return;
        }
        const remaining = raw.omitted_steps;
        if (page.next_step && integer(remaining)
            && Number.isSafeInteger(refresh.step + steps.length + remaining)) {
            const tailStart = refresh.step + steps.length + remaining - MAX_TURN_STEPS;
            if (tailStart > page.next_step) {
                refresh.truncated = true;
                refresh.step = tailStart;
                refresh.pending = null;
                this.query();
                return;
            }
        }
        if (refresh.pending) {
            if (refresh.pending.internal_input !== turn.internal_input
                || refresh.pending.source?.worker_id !== turn.source?.worker_id
                || refresh.pending.source?.request_id !== turn.source?.request_id
                || refresh.pending.source?.run_id !== turn.source?.run_id) {
                this.failed(); return;
            }
            refresh.pending.steps.push(...turn.steps);
        }
        else {
            const known = this.value.turns.find(item => item.index === turn.index);
            if ((!this.value.worker_id || this.value.worker_id === refresh.worker)
                && !turn.request_id && known?.request_id && JSON.stringify(known.user) === JSON.stringify(turn.user)) {
                turn.request_id = known.request_id;
            }
            refresh.pending = turn;
        }
        if (refresh.pending.steps.length > MAX_TURN_STEPS) {
            refresh.pending.steps = refresh.pending.steps.slice(-MAX_TURN_STEPS);
            refresh.truncated = true;
        }
        if (page.next_step) {
            this.fit([refresh.pending], () => { refresh.truncated = true; });
            refresh.step = page.next_step;
            this.query();
            return;
        }
        refresh.turns.unshift(refresh.pending);
        refresh.pending = null;
        let evicted = false;
        this.fit(refresh.turns, () => { evicted = true; refresh.truncated = true; });
        if (refresh.start === 0 || evicted || refresh.pages >= MAX_REFRESH_PAGES) {
            this.finish(refresh, refresh.start !== 0 || refresh.truncated);
        } else {
            refresh.start -= 1;
            refresh.step = 0;
            this.query();
        }
    }

    /** Publish only a validated tail that includes the latest completed turn. */
    private finish(refresh: Refresh, incomplete: boolean): void {
        if (refresh.generation !== this.generation) {
            // Do not regress live content or guess how provisional turn indices
            // map onto an older snapshot. Keep the live projection, then fetch
            // canonical indices after settlement, within the existing budget.
            this.cancelRefresh();
            this.schedule(refresh.connection);
            return;
        }
        this.value = { revision: refresh.revision, worker_id: refresh.worker,
            turns: refresh.turns, truncated: refresh.truncated || incomplete,
            incomplete, stale: false, refreshed_at: new Date().toISOString() };
        this.cancelRefresh();
        this.persist();
    }

    private failed(): void {
        const connection = this.refresh?.connection;
        this.cancelRefresh();
        this.value.stale = true;
        this.value.incomplete = true;
        this.persist();
        if (connection) this.schedule(connection);
    }

    /** Evict old turns/steps first; even an oversized latest answer keeps a prefix. */
    private fit(turns: DialogueTurn[], mark: () => void): void {
        for (const turn of turns) {
            if (turn.steps.length > MAX_TURN_STEPS) {
                turn.steps = turn.steps.slice(-MAX_TURN_STEPS);
                mark();
            }
        }
        const budget = this.maxBytes - 1024;
        while (Buffer.byteLength(JSON.stringify(turns)) > budget && turns.length) {
            mark();
            if (turns.length > 1) { turns.shift(); continue; }
            const turn = turns[0]!;
            if (turn.steps.length > 1) { turn.steps.shift(); continue; }
            // The final answer takes priority over verbose input and attachments.
            if (turn.user.length > 1) { turn.user.pop(); continue; }
            const input = turn.user[0];
            if (input && Buffer.byteLength(input.raw) > 256) {
                input.raw = prefix(input.raw, 256);
                continue;
            }
            const content = turn.steps[0]?.content;
            if (content && content.length > 1) { content.pop(); continue; }
            const part = content?.[0];
            if (part?.raw.length) {
                part.bytes ??= Buffer.byteLength(part.raw);
                part.truncated = true;
                part.raw = prefix(part.raw, Math.floor(Buffer.byteLength(part.raw) / 2));
            } else { turns.shift(); }
        }
    }

    /** Bound live memory immediately; coalesce reconstructible disk copies of it. */
    private persist(): void {
        if (this.stopped || this.session.closing) return;
        this.fit(this.value.turns, () => { this.value.truncated = true; this.value.incomplete = true; });
        this.storage.schedule();
    }

    /** Flush the current pending projection, including after admission is closed. */
    flush(): boolean {
        return this.storage.flush();
    }

    private cancelRefresh(): void {
        if (this.refresh?.timer) clearTimeout(this.refresh.timer);
        this.refresh = null;
    }

    private cancelScheduled(): void {
        if (this.scheduled) clearTimeout(this.scheduled);
        this.scheduled = null;
    }

    stop(): void {
        this.stopped = true;
        this.cancelScheduled();
        this.cancelRefresh();
        this.pendingUsers.clear();
        this.storage.stop();
    }
}
