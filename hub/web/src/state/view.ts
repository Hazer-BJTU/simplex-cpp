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
import type {
    ConfirmationPrompt,
    ContentPart,
    RequestRecord,
    SessionId,
    TranscriptEpoch,
    WorkerEnvelope,
} from '../../../shared/protocol.ts';

/** Envelopes retained per session on the client. */
export const TRANSCRIPT_CAP = 2000;

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
 * The worker protocol reports `input_admitted` with an empty payload — the text
 * of an admitted input is never echoed back. So the panel is the only place the
 * operator's own words exist, and this item is where they are kept: written
 * when the message is sent, marked admitted when the worker confirms it, and
 * removed if the hub refuses it (in which case the composer gets the text back
 * rather than losing it, which is the whole of defect D19).
 */
export interface OutboxItem {
    readonly kind: 'outbox';
    readonly id: string;
    readonly requestId: string;
    readonly parts: readonly ContentPart[];
    readonly operation: string;
    readonly state: 'pending' | 'admitted';
}

/** One line in the transcript. */
export type TranscriptItem = EventItem | RequestItem | NoteItem | OutboxItem;

/** The worker log tail, plus what the hub dropped. */
export interface LogState {
    readonly lines: readonly string[];
    readonly dropped: number;
    readonly logPath: string | null;
}

/** Everything the panel knows about one session. */
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
    /** Highest `hub_sequence` seen *in `epoch`*; also the replay cursor. */
    readonly lastSeq: number;
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
        lastSeq: 0,
        requestIndex: new Map(),
        requests: new Map(),
        seenRequests: new Set(),
        confirmations: new Map(),
        logs: { lines: [], dropped: 0, logPath: null },
        latestEvents: {},
        runActive: false,
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
    const latestEvents = name
        ? { ...view.latestEvents, [name]: envelope }
        : view.latestEvents;

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

    let lastSequenceByWorker = view.lastSequenceByWorker;
    let gaps = view.gaps;
    const workerId = typeof envelope.worker_id === 'string' ? envelope.worker_id : '';
    if (typeof envelope.sequence === 'number') {
        const previous = lastSequenceByWorker[workerId];
        if (typeof previous === 'number' && envelope.sequence !== previous + 1) gaps += 1;
        lastSequenceByWorker = { ...lastSequenceByWorker, [workerId]: envelope.sequence };
    }

    return { ...view, latestEvents, lastRunId, runActive, lastSequenceByWorker, gaps };
}

/** Trim a view's transcript, keeping the request index consistent. */
export function trim(view: ViewState): ViewState {
    if (view.items.length <= TRANSCRIPT_CAP) return view;
    const overflow = view.items.length - TRANSCRIPT_CAP;
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

/** Prepend items to a view (used by the replay-gap note, which belongs first). */
export function prepend(view: ViewState, item: TranscriptItem): ViewState {
    return { ...view, items: [item, ...view.items] };
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
