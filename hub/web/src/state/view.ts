/**
 * @file one session's client-side view: the transcript and what is derived
 * from it.
 *
 * This is the port of `web/js/state.js`'s per-session half, with two changes
 * that are the point of the rewrite rather than incidental:
 *
 * 1. **Every fold returns a new view.** The old store mutated a plain object
 *    and told listeners "something changed", which is why the consuming code
 *    had to re-render by hand. Here `indexEnvelope` and friends are pure, so a
 *    component can subscribe to the slice it draws and re-render when — and
 *    only when — that slice is replaced.
 *
 * 2. **An envelope is identified by its epoch as well as its sequence.** The
 *    hub's `hub_sequence` restarts at 1 with each hub process, so a transcript
 *    that spans a restart contains two envelopes numbered 1. The old store
 *    compared bare sequence numbers, which meant a post-restart replay was
 *    either dropped as a duplicate or spliced into the middle of the old
 *    numbering. The view therefore records the epoch its cursor belongs to.
 */
import { parseTokenUsage, type TokenUsage } from './tokenUsage.ts';
import type {
    ConfirmationPrompt,
    ContentPart,
    HistoryTurn,
    RequestRecord,
    SessionId,
    TranscriptEpoch,
    WorkerEnvelope,
} from '../../../shared/protocol.ts';
import { parseCompactResult } from './compact.ts';

/** Envelopes retained per session on the client. */
export const TRANSCRIPT_CAP = 2000;
export const TRANSCRIPT_BYTE_CAP = 8 * 1024 * 1024;
/** Recovery notices have their own small budget, independent of event trimming. */
export const TRANSCRIPT_NOTICE_CAP = 8;
export const TRANSCRIPT_NOTICE_BYTE_CAP = 16 * 1024;
export const LATEST_EVENT_CAP = 32;
export const LATEST_EVENT_BYTE_CAP = 4 * 1024 * 1024;
/** These envelopes drive controls and compact/history reconciliation. */
const CONTROL_EVENTS: ReadonlySet<string> = new Set([
    'ready', 'status', 'options', 'compact_finished',
]);
const itemSizes = new WeakMap<object, number>();
const arraySizes = new WeakMap<object, number>();
/** Encoded display size, cached by immutable item/array identity. */
export function displayBytes(items: readonly object[]): number {
    const previous = arraySizes.get(items);
    if (previous !== undefined) return previous;
    let bytes = 0;
    for (const item of items) {
        let size = itemSizes.get(item);
        if (size === undefined) {
            size = new TextEncoder().encode(JSON.stringify(item)).length;
            itemSizes.set(item, size);
        }
        bytes += size;
    }
    arraySizes.set(items, bytes);
    return bytes;
}

/** Worker log lines retained for the log pane. */
export const LOG_CAP = 500;

/** Tracked request outcomes retained per session. */
export const REQUEST_CAP = 200;

/** Event names that open a run. Mirrors the hub's own grouping. */
export const RUN_START_EVENTS: ReadonlySet<string> = new Set(['input_admitted', 'run_started']);

/** Event names that close a run. */
export const RUN_END_EVENTS: ReadonlySet<string> = new Set(['run_finished']);

/** How a note is coloured; `muted` is the default. */
export type NoteTone = 'muted' | 'warn' | 'error';

/** One worker envelope in the transcript. */
export interface EventItem {
    readonly kind: 'event';
    readonly id: string;
    readonly epoch: TranscriptEpoch | null;
    readonly envelope: WorkerEnvelope;
}

/** One tracked request outcome, shown as a chip. */
export interface RequestItem {
    readonly kind: 'request';
    readonly id: string;
    readonly request: RequestRecord;
}

/**
 * Something the panel itself has to say: a replay gap, a hub restart, a
 * locally detected problem. Rendered inline so it cannot be missed.
 */
export interface NoteItem {
    readonly kind: 'note';
    readonly id: string;
    readonly text: string;
    readonly tone: NoteTone;
}

/**
 * A message the operator sent.
 *
 * The worker's `input_admitted` reports the operation, but does not echo the
 * text of an admitted input. So the panel is the only place the
 * operator's own words exist, and this item is where they are kept: written
 * when the message is sent, marked admitted when the worker confirms it, and
 * removed if the hub refuses it (in which case the composer gets the text back
 * rather than losing it, which is the whole of defect D19).
 * A worker rejection retains the text and marks it rejected, so it no longer
 * appears to await admission. It is never automatically resent.
 */
