/** Bounded primary dialogue only. Raw events, reasoning, tools and extras never persist. */
import { newRequestId, buildPayload } from '../protocol/messages.ts';
import type { Session } from '../state/registry.ts';
import type { ForwardedEnvelope, WorkerConnection } from '../worker/connection.ts';
import { readPrivate, writePrivate } from './storage.ts';

export interface DialoguePart { type: string; modality: string; raw: string }
export interface DialogueTurn {
    index: number;
    request_id?: string;
    user: DialoguePart[];
    steps: { index: number; content: DialoguePart[] }[];
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
    truncated: boolean;
    timer: NodeJS.Timeout | null;
}
const object = (value: unknown): Record<string, unknown> | null =>
    value !== null && typeof value === 'object' && !Array.isArray(value)
        ? value as Record<string, unknown> : null;
const integer = (value: unknown): value is number => Number.isSafeInteger(value) && (value as number) >= 0;

/** Keep visible content, with explicit truncation. Do not pass through extras. */
export function dialogueParts(value: unknown, mark: () => void): DialoguePart[] {
    if (!Array.isArray(value)) return [];
    if (value.length > 4) mark();
    const parts: DialoguePart[] = [];
    for (const raw of value.slice(0, 4)) {
        const part = object(raw);
        if (!part || !['text', 'external_ref'].includes(String(part.type))
            || typeof part.modality !== 'string' || typeof part.raw !== 'string') { mark(); continue; }
        if (part.truncated === true || part.omitted === true) mark();
        const bytes = Buffer.from(part.raw);
        if (bytes.length > 4096) mark();
        const text = bytes.subarray(0, 4096).toString('utf8');
        parts.push({ type: part.type as string, modality: part.modality.slice(0, 32), raw: text });
    }
    return parts;
}

/** One worker-backed current-history projection; retries are finite and correlated. */
export class ConversationProjection {
    value: Conversation = {
        revision: null, worker_id: null, turns: [], truncated: false,
        incomplete: true, stale: true, refreshed_at: null,
    };
    private refresh: Refresh | null = null;
    private generation = 0;
    private attempts = 0;
    private stopped = false;
    private scheduled: NodeJS.Timeout | null = null;
    private latestConnection: WorkerConnection | null = null;
    private pendingUsers = new Map<string, unknown>();
    private lastSequence: number | string | null = null;
    readonly session: Session;
    readonly path: string;
    readonly maxBytes: number;
    constructor(session: Session, path: string, maxBytes: number) {
        this.session = session;
        this.path = path;
        this.maxBytes = maxBytes;
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
        this.cancelRefresh();
        this.lastSequence = null;
        this.latestConnection = connection;
        this.value.stale = true;
        this.value.incomplete = true;
        if (!this.stopped) this.persist();
        if (connection) this.schedule(connection, true);
    }

