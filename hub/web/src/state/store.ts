/**
 * @file the panel's store.
 *
 * Ported from `web/js/state.js` — same shape (one entry per session, one view
 * per session, bounded retention), different consumer: a Zustand store instead
 * of a listener set, so a component subscribes to the slice it draws and
 * re-renders when that slice is replaced rather than whenever anything changes.
 *
 * It is the *vanilla* store (`zustand/vanilla`), not the React-bound one, and
 * that is deliberate: the React binding lives in `usePanel.ts`, which means
 * this file imports no React and can be exercised by `node --test`. The four
 * behaviours below are all defects the old panel had, and each has a regression
 * test that runs without a browser.
 *
 * - **A replay is merged, not substituted** (defect A2). `subscribed` carries
 *   the envelopes *after* the cursor the panel asked from, not the whole
 *   transcript, so replacing the list with it erased everything older every
 *   time the socket reconnected.
 * - **A session list never deletes** (defect D23). `welcome` is the one
 *   authoritative list, because it is sent once per connection and nothing can
 *   have raced it; a list answering a refresh is merged, so the WebSocket reply
 *   and the REST reply can no longer overwrite each other.
 * - **The transcript epoch is tracked** (the second, quieter root cause of A2).
 *   Cursors belong to one hub process, and a cursor from before a restart
 *   silently returns nothing. When the epoch changes — or when `latest` moves
 *   backwards, which is how a hub too old to report an epoch gives itself away
 *   — the cursor resets and the panel asks again from the beginning, with a
 *   note in the transcript saying why.
 * - **The log pane is a tail, not an append log** (defect D20). The hub's
 *   `logs` message is `supervisor.logs()`: the last N lines of a ring buffer.
 *   Concatenating it onto what the panel already held doubled the pane on every
 *   refresh.
 *
 * The store owns no socket and no DOM: `lib/client.ts` feeds it. The rule that
 * `sent` is not `executed` is kept — a request is ever only reported as
 * admitted, rejected, or unknown, never inferred from a successful `send`.
 */
import { createStore } from 'zustand/vanilla';
import type {
    Capability,
    ConfirmationPrompt,
    ContentPart,
    HistoryPage,
    HubMessage,
    HubMetadata,
    RequestRecord,
    SessionDescription,
    SessionId,
    SessionIdentity,
    SnapshotView,
    TranscriptEpoch,
    WorkerEnvelope,
} from '../../../shared/protocol.ts';
import type { PanelSocketStatus } from '../lib/socket.ts';
import {
    LOG_CAP,
    REQUEST_CAP,
    append,
    emptyView,
    hubSequenceOf,
    indexEnvelope,
    nextItemId,
    noteItem,
    prepend,
    statsOf,
    trim,
    type LogState,
    type NoteTone,
    type OutboxItem,
    type TranscriptItem,
    type ViewState,
    type ViewStats,
} from './view.ts';

type WelcomeMessage = Extract<HubMessage, { type: 'welcome' }>;
type SubscribedMessage = Extract<HubMessage, { type: 'subscribed' }>;
type EventMessage = Extract<HubMessage, { type: 'event' }>;
type ConfirmationMessage = Extract<HubMessage, { type: 'confirmation' }>;
type ProcessMessage = Extract<HubMessage, { type: 'process' }>;
type ConnectionMessage = Extract<HubMessage, { type: 'connection' }>;
type RequestMessage = Extract<HubMessage, { type: 'request' }>;
type LogsMessage = Extract<HubMessage, { type: 'logs' }>;
type SnapshotMessage = Extract<HubMessage, { type: 'snapshot' }>;
type ErrorMessage = Extract<HubMessage, { type: 'error' }>;
type AcceptedMessage = Extract<HubMessage, { type: 'accepted' }>;

/** A note tone, as the transcript renders it. */
export type { NoteTone };

/**
 * Something the panel owes the operator an explanation for: a refusal from the
 * hub, or a supervisor action that came back `ok: false`.
 *
 * One slot rather than a queue. The old panel's toasts were a queue with no
 * expiry, so a burst of failures stacked up and hid the transcript; a single
 * line that the next action replaces is enough to notice, and nothing is lost
 * because every refusal is also visible where it happened (a failed input goes
 * back to the composer, a failed signal is in the transcript's request chips).
 */
export interface Notice {
    readonly tone: 'error' | 'warn' | 'info';
    /** The hub's error code, or the action name for a refused worker op. */
    readonly code: string;
    readonly text: string;
    /** Monotonic, so a component can tell two identical notices apart. */
    readonly at: number;
}

/**
 * A message the panel sent that the hub refused.
 *
 * Kept so the composer can put the text back rather than losing it, which is
 * the whole of defect D19.
 */
export interface FailedInput {
    readonly sessionId: SessionId;
    readonly parts: readonly ContentPart[];
    readonly operation: string;
    readonly reason: string;
    /** Monotonic, so a composer can tell two identical failures apart. */
    readonly at: number;
}

/** What the socket is doing, as the UI needs to describe it. */
export interface ConnectionState {
    readonly state: PanelSocketStatus['state'];
    readonly attempt: number;
    readonly nextDelayMs: number | null;
    readonly welcomeReceived: boolean;
    readonly error: string | null;
    /** A frame this panel could not use at all. */
    readonly refusal: { readonly code: string; readonly detail: string } | null;
    /** Frames whose type this protocol version does not define. */
    readonly ignoredFrames: number;
}

/**
 * What the caller should do after folding in a message.
 *
 * Only one path needs this: a `subscribed` reply numbered against a hub process
 * the panel's cursor does not belong to. The delta in that reply is unusable,
 * and the honest response is to ask again from the beginning rather than to
 * guess at an offset.
 */
