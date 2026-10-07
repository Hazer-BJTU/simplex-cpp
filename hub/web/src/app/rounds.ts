/**
 * @file folding a transcript into rounds.
 *
 * The old panel gave every event its own card, in arrival order, so a
 * conversation was a flat stream in which `persisted` and `input_committed`
 * sat at the same weight as the model's answer. This module is the layer that
 * was missing: it decides what belongs to what, so the renderer can draw a
 * turn — the operator's message, the model's replies, the tools it asked for,
 * the results — as one readable unit.
 *
 * It is a pure function of the transcript and the open prompts, which is what
 * makes it testable under `node --test` and keeps the renderer free of
 * `if (envelope.event === …)` chains.
 *
 * Three things it deliberately does not invent:
 *
 * - **Per-run model.** Neither `model_response` nor the `status` object names
 *   the model, so a round cannot say which one produced it. The model is
 *   session-level and belongs in the header.
 * - **Tool duration.** No per-tool start or finish event exists in the worker
 *   protocol. A round reports the wall clock from its first envelope to its
 *   last, and — when a tool's own output reports one — the process runtime the
 *   worker measured itself. Both are labelled as what they are.
 * - **A success flag.** The protocol has none: a result is a failure only when
 *   the tool framework annotated one, and "not executed" is a third state.
 */
import type { ConfirmationPrompt, HistoryTurn, RequestRecord, WorkerEnvelope } from '../../../shared/protocol.ts';
import { parseCompactResult, type CompactResult } from '../state/compact.ts';
import type {
    EventItem,
    NoteItem,
    OutboxItem,
    RequestItem,
    TranscriptItem,
} from '../state/view.ts';
import {
    callView,
    clockOf,
    contentText,
    costLine,
    obj,
    partsText,
    resultView,
    sinceMs,
    str,
    type CallView,
    type ResultView,
} from './content.ts';
import { parseToolOutput, type ToolOutput } from './toolOutput.ts';

/** Where a proposed call has got to. */
export type CallStatus =
    /** A prompt is open: the call is waiting for a decision. */
    | 'pending'
    /** Proposed, and no result or prompt yet. */
    | 'running'
    | 'ok'
    | 'failed'
    /** The loop did not dispatch it; not an execution and not a failure. */
    | 'skipped'
    | 'cancelled'
    /** The run finished without a result for this call. */
    | 'unknown';

/** One tool call, with whatever has happened to it so far. */
export interface ToolCall {
    readonly key: string;
    readonly id: string;
    readonly name: string;
    readonly args: unknown;
    /** The settled classification when a result exists, the proposed one before. */
    readonly security: string;
    readonly scheduling: string;
    readonly status: CallStatus;
    readonly result: ResultView | null;
    /** The result text, read for structure. Null until a result arrives. */
    readonly output: ToolOutput | null;
    readonly prompt: ConfirmationPrompt | null;
    /** The runtime the tool itself reported, when it reported one. */
    readonly reportedMs: number | null;
    /** Wall clock from the proposing envelope to the result, or null. */
    readonly elapsedMs: number | null;
}

/** One complete model response. */
export interface AssistantBlock {
    readonly key: string;
    readonly envelope: WorkerEnvelope;
    readonly text: string;
    readonly reasoning: string;
    readonly cost: string;
    readonly tokens: number | null;
    readonly clock: string;
    /** Keys of the calls this response proposed. */
    readonly callIds: readonly string[];
    /** A display-only fallback; live tool cards stay in the execution round. */
    readonly historyStep?: HistoryTurn['steps'][number];
}

/** Something the worker or the panel reported as a problem. */
export interface Problem {
    readonly key: string;
    readonly label: string;
    readonly text: string;
    readonly tone: 'warn' | 'error';
}

/** Settled run failure, with optional classification from newer workers. */
export interface RunFailure {
    readonly stage: 'model_request' | 'other';
    readonly operation?: 'compact' | 'auto_compact';
    readonly canContinue: boolean;
    readonly error: string;
}

/**
 * The order things happened in, within one round.
 *
 * Presentation groups messages and tools, while this sequence also retains
 * boundaries used to position restored responses. Protocol entries are not
 * rendered inline; they remain essential for replay and execution identity.
 */