export interface OutboxItem {
    readonly kind: 'outbox';
    readonly id: string;
    readonly requestId: string;
    readonly parts: readonly ContentPart[];
    readonly operation: string;
    readonly state: 'pending' | 'admitted' | 'rejected';
    readonly admittedSequence?: number;
    readonly admittedWorker?: string;
}

/** One line in the transcript. */
export type TranscriptItem = EventItem | RequestItem | NoteItem | OutboxItem;

/** The worker log tail, plus what the hub dropped. */
export interface LogState {
    readonly lines: readonly string[];
    readonly dropped: number;
    readonly logPath: string | null;
}

/** Bounded pages waiting for complete revision/cursor validation before publication. */
export interface HistoryLoad {
    readonly history: readonly HistoryTurn[];
    readonly revision: number;
    readonly worker: string;
    readonly total: number;
    readonly sequence: number;
    readonly next: number;
    readonly nextStep: number;
    readonly truncated: boolean;
}

/** Published history and its bounded, unpublished refresh candidate. */
export interface ViewState {
    readonly id: SessionId;
    /**
     * The hub process the cursor in `lastSeq` belongs to.
     *
     * Null means the hub did not tell us. Cursors still work — a restart is then
     * detected from `latest` moving backwards instead, which is coarser but
     * needs nothing from the hub.
     */
    readonly epoch: TranscriptEpoch | null;
    readonly items: readonly TranscriptItem[];
    /** Bounded replay/eviction warnings that survive transcript trimming and reload. */
    readonly transcriptNotices: readonly NoteItem[];
    /** Display-only history received from the worker, in turn order. */
    readonly history: readonly HistoryTurn[];
    readonly historyLoading: boolean;
    /** Older display history was evicted; canonical worker state is unchanged. */
    readonly historyTruncated: boolean;
    readonly historySequence: number | null;
    readonly historyWorker: string | null;
    readonly historyRevision: number | null;
    readonly historyLoad: HistoryLoad | null;
    /** Highest `hub_sequence` seen *in `epoch`*; also the replay cursor. */
    readonly lastSeq: number;
    /** Eviction requires a full replacement replay, even if a late live frame arrives. */
    readonly replayRequired: boolean;
    /** Changes on eviction so in-flight legacy replies cannot satisfy a later recovery. */
    readonly replayGeneration: number;
    /** request_id -> the `request` item currently in `items`. */
    readonly requestIndex: ReadonlyMap<string, RequestItem>;
    /** request_id -> hub request entry. */
    readonly requests: ReadonlyMap<string, RequestRecord>;
    /** request_ids already represented by an admission or rejection envelope. */
    readonly seenRequests: ReadonlySet<string>;
    /** confirmation_id -> prompt. */
    readonly confirmations: ReadonlyMap<string, ConfirmationPrompt>;
    readonly logs: LogState;
    /** event name -> most recent envelope. */
    readonly latestEvents: Readonly<Record<string, WorkerEnvelope>>;
    readonly runActive: boolean;
    /** A cancel was sent; wait for the worker to finish at a safe boundary. */
    readonly cancelPending: boolean;
    /** Local choices applied only with the next payload. */
    readonly modelSelection: Readonly<Record<string, unknown>>;
    /** Keep the worker catalog even when transcript entries are pruned or replaced. */
    readonly modelCatalog: WorkerEnvelope | null;
    /** Most recent response carrying cost; never an accumulated total. */
    readonly tokenUsage: (TokenUsage & { workerId: string; sequence: number }) | null;
    readonly lastRunId: string;
    readonly gaps: number;
    readonly duplicates: number;
    readonly droppedItems: number;
    /** worker_id -> last worker sequence seen, for gap detection. */
    readonly lastSequenceByWorker: Readonly<Record<string, number>>;
}

/** Fresh per-session view state. */
export function emptyView(id: SessionId): ViewState {
    return {
        id,
        epoch: null,
        items: [],
        transcriptNotices: [],
        history: [],
        historyLoading: false,
        historyTruncated: false,
        historySequence: null,
        historyWorker: null,
        historyRevision: null,
        historyLoad: null,
        lastSeq: 0,
        replayRequired: false,
        replayGeneration: 0,
        requestIndex: new Map(),
        requests: new Map(),
        seenRequests: new Set(),
        confirmations: new Map(),
        logs: { lines: [], dropped: 0, logPath: null },
        latestEvents: {},
        runActive: false,
        cancelPending: false,
        modelSelection: {},
        modelCatalog: null,
        tokenUsage: null,
        lastRunId: '',
        gaps: 0,
        duplicates: 0,
        droppedItems: 0,
        lastSequenceByWorker: {},
    };
}

let itemCounter = 0;