export interface ApplyEffects {
    readonly resubscribe?: { readonly session: SessionId; readonly since: number };
}

const NO_EFFECTS: ApplyEffects = {};

/**
 * How the worker should answer tool confirmations for one session.
 *
 * `approve` means "approve every call that would have asked" — it is the one
 * setting here that grants something, which is why it is stored per session
 * rather than read from a control that outlives the session it was set for.
 * The old panel kept it in a single DOM `<select>` that was never reset, so
 * choosing `approve` in one session silently applied it to every session
 * opened afterwards (defect D15).
 */
export type ConfirmMode = 'ask' | 'approve' | 'deny';

/** What the inspector drawer is showing. */
export type InspectorTab = 'run' | 'process' | 'logs' | 'snapshot';

/** A snapshot fetched over REST, kept with the session it belongs to. */
export interface SnapshotState {
    readonly sessionId: SessionId;
    readonly loading: boolean;
    readonly view: SnapshotView | null;
    readonly error: string | null;
}

/** The state held by the store. */
export interface PanelState {
    hub: HubMetadata | null;
    /** The hub process every cursor in `views` belongs to. */
    epoch: TranscriptEpoch | null;
    capabilities: ReadonlySet<Capability>;
    connection: ConnectionState;
    /** True once the hub has answered 401, so the token prompt can be shown. */
    authRequired: boolean;
    sessions: ReadonlyMap<SessionId, SessionDescription>;
    views: ReadonlyMap<SessionId, ViewState>;
    selected: SessionId | null;
    failedInput: FailedInput | null;
    /** The last refusal the operator has not dismissed. */
    notice: Notice | null;
    /** True when protocol events are rendered alongside the conversation. */
    showDetails: boolean;
    /**
     * Per-session confirmation mode.
     *
     * Absent means `ask`, which is the worker's own default. Storing the
     * absence rather than a value is what keeps a session that was never
     * configured from looking like one that was.
     */
    confirmMode: ReadonlyMap<SessionId, ConfirmMode>;
    /** Whether the context drawer is open, and what it is showing. */
    inspectorOpen: boolean;
    inspectorTab: InspectorTab;
    /** The worker's persisted snapshot, or the attempt to read it. */
    snapshot: SnapshotState | null;
    /** Whether the command palette is open. */
    paletteOpen: boolean;
    /** When the outstanding heartbeat was sent, or null. Not read by the UI. */
    pingSentAt: number | null;
    /**
     * Round trip of the last heartbeat, in milliseconds.
     *
     * `ping` was the one message the old panel could receive and never sent, so
     * the hub's `pong` was dead protocol surface. It is also the only way to
     * measure the link rather than guess at it from a socket state that a
     * browser reports as "open" long after the other end has stopped answering.
     */
    pingMs: number | null;
}

/** Everything the store can be asked to do. */
export interface PanelActions {
    // -------------------------------------------------------------- reads --
    /** Sessions ordered for display: newest first. */
    sessionList(): SessionDescription[];
    /** The session description, or null when the hub does not list it. */
    session(sessionId: SessionId): SessionDescription | null;
    view(sessionId: SessionId): ViewState;
    items(sessionId: SessionId): readonly TranscriptItem[];
    lastSeq(sessionId: SessionId): number;
    logs(sessionId: SessionId): LogState;
    latestEvent(sessionId: SessionId, name: string): WorkerEnvelope | null;
    latestData(sessionId: SessionId, name: string): unknown;
    openConfirmations(sessionId: SessionId): ConfirmationPrompt[];
    confirmation(sessionId: SessionId, confirmationId: string): ConfirmationPrompt | null;
    requests(sessionId: SessionId): RequestRecord[];
    unknownRequests(sessionId: SessionId): RequestRecord[];
    isRunActive(sessionId: SessionId): boolean;
    lastRunId(sessionId: SessionId): string;
    loop(sessionId: SessionId): Record<string, unknown> | null;
    statusData(sessionId: SessionId): unknown;
    model(sessionId: SessionId): string;
    hasCapability(capability: Capability): boolean;

    // ------------------------------------------------------------- writes --
    setHub(metadata: HubMetadata | null): void;
    setSelected(sessionId: SessionId | null): void;
    setAuthRequired(required: boolean): void;
    setConnection(status: PanelSocketStatus): void;
    /** Record a frame the panel could not use. */
    noteRefusal(code: string, detail: string): void;
    /** Record a frame whose type this protocol version does not define. */
    noteIgnoredFrame(): void;
    /** Forget the last refusal, once the UI has shown it. */
    clearRefusal(): void;

    /** Merge a session list: add and update, never delete (defect D23). */
    upsertSessions(list: readonly SessionDescription[]): void;
    upsertSession(session: SessionDescription): void;
    /** The hub's own full list, authoritative because nothing raced it. */
    applyWelcome(message: WelcomeMessage): void;
    removeSession(sessionId: SessionId): void;