export type TimelineEntry =
    | { readonly kind: 'compact'; readonly key: string }
    | { readonly kind: 'protocol'; readonly key: string }
    | { readonly kind: 'assistant'; readonly key: string }
    | { readonly kind: 'problem'; readonly key: string }
    /** A batch of calls no model response claimed. */
    | { readonly kind: 'calls'; readonly key: string }
    /** A note the panel itself added. */
    | { readonly kind: 'note'; readonly key: string };

/** One turn: an input, everything it caused, and how it settled. */
export interface Round {
    readonly key: string;
    /** 0 for the loose items before the first run, then 1, 2, 3 … in run order. */
    readonly index: number;
    readonly kind: 'prelude' | 'run';
    readonly input: OutboxItem | null;
    /** An admission replayed from the hub, whose text the protocol does not carry. */
    readonly admitted: EventItem | null;
    /** A continuation uses the worker's state and has no user message. */
    readonly continued: boolean;
    readonly compacting: boolean;
    readonly compactResult: CompactResult | null;
    readonly assistant: readonly AssistantBlock[];
    readonly calls: readonly ToolCall[];
    readonly problems: readonly Problem[];
    /** What happened, in order. */
    readonly timeline: readonly TimelineEntry[];
    /** Execution identities and boundaries retained for history reconciliation. */
    readonly protocol: readonly EventItem[];
    readonly notes: readonly NoteItem[];
    readonly requests: readonly RequestItem[];
    /** `run_finished`'s status, or `''` while the run is still open. */
    readonly status: string;
    readonly failure: RunFailure | null;
    readonly exchanges: number | null;
    readonly tokens: number | null;
    /** Wall clock from the round's first envelope to its last. */
    readonly wallMs: number | null;
    readonly open: boolean;
    readonly clock: string;
}

/** Bookkeeping events: real, but not conversation. */
const PROTOCOL_EVENTS = new Set([
    'ready', 'status', 'options', 'input_committed', 'run_started',
    'persisted', 'run_finished',
]);

/** Events whose payload is a problem report rather than content. */
const PROBLEM_EVENTS = new Set(['input_rejected', 'error', 'export_error']);

/** Drafts carry the prompt index so a call can find its own approval. */
interface Prompts {
    readonly byId: ReadonlyMap<string, ConfirmationPrompt>;
    readonly byName: ReadonlyMap<string, ConfirmationPrompt>;
}

/** A mutable draft, so the fold can be written in reading order. */
interface Draft {
    key: string;
    index: number;
    kind: 'prelude' | 'run';
    /** Wire identities are scoped to a worker; pending inputs have no run ID. */
    requestId: string;
    workerId: string;
    runId: string;
    /** Includes continuations/compactions, which do not render a user bubble. */
    localInput: OutboxItem | null;
    input: OutboxItem | null;
    admitted: EventItem | null;
    continued: boolean;
    compacting: boolean;
    compactResult: CompactResult | null;
    assistant: AssistantBlock[];
    calls: ToolCall[];
    problems: Problem[];
    timeline: TimelineEntry[];
    protocol: EventItem[];
    notes: NoteItem[];
    requests: RequestItem[];
    status: string;
    failure: RunFailure | null;
    exchanges: number | null;
    tokens: number | null;
    firstAt: number | null;
    lastAt: number | null;
    open: boolean;
    clock: string;
    /** When each call was proposed, so its result can be timed against it. */
    proposedAt: Map<string, number | null>;
    /** Calls whose result has already been attached. */
    settled: Set<string>;
}

function newDraft(key: string, index: number, kind: 'prelude' | 'run'): Draft {
    return {
        key,
        index,
        kind,
        requestId: '',
        workerId: '',
        runId: '',
        localInput: null,
        input: null,
        admitted: null,
        continued: false,
        compacting: false,
        compactResult: null,
        assistant: [],
        calls: [],
        problems: [],
        timeline: [],
        protocol: [],
        notes: [],
        requests: [],
        status: '',
        failure: null,
        exchanges: null,
        tokens: null,
        firstAt: null,
        lastAt: null,
        open: kind === 'run',
        clock: '',
        proposedAt: new Map(),
        settled: new Set(),
    };
}

/** Read a millisecond timestamp, or null. */
function stampOf(envelope: WorkerEnvelope): number | null {
    const at = Date.parse(str(envelope.received_at));
    return Number.isNaN(at) ? null : at;
}

/** The stable identity of a call within one round. */
function keyOf(call: CallView, ordinal: number): string {
    return call.id || `${call.name}#${ordinal}`;
}