/** A stable identity for a transcript item, used as its React key. */
export function nextItemId(prefix: string): string {
    itemCounter += 1;
    return `${prefix}-${itemCounter}`;
}

/**
 * Fold one envelope into a view's derived caches.
 *
 * Covers the latest event of each name, run activity, the last run id, and
 * worker-sequence gap counting. Applied to live envelopes and to replayed ones
 * alike, so a freshly subscribed panel shows status and options without waiting
 * for new events.
 *
 * The gap counter watches the *worker's* sequence, not the hub's: the hub's
 * ordering only says what this process saw, while a jump in the worker's own
 * numbering is evidence that something was lost in transit.
 */
export function indexEnvelope(view: ViewState, envelope: WorkerEnvelope): ViewState {
    const name = typeof envelope.event === 'string' ? envelope.event : '';
    // Replay can arrive after a newer history query. Only discard a projection
    // that predates this replacement, not pages fetched after it.
    if (name === 'compact_finished' && parseCompactResult(envelope.data)
        && typeof envelope.sequence === 'number'
        && (view.historyWorker === envelope.worker_id
            && (view.historySequence === null || view.historySequence <= envelope.sequence)
            || view.historyLoad?.worker === envelope.worker_id
                && view.historyLoad.sequence <= envelope.sequence)) {
        view = { ...view, history: [], historyLoading: false, historyTruncated: false,
            historySequence: envelope.sequence, historyRevision: null, historyLoad: null };
    }
    // Keep only a bounded derived cache. Unknown event names remain in the
    // transcript, but cannot grow a second unlimited per-name cache.
    const latestEvents: Record<string, WorkerEnvelope> = { ...view.latestEvents };
    if (name) {
        // Refresh insertion order on updates; it now represents recency.
        delete latestEvents[name];
        latestEvents[name] = envelope;
        const names = Object.keys(latestEvents);
        while (names.length > LATEST_EVENT_CAP
            || displayBytes(Object.values(latestEvents)) > LATEST_EVENT_BYTE_CAP) {
            // Incidental output must not evict controls. If control envelopes
            // alone exceed the hard budget, drop their least recent entry too.
            const index = names.findIndex(key => !CONTROL_EVENTS.has(key));
            const [removed] = names.splice(index < 0 ? 0 : index, 1);
            if (!removed) break;
            delete latestEvents[removed];
        }
    }

    let lastRunId = view.lastRunId;
    if (typeof envelope.run_id === 'string' && envelope.run_id.length > 0) {
        lastRunId = envelope.run_id;
    }

    let runActive = view.runActive;
    if (RUN_START_EVENTS.has(name)) {
        runActive = true;
    } else if (RUN_END_EVENTS.has(name)) {
        runActive = false;
    } else if (name === 'status' || name === 'ready') {
        const data = (envelope.data ?? {}) as Record<string, unknown>;
        if (typeof data.active === 'boolean') runActive = data.active;
        const loop = data.loop as Record<string, unknown> | undefined;
        if (loop && typeof loop.status === 'string') {
            runActive = loop.status === 'running' || data.active === true;
        }
    }

    const cost = name === 'model_response'
        ? parseTokenUsage((envelope.data as { cost?: unknown } | null)?.cost) : null;
    const usageSequence = Number(envelope.sequence);
    const tokenUsage = cost && (view.tokenUsage?.workerId !== envelope.worker_id
        || usageSequence > view.tokenUsage.sequence)
        ? { ...cost, workerId: envelope.worker_id, sequence: usageSequence }
        : view.tokenUsage;
    const cancelPending = view.cancelPending && runActive
        && !(RUN_START_EVENTS.has(name) && lastRunId !== view.lastRunId);
    const modelCatalog = name === 'options'
        && (view.modelCatalog?.worker_id !== envelope.worker_id
            || Number(envelope.sequence) > Number(view.modelCatalog?.sequence ?? -1))
        ? envelope : view.modelCatalog;
    const modelSelection = modelCatalog !== view.modelCatalog ? {} : view.modelSelection;
    return noteWorkerSequence({
        ...view, latestEvents, lastRunId, runActive, cancelPending, modelSelection, modelCatalog, tokenUsage,
    }, envelope);
}

/** Account for a received worker envelope without retaining it as transcript. */
export function noteWorkerSequence(view: ViewState, envelope: WorkerEnvelope): ViewState {
    const workerId = typeof envelope.worker_id === 'string' ? envelope.worker_id : '';
    const sequence = envelope.sequence;
    if (typeof sequence !== 'number' || !Number.isSafeInteger(sequence)) return view;
    const previous = view.lastSequenceByWorker[workerId];
    if (typeof previous === 'number' && sequence <= previous) return view;
    return {
        ...view,
        lastSequenceByWorker: { ...view.lastSequenceByWorker, [workerId]: sequence },
        gaps: view.gaps + (typeof previous === 'number' && sequence > previous + 1 ? 1 : 0),
    };
}