    applySubscribed(message: SubscribedMessage): ApplyEffects;
    applyEvent(message: EventMessage): void;
    beginHistory(sessionId: SessionId): void;
    endHistory(sessionId: SessionId): void;
    /** Commit one already validated page; false means it did not fit the current load. */
    applyHistoryPage(sessionId: SessionId, envelope: WorkerEnvelope, page: HistoryPage): boolean;
    applyRequest(message: RequestMessage): void;
    applyConfirmation(message: ConfirmationMessage): void;
    applyProcess(message: ProcessMessage): void;
    applyConnection(message: ConnectionMessage): void;
    applyLogs(message: LogsMessage): void;
    /** A full transcript handed over by the hub; replaces what is held. */
    applySnapshot(message: SnapshotMessage): void;
    /**
     * Merge a transcript read over HTTP.
     *
     * The recovery path when the socket is down: `GET /api/sessions/:id/events`
     * answers the same question as a re-subscribe, but it is a request rather
     * than a message, so it arrives without a session description or a cursor.
     * Merging is the right shape here for the same reason it is right for a
     * re-subscribe — anything already held is kept, anything new is appended.
     */
    mergeTranscript(sessionId: SessionId, transcript: readonly WorkerEnvelope[], latest: number): void;
    /** A refusal from the hub, matched to the message it answers. */
    applyError(message: ErrorMessage): void;
    /** An accepted command; a refused worker action is reported here. */
    applyAccepted(message: AcceptedMessage): void;

    /** Report something the operator needs to know about. */
    setNotice(tone: Notice['tone'], code: string, text: string): void;
    dismissNotice(): void;

    /**
     * Show the protocol events mixed in with the conversation.
     *
     * Off by default: `persisted`, `input_committed` and `run_started` are
     * real, but they are the machinery rather than the conversation, and the
     * old panel gave each one a card of the same weight as a model response.
     * The switch is one flag on the store rather than one per round, because
     * "show me what the protocol did" is a mood, not a property of a turn.
     */
    setShowDetails(show: boolean): void;
    toggleDetails(): void;

    // ------------------------------------------------------------- session --
    /** The mode in effect for a session; `ask` when none was chosen. */
    confirmModeOf(sessionId: SessionId): ConfirmMode;
    setConfirmMode(sessionId: SessionId, mode: ConfirmMode): void;

    setInspectorOpen(open: boolean): void;
    setInspectorTab(tab: InspectorTab): void;
    /** Record a snapshot fetch started for a session. */
    beginSnapshot(sessionId: SessionId): void;
    /**
     * Record a snapshot result — but only if it is still the session in hand.
     *
     * The old panel's snapshot pane awaited and then rendered whatever came
     * back, so switching sessions mid-request painted one session's state under
     * another's header (defect D25).
     */
    finishSnapshot(sessionId: SessionId, view: SnapshotView | null, error: string | null): void;

    setPaletteOpen(open: boolean): void;
    togglePalette(): void;

    /** A heartbeat was sent at `at`; the reply has not arrived yet. */
    beginPing(at: number): void;
    /** A `pong` arrived at `at` for a heartbeat sent at `sentAt`. */
    finishPing(sentAt: number, at: number): void;

    /** Append a note to a transcript (replay gaps, local warnings). */
    note(sessionId: SessionId, text: string, tone?: NoteTone): void;

    // -------------------------------------------------------------- input --
    /** Record an input the panel is about to send. */
    beginInput(
        sessionId: SessionId,
        requestId: string,
        parts: readonly ContentPart[],
        operation: string,
    ): void;
    /** The hub refused it: drop the pending message and hand the text back. */
    failInput(sessionId: SessionId, requestId: string, reason: string): void;
    /** A composer has taken the failed text; stop offering it. */
    clearFailedInput(): void;
}

/** The whole store: state plus actions. */
export type PanelStore = PanelState & PanelActions;

/** A vanilla Zustand store handle. */
export type PanelStoreApi = ReturnType<typeof createPanelStore>;

const INITIAL: PanelState = {
    hub: null,
    epoch: null,
    capabilities: new Set(),
    connection: {
        state: 'idle',
        attempt: 0,
        nextDelayMs: null,
        welcomeReceived: false,
        error: null,
        refusal: null,
        ignoredFrames: 0,
    },
    authRequired: false,
    sessions: new Map(),
    views: new Map(),
    selected: null,
    failedInput: null,
    notice: null,
    showDetails: false,
    confirmMode: new Map(),
    inspectorOpen: false,
    inspectorTab: 'run',
    snapshot: null,
    paletteOpen: false,
    pingSentAt: null,
    pingMs: null,
};

/** Read a view, creating an empty one without storing it. */
function viewOf(state: PanelState, sessionId: SessionId): ViewState {
    return state.views.get(sessionId) ?? emptyView(sessionId);
}

/** Replace one session's view. Returns an empty patch when nothing changed. */
function withView(
    state: PanelState,
    sessionId: SessionId,
    fold: (view: ViewState) => ViewState,
): Partial<PanelState> {
    const current = viewOf(state, sessionId);
    const next = fold(current);
    if (next === current && state.views.has(sessionId)) return {};
    const views = new Map(state.views);
    views.set(sessionId, next);
    return { views };
}

/** A session map with one entry replaced, without mutating the original. */
function upserted(
    state: PanelState,
    session: SessionDescription | undefined,
): ReadonlyMap<SessionId, SessionDescription> {
    const sessions = new Map(state.sessions);
    if (session && typeof session.session_id === 'string') {
        sessions.set(session.session_id, session);
    }
    return sessions;
}

/** Record that a request is already represented by an admission envelope. */
function markSeen(view: ViewState, requestId: string): ViewState {
    if (view.seenRequests.has(requestId)) return view;
    const seenRequests = new Set(view.seenRequests);
    seenRequests.add(requestId);
    return { ...view, seenRequests };
}