/** Settle a call's status from everything known about it. */
function statusOf(
    result: ResultView | null,
    prompt: ConfirmationPrompt | null,
    finished: boolean,
): CallStatus {
    if (prompt && prompt.settled_at === null) return 'pending';
    if (result) {
        if (result.cancelled) return 'cancelled';
        if (result.skipped) return 'skipped';
        return result.error ? 'failed' : 'ok';
    }
    if (finished) return 'unknown';
    return 'running';
}

/**
 * The open prompt that answers a proposed call.
 *
 * A call that carries an id is matched only by id: matching it by name as well
 * would hand one call's approval to another call of the same tool, and a prompt
 * is the one thing here that grants something.
 */
function promptFor(prompts: Prompts, view: CallView): ConfirmationPrompt | null {
    if (view.id) return prompts.byId.get(view.id) ?? null;
    return prompts.byName.get(view.name) ?? null;
}

/**
 * Per-projection parsing caches. Weak keys follow immutable event lifetimes:
 * trimming/replacing a transcript never retains its arguments or output here.
 * The uncached fold remains the correctness reference.
 */
export class RoundDerivations {
    private readonly results = new WeakMap<WorkerEnvelope, ResultView[]>();
    private readonly outputs = new WeakMap<ResultView, ToolOutput>();

    resultsOf(envelope: WorkerEnvelope): ResultView[] {
        let result = this.results.get(envelope);
        if (!result) {
            result = resultsOf(envelope);
            this.results.set(envelope, result);
        }
        return result;
    }

    outputOf(result: ResultView): ToolOutput {
        let output = this.outputs.get(result);
        if (!output) {
            output = parseToolOutput(result.text);
            this.outputs.set(result, output);
        }
        return output;
    }
}

/** Read the calls a model response proposed. */
function invokesOf(envelope: WorkerEnvelope): CallView[] {
    const message = obj(envelope.data) ?? {};
    return Array.isArray(message.invokes) ? message.invokes.map(callView) : [];
}

/** Read a `tool_calls` batch. */
function batchOf(envelope: WorkerEnvelope): CallView[] {
    return Array.isArray(envelope.data) ? envelope.data.map(callView) : [];
}

/** Read a `tool_results` batch, in the shape core actually sends. */
function resultsOf(envelope: WorkerEnvelope): ResultView[] {
    return Array.isArray(envelope.data) ? envelope.data.map(resultView) : [];
}

/**
 * Add a batch of proposed calls to a round, merging by identity.
 *
 * `tool_calls` and `model_response.invokes` describe the same batch: core emits
 * the response containing the calls and then a `tool_calls` event for it. The
 * old panel drew a card for each, so one `run_command` appeared twice. They are
 * merged by id here — but only by id, because the protocol allows them to
 * differ (the batch is pre-dispatch, the response is the message), so a call
 * that appears in only one of them stays a card of its own rather than being
 * dropped.
 */
function addCalls(
    draft: Draft,
    prompts: Prompts,
    views: readonly CallView[],
    envelope: WorkerEnvelope,
): string[] {
    const keys: string[] = [];
    for (const view of views) {
        const ordinal = draft.calls.filter((call) => call.name === view.name).length;
        const key = keyOf(view, ordinal);
        keys.push(key);
        if (draft.calls.some((call) => call.key === key)) continue;
        const prompt = promptFor(prompts, view);
        draft.calls.push({
            key,
            id: view.id,
            name: view.name,
            args: view.args,
            security: view.security,
            scheduling: view.scheduling,
            status: statusOf(null, prompt, false),
            result: null,
            output: null,
            prompt,
            reportedMs: null,
            elapsedMs: null,
        });
        draft.proposedAt.set(key, stampOf(envelope));
    }
    return keys;
}

/** Attach a settled result to its call. */
function settle(
    draft: Draft,
    call: ToolCall,
    result: ResultView,
    envelope: WorkerEnvelope,
    derivations?: RoundDerivations,
): ToolCall {
    const output = derivations?.outputOf(result) ?? parseToolOutput(result.text);
    const proposedAt = draft.proposedAt.get(call.key);
    return {
        ...call,
        // The result's call has been through security evaluation; the proposed
        // one had not, and the protocol warns against reading authority from it.
        security: result.security || call.security,
        result,
        output,
        status: statusOf(result, call.prompt, true),
        reportedMs: output.kind === 'document' ? output.document.reportedMs : null,
        elapsedMs: proposedAt === undefined || proposedAt === null
            ? null
            : sinceMs(envelope, proposedAt),
    };
}