    event(envelope: ForwardedEnvelope, connection: WorkerConnection): void {
        if (this.stopped || this.session.closing || this.session.connection !== connection) return;
        if (envelope.event === 'history') { this.history(envelope, connection); return; }
        if (envelope.event === 'history_error') {
            if (object(envelope.data)?.request_id === this.refresh?.request) this.failed();
            return;
        }
        const seq = envelope.sequence;
        if (this.lastSequence !== null && seq !== null) {
            try {
                if (BigInt(seq) <= BigInt(this.lastSequence)) return;
                if (BigInt(seq) > BigInt(this.lastSequence) + 1n) this.schedule(connection, true);
            } catch { this.value.incomplete = true; }
        }
        this.lastSequence = seq;
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
            if (turn) turn.steps.push({ index: (turn.steps.at(-1)?.index ?? -1) + 1, content });
            else this.value.incomplete = true;
        }
        if (['input_committed', 'model_response', 'run_finished', 'compact_finished'].includes(envelope.event)) {
            this.generation += 1;
            this.value.stale = true;
            this.persist();
            this.schedule(connection, true);
        } else if (envelope.event === 'ready' || envelope.event === 'status') {
            if (this.value.stale) this.schedule(connection, true);
        }
    }

    private turn(raw: unknown, mark: () => void): DialogueTurn | null {
        const turn = object(raw);
        if (!turn || !integer(turn.index) || !Array.isArray(turn.user) || !Array.isArray(turn.steps)) return null;
        if (turn.steps.length > 512) mark();
        return { index: turn.index, user: dialogueParts(turn.user, mark),
            steps: turn.steps.slice(0, 512).flatMap(rawStep => {
                const step = object(rawStep);
                if (!step || !integer(step.index) || !Array.isArray(step.content)) return [];
                if (step.omitted_parts) mark();
                return [{ index: step.index, content: dialogueParts(step.content, mark) }];
            }) };
    }

    private schedule(connection: WorkerConnection, reset = false): void {
        this.latestConnection = connection;
        if (reset) this.attempts = 0;
        if (this.stopped || this.scheduled || this.refresh || this.session.closing) return;
        this.scheduled = setTimeout(() => {
            this.scheduled = null;
            const current = this.latestConnection;
            if (!current || this.session.connection !== current || !current.isOpen || this.session.closing) return;
            const capabilities = this.session.workerCapabilities;
            if (capabilities?.workerId !== this.session.identity.workerId || !capabilities?.names.includes('session-history')) return;
            if (this.attempts++ >= 3) return;
            this.refresh = { connection: current, worker: this.session.identity.workerId!, request: '',
                start: 0, step: 0, revision: null, total: null, pages: 0, generation: this.generation,
                turns: [], truncated: false, timer: null };
            this.query();
        }, 25);
        this.scheduled.unref();
    }

    private query(): void {
        const refresh = this.refresh;
        if (!refresh) return;
        refresh.request = newRequestId();
        refresh.timer = setTimeout(() => this.failed(), 3000);
        refresh.timer.unref();
        const result = refresh.connection.sendPayload(buildPayload({ operation: 'history',
            requestId: refresh.request, start: refresh.start, step: refresh.step, limit: 10 }));
        if (!result.ok) this.failed();
    }

    private history(envelope: ForwardedEnvelope, connection: WorkerConnection): void {
        const refresh = this.refresh;
        const page = object(envelope.data);
        // The envelope identifies the active run. History correlation is the
        // payload request_id, which can differ while a model run is active.
        if (!refresh || refresh.connection !== connection || envelope.worker_id !== refresh.worker
            || page?.request_id !== refresh.request) return;
        if (refresh.timer) clearTimeout(refresh.timer);
        refresh.timer = null;
        if (!integer(page.revision) || !integer(page.total) || !integer(page.next) || !integer(page.next_step)
            || page.start !== refresh.start || page.step !== refresh.step || !Array.isArray(page.turns)
            || (refresh.revision !== null && refresh.revision !== page.revision)
            || (refresh.total !== null && refresh.total !== page.total)
            || page.next > page.total || (page.next === page.total && page.next_step !== 0)
            || (page.next < page.total && (page.next < refresh.start
                || (page.next === refresh.start && page.next_step <= refresh.step)))
            || refresh.generation !== this.generation) { this.failed(); return; }
        refresh.revision = page.revision;
        refresh.total = page.total;
        let expectedTurn = refresh.start;
        let expectedStep = refresh.step;
        for (const raw of page.turns) {
            const source = object(raw);
            const turn = this.turn(raw, () => { refresh.truncated = true; });
            if (!turn || turn.index !== expectedTurn || turn.index >= page.total
                || turn.steps.some((step, index) => step.index !== expectedStep + index)) { this.failed(); return; }
            if (source?.omitted_user_parts) refresh.truncated = true;
            const previous = refresh.turns.at(-1);
            if (previous?.index === turn.index) previous.steps.push(...turn.steps);
            else {
                // History omits request IDs; retain a known live association
                // only for the same worker, index and visible user content.
                const known = this.value.turns.find(item => item.index === turn.index);
                if ((!this.value.worker_id || this.value.worker_id === refresh.worker)
                    && known?.request_id && JSON.stringify(known.user) === JSON.stringify(turn.user)) {
                    turn.request_id = known.request_id;
                }
                refresh.turns.push(turn);
            }
            expectedTurn += 1;
            expectedStep = 0;
        }
        const last = refresh.turns.at(-1);
        // The two cursors must describe exactly the assembled page, including
        // a continuation inside its final turn. Never silently skip a turn.
        if (page.next_step === 0) {
            if (page.next !== refresh.start + page.turns.length) { this.failed(); return; }
        } else if (!page.turns.length || !last || page.next !== last.index
            || page.next_step !== (last.steps.at(-1)?.index ?? -1) + 1) {
            this.failed(); return;
        }
        refresh.pages += 1;
        const budgetReached = Buffer.byteLength(JSON.stringify(refresh.turns)) > this.maxBytes - 2048 || refresh.pages >= 64;
        if (page.next === page.total || budgetReached) {
            this.value = { revision: page.revision, worker_id: refresh.worker,
                turns: refresh.turns, truncated: refresh.truncated || this.value.truncated || budgetReached,
                incomplete: budgetReached && page.next !== page.total, stale: false,
                refreshed_at: new Date().toISOString() };
            this.cancelRefresh();
            this.persist();
        } else {
            refresh.start = page.next;
            refresh.step = page.next_step;
            this.query();
        }
    }

    private failed(): void {
        const connection = this.refresh?.connection;
        this.cancelRefresh();
        this.value.stale = true;
        this.value.incomplete = true;
        this.persist();
        if (connection) this.schedule(connection);
    }

    private persist(): void {
        if (this.stopped || this.session.closing) return;
        while (Buffer.byteLength(JSON.stringify(this.value)) > this.maxBytes - 1) {
            if (this.value.turns.length <= 1) {
                const turn = this.value.turns[0];
                if (turn?.steps.length) turn.steps.shift();
                else this.value.turns.shift();
            } else this.value.turns.shift();
            this.value.truncated = true;
            this.value.incomplete = true;
        }
        writePrivate(this.path, this.value, this.maxBytes);
    }

    private cancelRefresh(): void {
        if (this.refresh?.timer) clearTimeout(this.refresh.timer);
        this.refresh = null;
    }

    stop(): void {
        this.stopped = true;
        if (this.scheduled) clearTimeout(this.scheduled);
        this.scheduled = null;
        this.cancelRefresh();
        this.pendingUsers.clear();
    }
}