/** Mark a pending message admitted, keeping its text on screen. */
function admitInput(view: ViewState, requestId: string, envelope: WorkerEnvelope): ViewState {
    let changed = false;
    const items = view.items.map((item) => {
        if (item.kind !== 'outbox' || item.requestId !== requestId) return item;
        if (item.state === 'admitted') return item;
        changed = true;
        return { ...item, state: 'admitted',
            admittedSequence: typeof envelope.sequence === 'number'
                ? envelope.sequence : undefined,
            admittedWorker: envelope.worker_id } as OutboxItem;
    });
    const marked = markSeen(view, requestId);
    return changed ? { ...marked, items } : marked;
}

/** Drop a pending message the hub refused. */
function dropOutbox(view: ViewState, requestId: string): ViewState {
    const items = view.items.filter(
        (item) => !(item.kind === 'outbox' && item.requestId === requestId),
    );
    return items.length === view.items.length ? view : { ...view, items };
}

/**
 * The open prompts a session description lists.
 *
 * A description is a snapshot of what is open right now, so it replaces the
 * map rather than merging into it: a prompt that was decided while the panel
 * was disconnected must not survive as a button that can no longer do anything.
 */
function seedConfirmations(
    view: ViewState,
    prompts: readonly ConfirmationPrompt[] | undefined,
): ReadonlyMap<string, ConfirmationPrompt> {
    const confirmations = new Map<string, ConfirmationPrompt>();
    for (const prompt of prompts ?? []) {
        if (prompt?.confirmation_id) confirmations.set(prompt.confirmation_id, prompt);
    }
    if (confirmations.size === view.confirmations.size) {
        let same = true;
        for (const [id, prompt] of confirmations) {
            if (view.confirmations.get(id)?.settled_at !== prompt.settled_at) {
                same = false;
                break;
            }
        }
        if (same) return view.confirmations;
    }
    return confirmations;
}

/** The item an envelope becomes in the transcript. */
function eventItem(view: ViewState, envelope: WorkerEnvelope): TranscriptItem {
    return { kind: 'event', id: nextItemId('ev'), epoch: view.epoch, envelope };
}

/** Bookkeeping shared by the replay and live paths. */
function foldEnvelope(view: ViewState, envelope: WorkerEnvelope): ViewState {
    if (envelope.event === 'history') {
        // History is a transient query reply, never an event card or a cached
        // latest event containing megabytes of display data.
        const worker = envelope.worker_id;
        const sequence = envelope.sequence;
        return typeof sequence === 'number'
            ? { ...view, lastSequenceByWorker: {
                ...view.lastSequenceByWorker, [worker]: sequence,
            } }
            : view;
    }
    let next = indexEnvelope(view, envelope);
    if (envelope.event === 'input_admitted' && typeof envelope.request_id === 'string') {
        const requestId = envelope.request_id;
        const mine = next.items.some(
            (item) => item.kind === 'outbox' && item.requestId === requestId,
        );
        next = admitInput(next, requestId, envelope);
        // The panel's own message already stands for this input, and it is the
        // only place the text exists — `input_admitted` carries an empty
        // payload. A second row repeating that is noise. The placeholder is for
        // history this page never sent: after a reload there is no outbox item,
        // and an admitted input with no visible text would be a hole in the
        // conversation.
        if (mine) return next;
    }
    next = append(next, eventItem(next, envelope));
    if (envelope.event === 'input_rejected') {
        const rejected = (envelope.data as Record<string, unknown> | null)?.request_id;
        if (typeof rejected === 'string') next = markSeen(next, rejected);
    }
    return next;
}

/**
 * Merge a replayed delta into a view.
 *
 * The delta holds the envelopes after the cursor the panel asked from. Anything
 * at or below the cursor is a duplicate — the panel asked twice, or a live
 * event overtook the replay — and anything above it is new and appended, so the
 * operator keeps reading the history they already had.
 */
function mergeEnvelopes(view: ViewState, transcript: readonly WorkerEnvelope[]): ViewState {
    const previousLast = view.lastSeq;
    let duplicates = 0;
    let firstSeq: number | null = null;
    let next = view;

    for (const envelope of transcript) {
        if (!envelope || typeof envelope !== 'object') continue;
        const seq = hubSequenceOf(envelope);
        if (seq !== null && previousLast > 0 && seq <= previousLast) {
            duplicates += 1;
            continue;
        }
        if (firstSeq === null && seq !== null) firstSeq = seq;
        next = foldEnvelope(next, envelope);
    }

    if (duplicates > 0) next = { ...next, duplicates: next.duplicates + duplicates };
    if (previousLast > 0 && firstSeq !== null && firstSeq > previousLast + 1) {
        // The hub's ring no longer holds everything the panel asked for.
        next = prepend(next, noteItem(
            `transcript gap: replay resumed at hub_sequence ${firstSeq}`
            + ` (last seen ${previousLast}); earlier envelopes are gone`,
            'warn',
        ));
        next = { ...next, gaps: next.gaps + 1 };
    }
    return next;
}

/** Rebuild request chips from the session description after a replay. */
function seedFromSession(state: PanelState, sessionId: SessionId): Partial<PanelState> {
    const session = state.sessions.get(sessionId);
    if (!session) return {};
    return withView(state, sessionId, (view) => {
        let next = view;
        for (const entry of session.requests ?? []) {
            if (!entry || typeof entry.request_id !== 'string') continue;
            const requests = new Map(next.requests);
            requests.set(entry.request_id, entry);
            next = { ...next, requests };
            if (next.requestIndex.has(entry.request_id)) continue;
            if (next.seenRequests.has(entry.request_id)) continue;
            const item = { kind: 'request', id: nextItemId('req'), request: entry } as const;
            const requestIndex = new Map(next.requestIndex);
            requestIndex.set(entry.request_id, item);
            next = { ...next, items: [...next.items, item], requestIndex };
        }
        if (session.last_run_id) next = { ...next, lastRunId: session.last_run_id };
        return trim(next);
    });
}