/**
 * Match a result to the call it answers.
 *
 * Ids are the protocol's own correlation and are used whenever both sides have
 * one. A result that arrives without one is matched to the earliest call of the
 * same name that has not been settled, which is the best available reading.
 */
function matchCall(draft: Draft, result: ResultView): ToolCall | null {
    if (result.id) return draft.calls.find((call) => call.id === result.id) ?? null;
    return draft.calls.find(
        (call) => call.name === result.name && !draft.settled.has(call.key),
    ) ?? null;
}

/** Note the envelope's time span, so a round can report its wall clock. */
function track(draft: Draft, envelope: WorkerEnvelope): void {
    if (!draft.clock) draft.clock = clockOf(envelope);
    const at = stampOf(envelope);
    if (at === null) return;
    if (draft.firstAt === null || at < draft.firstAt) draft.firstAt = at;
    if (draft.lastAt === null || at > draft.lastAt) draft.lastAt = at;
}

/**
 * Associate inputs and execution independently of their arrival order.
 *
 * A locally sent input or Hub request record gets its own pending draft. It
 * becomes a numbered run only when a worker execution event identifies it.
 * Incoming run IDs take precedence over request IDs, and both are scoped by
 * worker ID so a restarted worker cannot inherit an earlier run's results.
 * Rejections use data.request_id and never select the active run, including
 * envelopes from older workers that still echo the last admitted run ID.
 */
class RoundGrouping {
    private readonly drafts: Draft[] = [];
    private readonly byRun = new Map<string, Draft>();
    private readonly byRequest = new Map<string, Draft>();
    private readonly inputs = new Map<string, Draft[]>();
    private current: Draft | null = null;
    private loose: Draft | null = null;
    private runCount = 0;

    private identity(worker: string, id: string): string {
        return JSON.stringify([worker, id]);
    }

    private create(requestId = ''): Draft {
        const draft = newDraft(`round-${this.drafts.length}`, 0, 'prelude');
        draft.requestId = requestId;
        this.drafts.push(draft);
        if (requestId) {
            const candidates = this.inputs.get(requestId) ?? [];
            candidates.push(draft);
            this.inputs.set(requestId, candidates);
        }
        return draft;
    }

    /** A new request cannot move the current execution cursor. */
    pending(requestId: string): Draft {
        return this.inputs.get(requestId)?.at(-1) ?? this.create(requestId);
    }

    /** A repeated local ID is a new attempt, not an edit of an earlier run. */
    input(item: OutboxItem): Draft {
        const draft = this.inputs.get(item.requestId)?.findLast((candidate) =>
            candidate.kind === 'prelude' && !candidate.status && !candidate.localInput)
            ?? this.create(item.requestId);
        draft.localInput = item;
        draft.workerId = item.admittedWorker ?? '';
        return draft;
    }

    private unbound(requestId: string, envelope: WorkerEnvelope): Draft | undefined {
        const workerId = str(envelope.worker_id);
        return this.inputs.get(requestId)?.findLast((draft) =>
            draft.kind === 'prelude' && !draft.status
            && (!draft.workerId || draft.workerId === workerId)
            && (draft.localInput?.admittedSequence === undefined
                || (typeof envelope.sequence === 'number'
                    && envelope.sequence >= draft.localInput.admittedSequence)));
    }

    /** A known run is authoritative; a different run must not reuse its draft. */
    known(envelope: WorkerEnvelope): Draft | undefined {
        const worker = str(envelope.worker_id);
        const run = str(envelope.run_id);
        if (run) {
            const draft = this.byRun.get(this.identity(worker, run));
            if (draft) return draft;
        }
        const request = str(envelope.request_id);
        if (!request) return undefined;
        const draft = this.byRequest.get(this.identity(worker, request));
        return draft && (!run || !draft.runId || draft.runId === run) ? draft : undefined;
    }

    private bind(draft: Draft, envelope: WorkerEnvelope): void {
        draft.workerId ||= str(envelope.worker_id);
        draft.runId ||= str(envelope.run_id);
        draft.requestId ||= str(envelope.request_id);
        if (draft.runId) this.byRun.set(this.identity(draft.workerId, draft.runId), draft);
        if (draft.requestId) this.byRequest.set(this.identity(draft.workerId, draft.requestId), draft);
    }

