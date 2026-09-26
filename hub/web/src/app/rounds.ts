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
import type { ConfirmationPrompt, WorkerEnvelope } from '../../../shared/protocol.ts';
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
}

/** Something the worker or the panel reported as a problem. */
export interface Problem {
    readonly key: string;
    readonly label: string;
    readonly text: string;
    readonly tone: 'warn' | 'error';
}

/**
 * The order things happened in, within one round.
 *
 * The round groups its contents by kind so the renderer can draw them well, and
 * that grouping is what a reader wants — except when technical details are on,
 * where the point is to see the protocol in the order it arrived. This is the
 * order, kept alongside the grouping rather than instead of it.
 */
export type TimelineEntry =
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
    readonly assistant: readonly AssistantBlock[];
    readonly calls: readonly ToolCall[];
    readonly problems: readonly Problem[];
    /** What happened, in order. */
    readonly timeline: readonly TimelineEntry[];
    /** Bookkeeping events, shown only when technical details are on. */
    readonly protocol: readonly EventItem[];
    readonly notes: readonly NoteItem[];
    readonly requests: readonly RequestItem[];
    /** `run_finished`'s status, or `''` while the run is still open. */
    readonly status: string;
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
    input: OutboxItem | null;
    admitted: EventItem | null;
    assistant: AssistantBlock[];
    calls: ToolCall[];
    problems: Problem[];
    timeline: TimelineEntry[];
    protocol: EventItem[];
    notes: NoteItem[];
    requests: RequestItem[];
    status: string;
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
        input: null,
        admitted: null,
        assistant: [],
        calls: [],
        problems: [],
        timeline: [],
        protocol: [],
        notes: [],
        requests: [],
        status: '',
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
): ToolCall {
    const output = parseToolOutput(result.text);
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

/** Build the rounds of a transcript. */
export function buildRounds(
    items: readonly TranscriptItem[],
    confirmations: ReadonlyMap<string, ConfirmationPrompt>,
): Round[] {
    const byId = new Map<string, ConfirmationPrompt>();
    const byName = new Map<string, ConfirmationPrompt>();
    const prompts: Prompts = { byId, byName };
    for (const prompt of confirmations.values()) {
        if (prompt.settled_at !== null) continue;
        if (prompt.call?.id) byId.set(prompt.call.id, prompt);
        if (prompt.call?.name) byName.set(prompt.call.name, prompt);
    }

    const closed: Draft[] = [];
    let runCount = 0;
    let current = newDraft('prelude', 0, 'prelude');

    const hasContent = (draft: Draft): boolean => draft.input !== null
        || draft.admitted !== null
        || draft.assistant.length > 0
        || draft.calls.length > 0
        || draft.protocol.length > 0
        || draft.problems.length > 0
        || draft.notes.length > 0
        || draft.requests.length > 0;

    /** Finish the draft in hand, keeping it only if it holds anything. */
    const flush = (): void => {
        if (hasContent(current)) closed.push(current);
    };

    /** Begin a new run, closing whatever came before it. */
    const startRun = (): Draft => {
        flush();
        runCount += 1;
        current = newDraft(`run-${runCount}`, runCount, 'run');
        return current;
    };

    /** The run in hand, starting one if the prelude is what came before. */
    const ensureRun = (): Draft => (current.kind === 'run' ? current : startRun());

    for (const item of items) {
        if (item.kind === 'outbox') {
            // The panel's own message arrives before the wire reports its
            // admission, so an outbox item opens the turn.
            if (current.kind === 'run' && (current.input || current.admitted)) startRun();
            else ensureRun();
            current.input = item;
            continue;
        }

        if (item.kind === 'note') {
            current.notes.push(item);
            current.timeline.push({ kind: 'note', key: item.id });
            continue;
        }

        if (item.kind === 'request') {
            current.requests.push(item);
            continue;
        }

        const { envelope } = item;
        const name = str(envelope.event);

        if (name === 'input_admitted') {
            // A second admission while a run already holds one is a new turn.
            if (current.kind !== 'run' || current.admitted !== null) startRun();
            if (current.input === null) current.admitted = item;
            current.open = true;
            track(current, envelope);
            current.protocol.push(item);
            current.timeline.push({ kind: 'protocol', key: item.id });
            continue;
        }

        if (name === 'run_started') {
            // Follows its admission inside the same turn, so this continues the
            // run in hand rather than opening another.
            const run = ensureRun();
            run.open = true;
            track(run, envelope);
            run.protocol.push(item);
            run.timeline.push({ kind: 'protocol', key: item.id });
            continue;
        }

        if (name === 'run_finished') {
            const summary = obj(envelope.data) ?? {};
            const run = ensureRun();
            run.status = str(summary.status) || 'finished';
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
            // Whatever follows belongs to the next turn, not to this one, so
            // the draft is closed here rather than at the next opener.
            flush();
            current = newDraft('between', 0, 'prelude');
            continue;
        }

        if (PROTOCOL_EVENTS.has(name)) {
            track(current, envelope);
            current.protocol.push(item);
            current.timeline.push({ kind: 'protocol', key: item.id });
            continue;
        }

        if (name === 'model_response') {
            const run = ensureRun();
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
            const run = ensureRun();
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
            const run = ensureRun();
            track(run, envelope);
            for (const result of resultsOf(envelope)) {
                const call = matchCall(run, result);
                if (!call) {
                    // A result whose proposal is not in this transcript: a
                    // replay can begin mid-turn. Shown rather than dropped.
                    const output = parseToolOutput(result.text);
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
                run.calls[at] = settle(run, call, result, envelope);
                run.settled.add(call.key);
            }
            continue;
        }

        if (PROBLEM_EVENTS.has(name)) {
            const run = ensureRun();
            track(run, envelope);
            const data = obj(envelope.data) ?? {};
            run.problems.push({
                key: item.id,
                label: name === 'input_rejected'
                    ? 'input rejected'
                    : name === 'error' ? 'worker diagnostic' : 'markdown export failed',
                text: str(data.message) || 'the worker reported a problem',
                tone: name === 'error' ? 'error' : 'warn',
            });
            run.timeline.push({ kind: 'problem', key: item.id });
            continue;
        }

        // An event name this build has never heard of: core is allowed to add
        // them, and a panel that dropped one would lose part of the turn.
        const run = ensureRun();
        track(run, envelope);
        run.problems.push({
            key: item.id,
            label: name || '(unnamed event)',
            text: 'this panel has no renderer for that event yet',
            tone: 'warn',
        });
        run.timeline.push({ kind: 'problem', key: item.id });
    }

    flush();
    return closed.map((draft) => ({
        key: draft.key,
        index: draft.index,
        kind: draft.kind,
        input: draft.input,
        admitted: draft.admitted,
        assistant: draft.assistant,
        calls: draft.calls,
        problems: draft.problems,
        timeline: draft.timeline,
        protocol: draft.protocol,
        notes: draft.notes,
        requests: draft.requests,
        status: draft.status,
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