/**
 * Reconcile the hub's transcript epoch, resetting cursors when it changed.
 *
 * Kept separate from the message handlers because both `welcome` and
 * `subscribed` carry the epoch, and the answer has to be the same from either.
 */
function reconcileEpoch(
    state: PanelState,
    incoming: TranscriptEpoch | null | undefined,
): { patch: Partial<PanelState>; restarted: boolean } {
    if (typeof incoming !== 'string' || incoming.length === 0) {
        // A hub older than the epoch field. Cursors still work; a restart is
        // then detected from `latest` moving backwards instead.
        return { patch: {}, restarted: false };
    }
    if (state.epoch === null) return { patch: { epoch: incoming }, restarted: false };
    if (state.epoch === incoming) return { patch: {}, restarted: false };

    // A different hub process than the one these cursors were taken from. Its
    // numbering starts again at 1, so a cursor from the old process would
    // return nothing at all — precisely the silence A2 was made of.
    const views = new Map<SessionId, ViewState>();
    for (const [id, view] of state.views) {
        views.set(id, prepend(
            { ...view, epoch: incoming, lastSeq: 0 },
            noteItem('the hub restarted: its transcript numbering began again, so this'
                + ' session was replayed from the start of the new hub process', 'warn'),
        ));
    }
    return { patch: { epoch: incoming, views }, restarted: true };
}

/**
 * Counters for one session.
 *
 * A function of the view rather than a store method, and that is deliberate:
 * a method that built a fresh object on every call is a natural thing to put in
 * a React selector and re-renders forever when you do. Removing it removes the
 * trap instead of documenting it — components take the view with `useView` and
 * memoise against its identity.
 */
export function statsFor(state: PanelState, sessionId: SessionId): ViewStats {
    return statsOf(state.views.get(sessionId));
}

/**
 * Create a panel store.
 *
 * A factory rather than a bare singleton so a test can have its own; the
 * application uses the one exported at the bottom of this file.
 */