    /** Select the identified run, creating it for a replay beginning mid-run. */
    execution(envelope: WorkerEnvelope): Draft {
        const request = str(envelope.request_id);
        let draft = this.known(envelope);
        // Request IDs may be reused after the admission window. Never reopen a
        // rejection; a fresh admission after completion also starts a new run.
        if (draft?.status === 'rejected'
            || (envelope.event === 'input_admitted' && draft?.status)) draft = undefined;
        if (!draft) {
            draft = this.unbound(request, envelope);
            if (!draft && !request && !str(envelope.run_id)
                && this.current?.workerId === str(envelope.worker_id)) draft = this.current;
            draft ??= this.create(request);
        }
        this.bind(draft, envelope);
        if (draft.kind !== 'run') {
            draft.kind = 'run';
            draft.index = ++this.runCount;
            draft.open = true;
            this.current = draft;
            this.loose = null;
        }
        return draft;
    }

    /** Keep even unidentifiable refusals visible without changing execution. */
    rejected(item: EventItem): Draft {
        const value = obj(item.envelope.data)?.request_id;
        const request = typeof value === 'string' ? value : '';
        const worker = str(item.envelope.worker_id);
        const draft = this.unbound(request, item.envelope) ?? this.create(request);
        // Do not overwrite execution lookup for an already admitted request
        // when an older worker rejects a duplicate carrying that same ID.
        draft.workerId = worker;
        draft.status = 'rejected';
        draft.open = false;
        return draft;
    }

    /** Bookkeeping may refer to an already completed run; it cannot open one. */
    bookkeeping(envelope?: WorkerEnvelope): Draft {
        if (envelope) {
            const known = this.known(envelope);
            if (known) return known;
            const worker = str(envelope.worker_id);
            if (this.current && (!worker || this.current.workerId === worker)
                && !str(envelope.run_id) && !str(envelope.request_id)) return this.current;
        } else if (this.current) {
            return this.current;
        }
        this.loose ??= this.create();
        return this.loose;
    }

    /** Settling one run must not close another run or promote a queued input. */
    finished(draft: Draft): void {
        draft.open = false;
        if (this.current === draft) {
            this.current = null;
            this.loose = null;
        }
    }

    /**
     * Return executed rounds in worker admission order, not outbox creation
     * order. A local pending input can precede an earlier admission from another
     * panel. Keep non-run slots in place, but fill run slots by their execution
     * index so history matching and latest-run selection share the same order.
     * A replay starting mid-run uses its first retained execution event.
     */
    ordered(): Draft[] {
        const runs = this.drafts.filter((draft) => draft.kind === 'run')
            .sort((left, right) => left.index - right.index);
        let nextRun = 0;
        return this.drafts.map((draft) => (
            draft.kind === 'run' ? runs[nextRun++]! : draft
        ));
    }
}