/** Trim a view's transcript, keeping the request index consistent. */
export function trim(view: ViewState): ViewState {
    let bytes = displayBytes(view.items);
    let overflow = Math.max(0, view.items.length - TRANSCRIPT_CAP);
    for (let index = 0; index < overflow; ++index) bytes -= displayBytes([view.items[index]!]);
    while (bytes > TRANSCRIPT_BYTE_CAP && overflow < view.items.length) {
        bytes -= displayBytes([view.items[overflow]!]);
        overflow += 1;
    }
    if (overflow === 0) return view;
    const removed = view.items.slice(0, overflow);
    const items = view.items.slice(overflow);
    // Copied only if something removed actually had an index entry, which is
    // rare: the request chips are a small minority of a long transcript.
    let requestIndex: Map<string, RequestItem> | null = null;
    for (const item of removed) {
        if (item.kind !== 'request') continue;
        const current = (requestIndex ?? view.requestIndex).get(item.request.request_id);
        if (current !== item) continue;
        requestIndex ??= new Map(view.requestIndex);
        requestIndex.delete(item.request.request_id);
    }
    return {
        ...view,
        items,
        droppedItems: view.droppedItems + removed.length,
        ...(requestIndex ? { requestIndex } : {}),
    };
}

/** Append items to a view and trim. */
export function append(view: ViewState, ...added: readonly TranscriptItem[]): ViewState {
    if (added.length === 0) return view;
    return trim({ ...view, items: [...view.items, ...added] });
}

/** Keep recovery warnings visible above the transcript, within a separate hard budget. */
export function addTranscriptNotice(view: ViewState, item: NoteItem): ViewState {
    if (view.transcriptNotices.some(previous => previous.text === item.text && previous.tone === item.tone)) {
        return view;
    }
    let transcriptNotices = [...view.transcriptNotices, item].slice(-TRANSCRIPT_NOTICE_CAP);
    while (displayBytes(transcriptNotices) > TRANSCRIPT_NOTICE_BYTE_CAP) {
        transcriptNotices = transcriptNotices.slice(1);
    }
    return { ...view, transcriptNotices };
}

/** Control caches retained when inactive transcript/history copies are evicted. */
export function controlEvents(view: ViewState): Readonly<Record<string, WorkerEnvelope>> {
    return Object.fromEntries(Object.entries(view.latestEvents)
        .filter(([name]) => CONTROL_EVENTS.has(name)));
}

/** Display copies counted against the aggregate panel budget. */
export function viewDisplayBytes(view: ViewState): number {
    return displayBytes(view.items) + displayBytes(view.history)
        + displayBytes(view.historyLoad?.history ?? [])
        + displayBytes(view.transcriptNotices) + displayBytes(Object.values(view.latestEvents));
}

/** A note about the transcript itself, not about the conversation. */
export function noteItem(text: string, tone: NoteTone = 'muted'): NoteItem {
    return { kind: 'note', id: nextItemId('note'), text, tone };
}

/** Read the hub_sequence of an envelope, when it has a usable one. */
export function hubSequenceOf(envelope: WorkerEnvelope): number | null {
    return typeof envelope.hub_sequence === 'number' ? envelope.hub_sequence : null;
}

/** Counters for one view, for a status line or a session row. */
export interface ViewStats {
    readonly items: number;
    readonly lastSeq: number;
    readonly gaps: number;
    readonly duplicates: number;
    readonly droppedItems: number;
    readonly confirmations: number;
    readonly unknownRequests: number;
}

/**
 * Summarise a view.
 *
 * A pure function of the view rather than a store method, because a React
 * component must memoise it against the view identity: a selector that built a
 * fresh object on every call would re-render forever.
 */
export function statsOf(view: ViewState | undefined): ViewStats {
    if (!view) {
        return {
            items: 0, lastSeq: 0, gaps: 0, duplicates: 0,
            droppedItems: 0, confirmations: 0, unknownRequests: 0,
        };
    }
    let unknownRequests = 0;
    for (const request of view.requests.values()) {
        if (request.state === 'unknown') unknownRequests += 1;
    }
    return {
        items: view.items.length,
        lastSeq: view.lastSeq,
        gaps: view.gaps,
        duplicates: view.duplicates,
        droppedItems: view.droppedItems,
        confirmations: view.confirmations.size,
        unknownRequests,
    };
}