export function createPanelStore() {
    return createStore<PanelStore>()((set, get) => ({
        ...INITIAL,

        // ------------------------------------------------------------ reads --
        sessionList() {
            return [...get().sessions.values()].sort((a, b) => {
                const left = a.created_at ?? '';
                const right = b.created_at ?? '';
                if (left === right) return a.session_id.localeCompare(b.session_id);
                return right.localeCompare(left);
            });
        },
        session: (sessionId) => get().sessions.get(sessionId) ?? null,
        view: (sessionId) => viewOf(get(), sessionId),
        items: (sessionId) => get().views.get(sessionId)?.items ?? [],
        lastSeq: (sessionId) => get().views.get(sessionId)?.lastSeq ?? 0,
        logs: (sessionId) => get().views.get(sessionId)?.logs
            ?? { lines: [], dropped: 0, logPath: null },
        latestEvent(sessionId, name) {
            return get().views.get(sessionId)?.latestEvents[name] ?? null;
        },
        latestData(sessionId, name) {
            return get().views.get(sessionId)?.latestEvents[name]?.data ?? null;
        },
        openConfirmations(sessionId) {
            const view = get().views.get(sessionId);
            return view ? [...view.confirmations.values()] : [];
        },
        confirmation: (sessionId, confirmationId) => get().views
            .get(sessionId)?.confirmations.get(confirmationId) ?? null,
        requests(sessionId) {
            const view = get().views.get(sessionId);
            return view ? [...view.requests.values()] : [];
        },
        unknownRequests(sessionId) {
            return get().requests(sessionId).filter((entry) => entry.state === 'unknown');
        },
        isRunActive: (sessionId) => Boolean(get().views.get(sessionId)?.runActive),
        lastRunId(sessionId) {
            const view = get().views.get(sessionId);
            if (view?.lastRunId) return view.lastRunId;
            return get().sessions.get(sessionId)?.last_run_id ?? '';
        },
        loop(sessionId) {
            const data = get().latestData(sessionId, 'status')
                ?? get().latestData(sessionId, 'ready');
            if (typeof data !== 'object' || data === null) return null;
            const loop = (data as Record<string, unknown>).loop;
            return typeof loop === 'object' && loop !== null
                ? loop as Record<string, unknown>
                : null;
        },
        statusData: (sessionId) => get().latestData(sessionId, 'status')
            ?? get().latestData(sessionId, 'ready'),
        model(sessionId) {
            const spec = get().sessions.get(sessionId)?.spec as
                Record<string, unknown> | undefined;
            if (!spec) return '';
            if (typeof spec.model === 'string' && spec.model.length > 0) return spec.model;
            const data = get().latestData(sessionId, 'options') as
                { model?: { current?: { model?: unknown } } } | null;
            const current = data?.model?.current;
            if (current && typeof current.model === 'string') return current.model;
            if (typeof spec.provider === 'string') return spec.provider;
            return '';
        },
        hasCapability: (capability) => get().capabilities.has(capability),

        // ----------------------------------------------------------- writes --
        setHub(metadata) {
            set({ hub: metadata, capabilities: new Set(metadata?.capabilities ?? []) });
        },
        setSelected(sessionId) {
            if (get().selected === sessionId) return;
            set({ selected: sessionId });
        },
        setAuthRequired(required) {
            if (get().authRequired === required) return;
            set({ authRequired: required });
        },
        setConnection(status) {
            const previous = get().connection;
            set({
                connection: {
                    state: status.state,
                    attempt: status.attempt,
                    nextDelayMs: status.nextDelayMs,
                    welcomeReceived: status.welcomeReceived,
                    error: status.error ?? null,
                    // A refusal is a separate event, so a state change does not
                    // clear it: a reconnect would otherwise hide the reason.
                    refusal: previous.refusal,
                    ignoredFrames: previous.ignoredFrames,
                },
            });
        },
        noteRefusal(code, detail) {
            set({ connection: { ...get().connection, refusal: { code, detail } } });
        },
        noteIgnoredFrame() {
            const connection = get().connection;
            set({ connection: { ...connection, ignoredFrames: connection.ignoredFrames + 1 } });
        },
        clearRefusal() {
            const connection = get().connection;
            if (!connection.refusal) return;
            set({ connection: { ...connection, refusal: null } });
        },

        upsertSessions(list) {
            let sessions: Map<SessionId, SessionDescription> | null = null;
            for (const session of list ?? []) {
                if (!session || typeof session.session_id !== 'string') continue;
                sessions ??= new Map(get().sessions);
                sessions.set(session.session_id, session);
            }
            if (sessions) set({ sessions });
        },
        upsertSession(session) {
            if (!session || typeof session.session_id !== 'string') return;
            set({ sessions: upserted(get(), session) });
        },
        applyWelcome(message) {
            const state = get();
            const patch: Partial<PanelState> = {};
            if (message.hub) {
                patch.hub = message.hub;
                patch.capabilities = new Set(message.hub.capabilities ?? []);
            }
            // `welcome` is the one authoritative list: it is sent once, on a
            // fresh connection, and nothing can have raced it. This is
            // therefore the only place a session may disappear for not being
            // listed (defect D23).
            const sessions = new Map<SessionId, SessionDescription>();
            for (const session of message.sessions ?? []) {
                if (!session || typeof session.session_id !== 'string') continue;
                sessions.set(session.session_id, session);
            }
            patch.sessions = sessions;
            if (state.selected && !sessions.has(state.selected)) patch.selected = null;

            const epoch = reconcileEpoch(state, message.hub?.transcript_epoch);
            set({ ...patch, ...epoch.patch });

            // Re-seed every surviving view so request chips and open prompts
            // survive a reconnect. Prompts matter most here: they are the one
            // message whose loss makes a tool call fail, which is why the hub
            // now broadcasts them to every client and not only subscribers.
            for (const id of get().views.keys()) {
                const listed = sessions.get(id);
                if (!listed) continue;
                set(withView(get(), id, (view) => {
                    const confirmations = seedConfirmations(view, listed.confirmations);
                    return confirmations === view.confirmations
                        ? view
                        : { ...view, confirmations };
                }));
                set(seedFromSession(get(), id));
            }
        },
        removeSession(sessionId) {
            const sessions = new Map(get().sessions);
            sessions.delete(sessionId);
            const views = new Map(get().views);
            views.delete(sessionId);
            const patch: Partial<PanelState> = { sessions, views };
            if (get().selected === sessionId) patch.selected = null;
            set(patch);
        },

        applySubscribed(message) {
            const sessionId = message.session?.session_id;
            if (typeof sessionId !== 'string') return NO_EFFECTS;

            // The epoch on the reply itself may differ from the one the cursor
            // was taken under — the hub restarted in between. The delta is then
            // numbered against a series the panel is not holding, so it is
            // dropped rather than spliced in, and the panel asks again from 0.
            const epoch = reconcileEpoch(get(), message.transcript_epoch);
            if (Object.keys(epoch.patch).length > 0) set(epoch.patch);
            if (epoch.restarted) {
                set({ sessions: upserted(get(), message.session) });
                return { resubscribe: { session: sessionId, since: 0 } };
            }

            set({ sessions: upserted(get(), message.session) });

            const latest = typeof message.latest === 'number' ? message.latest : 0;
            const before = viewOf(get(), sessionId);
            if (latest < before.lastSeq) {
                // The hub's counter went backwards. Either it restarted without
                // reporting an epoch, or the session was deleted and recreated:
                // both mean the cursor is meaningless and the delta in hand
                // belongs to a numbering the panel is not holding.
                set(withView(get(), sessionId, (view) => prepend(
                    { ...view, lastSeq: 0 },
                    noteItem('the hub\'s transcript numbering went backwards, so it restarted'
                        + ' (or this session was recreated): the replay started over', 'warn'),
                )));
                set(seedFromSession(get(), sessionId));
                return { resubscribe: { session: sessionId, since: 0 } };
            }

            set(withView(get(), sessionId, (view) => {
                const merged = mergeEnvelopes(view, message.transcript ?? []);
                return { ...merged, lastSeq: Math.max(merged.lastSeq, latest) };
            }));

            if (Array.isArray(message.logs)) {
                set(withView(get(), sessionId, (view) => ({
                    ...view,
                    logs: { ...view.logs, lines: message.logs!.slice(-LOG_CAP) },
                })));
            }
            // Open prompts come from the session description, which is where
            // the hub actually puts them. The old panel read a `confirmations`
            // array off this message — a field `subscribed` has never carried —
            // so after a reload an open approval was invisible until some later
            // frame happened to mention it. Since A1 makes prompts reach every
            // client, the description is now the reliable source.
            set(withView(get(), sessionId, (view) => {
                const confirmations = seedConfirmations(view, message.session.confirmations);
                return confirmations === view.confirmations ? view : { ...view, confirmations };
            }));
            set(seedFromSession(get(), sessionId));
            return NO_EFFECTS;
        },

        applyEvent(message) {
            const envelope = message.envelope;
            const sessionId = message.session ?? envelope?.session_id;
            if (typeof sessionId !== 'string' || !envelope || typeof envelope !== 'object') return;
            const seq = hubSequenceOf(envelope);
            if (seq !== null && get().lastSeq(sessionId) > 0 && seq <= get().lastSeq(sessionId)) {
                set(withView(get(), sessionId, (view) => ({
                    ...view,
                    duplicates: view.duplicates + 1,
                })));
                return;
            }
            set(withView(get(), sessionId, (view) => {
                const advanced = seq !== null ? { ...view, lastSeq: seq } : view;
                return foldEnvelope(advanced, envelope);
            }));
        },

        beginHistory(sessionId) {
            set(withView(get(), sessionId, (view) => ({ ...view, historyLoading: true })));
        },

        endHistory(sessionId) {
            set(withView(get(), sessionId, (view) => ({ ...view, historyLoading: false })));
        },

        applyHistoryPage(sessionId, envelope, page) {
            if (envelope.event !== 'history') return false;
            let accepted = false;
            set(withView(get(), sessionId, (view) => {
                const fresh = page.start === 0 && page.step === 0;
                if (!fresh && view.historyWorker !== envelope.worker_id) return view;
                let history;
                if (fresh) {
                    history = page.turns;
                } else if (page.start === view.history.length && page.step === 0) {
                    history = [...view.history, ...page.turns];
                } else if (page.start === view.history.length - 1
                    && page.step === view.history[page.start]?.steps.length) {
                    const previous = view.history[page.start];
                    if (!previous || page.turns.length === 0) return view;
                    history = [...view.history.slice(0, -1), {
                        ...previous,
                        steps: [...previous.steps, ...page.turns[0]!.steps],
                        omitted_steps: page.turns[0]!.omitted_steps,
                    }, ...page.turns.slice(1)];
                } else {
                    return view;
                }
                accepted = true;
                const done = page.next === page.total && page.next_step === 0;
                return { ...view, history, historyLoading: !done,
                    historyWorker: envelope.worker_id,
                    historySequence: typeof envelope.sequence === 'number'
                        ? envelope.sequence : view.historySequence };
            }));
            return accepted;
        },

        applyRequest(message) {
            const { session, request: entry } = message;
            if (typeof session !== 'string' || !entry
                || typeof entry.request_id !== 'string') return;
            set(withView(get(), session, (view) => {
                const requests = new Map(view.requests);
                requests.set(entry.request_id, entry);
                let next: ViewState = { ...view, requests };
                if (next.requests.size > REQUEST_CAP) {
                    const kept = new Map<string, RequestRecord>();
                    // Settled entries go first: a `sent` request is still in
                    // flight, and is the one a reader is most likely waiting on.
                    for (const [key, value] of next.requests) {
                        if (kept.size >= REQUEST_CAP && value.state !== 'sent') continue;
                        kept.set(key, value);
                    }
                    next = { ...next, requests: kept };
                }
                const existing = next.requestIndex.get(entry.request_id);
                if (existing) {
                    const items = next.items.map((item) => (
                        item === existing ? { ...item, request: entry } : item
                    ));
                    return { ...next, items };
                }
                const item = { kind: 'request', id: nextItemId('req'), request: entry } as const;
                const requestIndex = new Map(next.requestIndex);
                requestIndex.set(entry.request_id, item);
                return append({ ...next, requestIndex }, item);
            }));
        },

        applyConfirmation(message) {
            const { session, confirmation: prompt, open } = message;
            if (typeof session !== 'string' || !prompt?.confirmation_id) return;
            set(withView(get(), session, (view) => {
                const confirmations = new Map(view.confirmations);
                if (open) confirmations.set(prompt.confirmation_id, prompt);
                else confirmations.delete(prompt.confirmation_id);
                return { ...view, confirmations };
            }));
        },

        applyProcess(message) {
            const { session: sessionId, process } = message;
            if (typeof sessionId !== 'string') return;
            const session = get().sessions.get(sessionId);
            if (!session) return;
            set({ sessions: upserted(get(), { ...session, process: process ?? null }) });
        },

        applyConnection(message) {
            const { session: sessionId, connected, identity } = message;
            if (typeof sessionId !== 'string') return;
            const session = get().sessions.get(sessionId);
            if (!session) return;
            set({
                sessions: upserted(get(), {
                    ...session,
                    connected: Boolean(connected),
                    identity: identity
                        ? { ...session.identity, ...identity } as SessionIdentity
                        : session.identity,
                }),
            });
        },

        applyLogs(message) {
            const { session: sessionId, lines, dropped, log_path: logPath } = message;
            if (typeof sessionId !== 'string') return;
            // The hub's `logs` message is a fresh tail of a ring buffer, not an
            // increment: replacing is what keeps a refresh from doubling the
            // pane (defect D20).
            set(withView(get(), sessionId, (view) => ({
                ...view,
                logs: {
                    lines: Array.isArray(lines) ? lines.slice(-LOG_CAP) : view.logs.lines,
                    dropped: typeof dropped === 'number' ? dropped : view.logs.dropped,
                    logPath: typeof logPath === 'string' ? logPath : view.logs.logPath,
                },
            })));
        },

        applySnapshot(message) {
            const sessionId = message.session?.session_id;
            if (typeof sessionId !== 'string') return;
            set({ sessions: upserted(get(), message.session) });
            set(withView(get(), sessionId, (view) => {
                // A snapshot is the whole transcript, so it replaces: keeping
                // the old items would duplicate everything the hub re-sent.
                let next: ViewState = {
                    ...emptyView(sessionId),
                    epoch: get().epoch,
                    confirmations: view.confirmations,
                    logs: view.logs,
                    history: view.history,
                    historyLoading: view.historyLoading,
                    historySequence: view.historySequence,
                    historyWorker: view.historyWorker,
                };
                let maxSeq = 0;
                for (const envelope of message.transcript ?? []) {
                    if (!envelope || typeof envelope !== 'object') continue;
                    const seq = hubSequenceOf(envelope);
                    if (seq !== null && seq > maxSeq) maxSeq = seq;
                    next = foldEnvelope(next, envelope);
                }
                return { ...next, lastSeq: maxSeq };
            }));
            set(seedFromSession(get(), sessionId));
        },

        mergeTranscript(sessionId, transcript, latest) {
            if (typeof sessionId !== 'string') return;
            set(withView(get(), sessionId, (view) => {
                const merged = mergeEnvelopes(view, transcript);
                return { ...merged, lastSeq: Math.max(merged.lastSeq, latest) };
            }));
            set(seedFromSession(get(), sessionId));
        },

        applyError(message) {
            const code = typeof message.error === 'string' ? message.error : 'unknown_error';
            get().setNotice('error', code, message.message || 'the hub refused the message');
            if (code !== 'input_not_sent') return;
            // The hub echoes the message it refused, so the exact request is
            // known rather than guessed at (defect D19: the text used to be lost).
            const request = message.request as { request_id?: unknown } | undefined;
            const requestId = typeof request?.request_id === 'string' ? request.request_id : '';
            if (!requestId) return;
            const sessionId = typeof message.session === 'string' ? message.session : null;
            if (!sessionId) return;
            get().failInput(sessionId, requestId, message.message || 'the input was not sent');
        },

        applyAccepted(message) {
            const result = message.result;
            if (!result || result.ok !== false) return;
            // A refused action answers with a well-formed message rather than an
            // error frame — "a worker is already running" is a normal outcome,
            // not a protocol failure — so it needs reporting from here.
            get().setNotice(
                'warn',
                message.action,
                result.error || `the hub refused ${message.action}`,
            );
        },

        setNotice(tone, code, text) {
            set({ notice: { tone, code, text, at: (get().notice?.at ?? 0) + 1 } });
        },

        dismissNotice() {
            if (get().notice === null) return;
            set({ notice: null });
        },

        setShowDetails(show) {
            if (get().showDetails === show) return;
            set({ showDetails: show });
        },

        toggleDetails() {
            set({ showDetails: !get().showDetails });
        },

        confirmModeOf: (sessionId) => get().confirmMode.get(sessionId) ?? 'ask',

        setConfirmMode(sessionId, mode) {
            const confirmMode = new Map(get().confirmMode);
            // `ask` is the default, so recording it would only make an
            // untouched session indistinguishable from a configured one.
            if (mode === 'ask') confirmMode.delete(sessionId);
            else confirmMode.set(sessionId, mode);
            set({ confirmMode });
        },

        setInspectorOpen(open) {
            if (get().inspectorOpen === open) return;
            set({ inspectorOpen: open });
        },

        setInspectorTab(tab) {
            if (get().inspectorTab === tab) return;
            set({ inspectorTab: tab });
        },

        beginSnapshot(sessionId) {
            set({ snapshot: { sessionId, loading: true, view: null, error: null } });
        },

        finishSnapshot(sessionId, view, error) {
            const current = get().snapshot;
            // A reply for a session the operator has already left is dropped:
            // it describes something no longer on screen.
            if (!current || current.sessionId !== sessionId || current.loading === false) return;
            set({ snapshot: { sessionId, loading: false, view, error } });
        },

        setPaletteOpen(open) {
            if (get().paletteOpen === open) return;
            set({ paletteOpen: open });
        },

        togglePalette() {
            set({ paletteOpen: !get().paletteOpen });
        },

        beginPing(at) {
            set({ pingMs: null, pingSentAt: at });
        },

        finishPing(sentAt, at) {
            // A reply to an earlier heartbeat is stale: reporting it would give
            // a latency that belongs to a request already superseded.
            if (get().pingSentAt !== sentAt) return;
            set({ pingMs: Math.max(0, at - sentAt), pingSentAt: null });
        },

        note(sessionId, text, tone = 'muted') {
            set(withView(get(), sessionId, (view) => append(view, noteItem(text, tone))));
        },

        // ------------------------------------------------------------ input --
        beginInput(sessionId, requestId, parts, operation) {
            const item: OutboxItem = {
                kind: 'outbox',
                id: nextItemId('out'),
                requestId,
                parts,
                operation,
                state: 'pending',
            };
            set(withView(get(), sessionId, (view) => append(view, item)));
        },

        failInput(sessionId, requestId, reason) {
            const pending = get().view(sessionId).items.find(
                (item): item is OutboxItem => (
                    item.kind === 'outbox' && item.requestId === requestId
                ),
            );
            set(withView(get(), sessionId, (view) => dropOutbox(view, requestId)));
            if (!pending) return;
            set({
                failedInput: {
                    sessionId,
                    parts: pending.parts,
                    operation: pending.operation,
                    reason,
                    at: (get().failedInput?.at ?? 0) + 1,
                },
            });
        },

        clearFailedInput() {
            if (get().failedInput === null) return;
            set({ failedInput: null });
        },
    }));
}

/** The store the application uses. */
export const panelStore = createPanelStore();

/** Convenience for module-scope code that does not want to import the hook. */
export const store = panelStore;