/** Build transcript rounds, returning executed runs in admission order. */
export function buildRounds(
    items: readonly TranscriptItem[],
    confirmations: ReadonlyMap<string, ConfirmationPrompt>,
    requests: ReadonlyMap<string, RequestRecord> = new Map(),
    derivations?: RoundDerivations,
): Round[] {
    // Older workers left admission data empty. Their request records can help
    // while retained, but newer replayable admission events take precedence.
    const continuationIds = new Set<string>();
    const compactIds = new Set<string>();
    for (const request of requests.values()) {
        if (request.operation === 'continue') continuationIds.add(request.request_id);
        if (request.operation === 'compact') compactIds.add(request.request_id);
    }
    for (const item of items) {
        if (item.kind === 'outbox' && item.operation === 'continue') {
            continuationIds.add(item.requestId);
        } else if (item.kind === 'outbox' && item.operation === 'compact') {
            compactIds.add(item.requestId);
        } else if (item.kind === 'request' && item.request.operation === 'continue') {
            continuationIds.add(item.request.request_id);
        } else if (item.kind === 'request' && item.request.operation === 'compact') {
            compactIds.add(item.request.request_id);
        }
    }
    const byId = new Map<string, ConfirmationPrompt>();
    const byName = new Map<string, ConfirmationPrompt>();
    const prompts: Prompts = { byId, byName };
    for (const prompt of confirmations.values()) {
        if (prompt.settled_at !== null) continue;
        if (prompt.call?.id) byId.set(prompt.call.id, prompt);
        if (prompt.call?.name) byName.set(prompt.call.name, prompt);
    }

    const groups = new RoundGrouping();

    for (const item of items) {
        if (item.kind === 'outbox') {
            const pending = groups.input(item);
            if (item.operation === 'continue') pending.continued = true;
            else if (item.operation === 'compact') pending.compacting = true;
            else pending.input = item;
            continue;
        }

        if (item.kind === 'note') {
            const draft = groups.bookkeeping();
            draft.notes.push(item);
            draft.timeline.push({ kind: 'note', key: item.id });
            continue;
        }

        if (item.kind === 'request') {
            const pending = groups.pending(item.request.request_id);
            pending.requests.push(item);
            if (item.request.operation === 'continue') pending.continued = true;
            if (item.request.operation === 'compact') pending.compacting = true;
            continue;
        }

        const { envelope } = item;
        const name = str(envelope.event);

        if (name === 'input_admitted') {
            const run = groups.execution(envelope);
            const operation = str(obj(envelope.data)?.operation);
            if (operation === 'compact') {
                run.compacting = true;
            } else if (operation === 'continue'
                || (!operation && continuationIds.has(envelope.request_id))) {
                run.continued = true;
            } else if (run.input === null) {
                run.admitted = item;
            }
            run.open = true;
            track(run, envelope);
            run.protocol.push(item);
            run.timeline.push({ kind: 'protocol', key: item.id });
            continue;
        }

        if (name === 'compact_finished') {
            const result = parseCompactResult(envelope.data);
            if (result) {
                const run = groups.execution(envelope);
                if (result.origin === 'automatic') {
                    // The paired host tool card owns progress; this event only
                    // invalidates history and must not turn the run into compact.
                    track(run, envelope);
                    continue;
                }
                run.compacting = true;
                run.compactResult = result;
                track(run, envelope);
                run.timeline.push({ kind: 'compact', key: item.id });
                continue;
            }
        }

        if (name === 'run_started') {
            // Follows its admission inside the same turn, so this continues the
            // run in hand rather than opening another.
            const run = groups.execution(envelope);
            run.open = true;
            track(run, envelope);
            run.protocol.push(item);
            run.timeline.push({ kind: 'protocol', key: item.id });
            continue;
        }

        if (name === 'run_finished') {
            const summary = obj(envelope.data) ?? {};
            const run = groups.execution(envelope);
            run.status = str(summary.status) || 'finished';
            if (run.status === 'failed') {
                const failure = obj(summary.failure) ?? {};
                run.failure = {
                    stage: failure.stage === 'model_request' ? 'model_request' : 'other',
                    ...(failure.operation === 'compact' || failure.operation === 'auto_compact'
                        ? { operation: failure.operation } : {}),
                    canContinue: failure.can_continue === true,
                    error: str(summary.error),
                };
            }
            run.exchanges = typeof summary.exchanges === 'number' ? summary.exchanges : null;
            track(run, envelope);
            run.protocol.push(item);
            run.timeline.push({ kind: 'protocol', key: item.id });
            run.open = false;
            // A call the run never answered is not still running: the turn is
            // over and no result was reported for it. Saying so is the whole
            // difference between "waiting" and "we were never told".
            run.calls = run.calls.map((call) => (call.result
                ? call
                : { ...call, status: statusOf(null, call.prompt, true) }));
            groups.finished(run);
            continue;
        }

        if (PROTOCOL_EVENTS.has(name)) {
            const draft = name === 'input_committed'
                ? groups.execution(envelope) : groups.bookkeeping(envelope);
            track(draft, envelope);
            draft.protocol.push(item);
            draft.timeline.push({ kind: 'protocol', key: item.id });
            continue;
        }

        if (name === 'model_response') {
            const run = groups.execution(envelope);
            track(run, envelope);
            const message = obj(envelope.data) ?? {};
            const cost = costLine(message.cost);
            const callIds = addCalls(run, prompts, invokesOf(envelope), envelope);
            run.assistant.push({
                key: item.id,
                envelope,
                text: partsText(message.content),
                reasoning: contentText(message.reasoning),
                cost: cost.text,
                tokens: cost.total,
                clock: clockOf(envelope),
                callIds,
            });
            if (cost.total !== null) run.tokens = (run.tokens ?? 0) + cost.total;
            run.timeline.push({ kind: 'assistant', key: item.id });
            continue;
        }

        if (name === 'tool_calls') {
            const run = groups.execution(envelope);
            track(run, envelope);
            const claimed = new Set(run.assistant.flatMap((block) => [...block.callIds]));
            for (const key of addCalls(run, prompts, batchOf(envelope), envelope)) {
                // A card the model response already claims is drawn with it, in
                // the response's place rather than the batch's.
                if (!claimed.has(key)) run.timeline.push({ kind: 'calls', key });
            }
            continue;
        }

        if (name === 'tool_results') {
            const run = groups.execution(envelope);
            track(run, envelope);
            for (const result of derivations?.resultsOf(envelope) ?? resultsOf(envelope)) {
                const call = matchCall(run, result);
                if (!call) {
                    // A result whose proposal is not in this transcript: a
                    // replay can begin mid-turn. Shown rather than dropped.
                    const output = derivations?.outputOf(result) ?? parseToolOutput(result.text);
                    const key = `orphan-${item.id}-${run.calls.length}`;
                    run.timeline.push({ kind: 'calls', key });
                    run.calls.push({
                        key,
                        id: result.id,
                        name: result.name,
                        args: undefined,
                        security: result.security,
                        scheduling: '',
                        status: statusOf(result, null, true),
                        result,
                        output,
                        prompt: null,
                        reportedMs: output.kind === 'document'
                            ? output.document.reportedMs
                            : null,
                        elapsedMs: null,
                    });
                    continue;
                }
                const at = run.calls.indexOf(call);
                run.calls[at] = settle(run, call, result, envelope, derivations);
                run.settled.add(call.key);
            }
            continue;
        }

        if (PROBLEM_EVENTS.has(name)) {
            const rejection = obj(envelope.data);
            const rejectedRequestId = rejection?.request_id;
            const queueFull = name === 'input_rejected' && rejection?.code === 'payload_queue_full';
            const run = name === 'input_rejected'
                ? groups.rejected(item) : groups.bookkeeping(envelope);
            if (name === 'input_rejected' && (rejection?.operation === 'compact'
                || (typeof rejectedRequestId === 'string' && compactIds.has(rejectedRequestId)))) {
                run.compacting = true;
            }
            if (name === 'input_rejected' && run.compacting) {
                run.status = 'rejected';
                run.open = false;
            }
            track(run, envelope);
            const data = obj(envelope.data) ?? {};
            run.problems.push({
                key: item.id,
                label: name === 'input_rejected'
                    ? queueFull ? 'Input queue full' : 'input rejected'
                    : name === 'error' ? 'worker diagnostic' : 'markdown export failed',
                text: str(data.message) || 'the worker reported a problem',
                tone: name === 'error' ? 'error' : 'warn',
            });
            run.timeline.push({ kind: 'problem', key: item.id });
            continue;
        }

        // An event name this build has never heard of: core is allowed to add
        // them, and a panel that dropped one would lose part of the turn.
        const run = groups.execution(envelope);
        track(run, envelope);
        run.problems.push({
            key: item.id,
            label: name || '(unnamed event)',
            text: 'this panel has no renderer for that event yet',
            tone: 'warn',
        });
        run.timeline.push({ kind: 'problem', key: item.id });
    }

    return groups.ordered().filter((draft) => draft.input !== null
        || draft.admitted !== null || draft.continued || draft.compacting
        || draft.assistant.length > 0 || draft.calls.length > 0
        || draft.protocol.length > 0 || draft.problems.length > 0
        || draft.notes.length > 0 || draft.requests.length > 0
    ).map((draft) => ({
        key: draft.key,
        index: draft.index,
        kind: draft.kind,
        input: draft.input,
        admitted: draft.admitted,
        continued: draft.continued,
        compacting: draft.compacting,
        compactResult: draft.compactResult,
        assistant: draft.assistant,
        calls: draft.calls,
        problems: draft.problems,
        timeline: draft.timeline,
        protocol: draft.protocol,
        notes: draft.notes,
        requests: draft.requests,
        status: draft.status,
        failure: draft.failure,
        exchanges: draft.exchanges,
        tokens: draft.tokens,
        wallMs: draft.firstAt !== null && draft.lastAt !== null
            ? draft.lastAt - draft.firstAt
            : null,
        // A run with no `run_finished` may still be going, or the worker may
        // have died. Both look the same from here, so neither is claimed.
        open: draft.open,
        clock: draft.clock,
    }));
}
