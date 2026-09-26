/**
 * @file hub panel protocol: the vocabulary both ends share.
 *
 * This module is the single place the panel protocol is written down. Before it
 * existed, the version was spelled out three times — in `src/hub.js`, in
 * `src/panel/api.js`, and in the browser's `web/js/api.js` — and nothing
 * compared them, so `protocol.version` in `/api/meta` and the `v` stamped on
 * every frame could drift apart unnoticed.
 *
 * It is TypeScript on purpose. Node runs it directly (type stripping, which the
 * declared floor of 22.18 guarantees) and the panel bundle compiles the same
 * source, so the two ends cannot disagree about a constant or a message shape.
 * Nothing here may import a Node built-in or touch the DOM: the browser is one
 * of its consumers.
 *
 * The shapes below mirror `docs/hub-protocol.md`, which stays the prose
 * reference, and `test/panel-protocol-drift.test.js` fails when the two
 * diverge. A field is optional only where a hub may legitimately omit it, since
 * tolerating that is what makes an additive change safe.
 */

// --------------------------------------------------------------- constants --

/** Protocol name and version announced by `/api/meta` and by `welcome`. */
export const PANEL_PROTOCOL = {
    name: 'simplex-hub-panel',
    version: 1,
} as const;

/**
 * Version stamped on every panel message.
 *
 * Derived rather than repeated, because a second literal is a second thing to
 * forget.
 */
export const PANEL_VERSION: number = PANEL_PROTOCOL.version;

/**
 * Features this hub build offers, advertised in `/api/meta` and `welcome`.
 *
 * The list is what makes an additive change safe: a panel checks for a
 * capability instead of guessing from a version number, so an older hub that
 * does not advertise one simply does not offer that part of the UI.
 *
 * These describe the hub build, not its configuration. Deriving the list from
 * the config was tempting, but nothing in it actually varies that way —
 * `supervisor` means "this hub starts and signals worker processes", which stays
 * true whichever launcher renders the configuration, and the launcher's own
 * difference is already reported as `launcher.owns_config`. A capability list
 * derived from configuration would be an abstraction over an empty set, so the
 * list stays a property of the build and the *consumers* are what changed: the
 * panel reads it now, and did not before.
 */
export const CAPABILITIES = [
    /** Worker envelopes are forwarded to the panel, unknown events included. */
    'worker-events',
    /** Tool confirmations are surfaced and can be answered. */
    'confirmations',
    /** Worker processes can be started, stopped, and force-killed. */
    'supervisor',
    /** A `subscribed` reply replays the transcript after a cursor. */
    'transcript-replay',
    /** The worker's persisted snapshot can be read, never written. */
    'snapshot-view',
    /**
     * `transcript_epoch` is reported, so a cursor taken before a hub restart is
     * recognised as stale instead of silently returning nothing.
     */
    'transcript-epoch',
    /**
     * Confirmations reach every connected panel, not only those subscribed to
     * the session in question, so an approval cannot be stranded behind a
     * session the operator is not looking at.
     */
    'global-confirmations',
] as const;

/** One advertised capability. */
export type Capability = (typeof CAPABILITIES)[number];

/**
 * Error codes a panel can receive.
 *
 * The codes are stable; their `message` is not, which is why a client switches
 * on the code.
 */
export const ERROR_CODES = [
    'bad_json',
    'bad_message',
    'unsupported_version',
    'unknown_session',
    'invalid_session',
    'session_exists',
    'session_busy',
    'input_not_sent',
    'signal_not_sent',
    'unknown_confirmation',
    'confirmation_rejected',
    'worker_action_failed',
    'binary_not_supported',
    'internal_error',
] as const;

/** One stable error code. */
export type ErrorCode = (typeof ERROR_CODES)[number];

// ------------------------------------------------------------------- types --

/** A session id: 1-128 characters of `[A-Za-z0-9_-]`. */
export type SessionId = string;

/** One content part of a payload, as the worker protocol defines it. */
export interface ContentPart {
    type: 'text' | 'binary' | 'external_ref' | string;
    raw: string;
    extras?: unknown;
}

/** Payload options; `confirmation.mode` is the one that carries authority. */
export interface PayloadOptions {
    model?: Record<string, unknown>;
    tools?: Record<string, unknown>;
    confirmation?: { mode?: 'ask' | 'approve' | 'deny' | string };
}

/** Launch specification for one session. */
export interface SessionSpec {
    provider?: string;
    model?: string;
    threads?: number;
    maxExchanges?: number;
    eventCapacity?: number;
    systemPromptFile?: string;
    workspace?: string;
    platform?: string;
    software?: string[];
    persistence?: { enabled?: boolean; readable?: boolean };
    restore?: 'if_present' | 'never' | string;
    env?: Record<string, string>;
    extraArgs?: string[];
}

/** Worker identity as the hub sees it. */
export interface SessionIdentity {
    state: string;
    worker_id: string | null;
    since: string | null;
}

/** Per-session counters the hub keeps. */
export interface SessionStats {
    events: number;
    gaps: number;
    duplicates: number;
    protocolErrors: number;
    incarnations: number;
}

/** One call the model proposed, as it appears inside a confirmation prompt. */
export interface PendingCall {
    type?: string;
    security?: string;
    id?: string;
    name?: string;
    arguments?: unknown;
    extras?: unknown;
}

/** One tool confirmation prompt. */
export interface ConfirmationPrompt {
    confirmation_id: string;
    session_id: SessionId;
    worker_id: string;
    run_id: string;
    /** `awaiting-identity`, `awaiting-decision`, `decided`, or `retired`. */
    state: string;
    verified: boolean;
    identity_state: string;
    call: PendingCall;
    received_at: string;
    deadline_at: string | null;
    settled_at: string | null;
    decision: string | null;
    reason: string | null;
}

/** Outcome attached to a closing confirmation. */
export interface ConfirmationOutcome {
    phase: string;
    detail?: string | null;
}

/**
 * What happened to one payload the hub sent.
 *
 * `unknown` is neither success nor failure: the connection dropped before either
 * was observed, the hub never resends, and a client must not present it as
 * either. That is why the state is an explicit value rather than a boolean.
 */
export interface RequestRecord {
    request_id: string;
    operation: string;
    state: 'sent' | 'admitted' | 'rejected' | 'unknown';
    sent_at: string;
    settled_at: string | null;
    detail: string;
}

/** One supervised worker process. */
export interface ProcessDescription {
    state: string;
    pid: number | null;
    started_at: string;
    exited_at: string | null;
    exit_code: number | null;
    signal: string | null;
    error: string | null;
    stop_requested: boolean;
    command: string;
    args: string[];
    cwd: string;
    process_group_killed: boolean;
    log_path: string | null;
    log_lines: number;
    log_dropped: number;
}

/** One session, as the panel sees it. */
export interface SessionDescription {
    session_id: SessionId;
    created_at: string;
    spec: SessionSpec;
    connected: boolean;
    identity: SessionIdentity;
    stats: SessionStats;
    last_run_id: string;
    last_event_at: string | null;
    last_event: string | null;
    confirmations: ConfirmationPrompt[];
    process: ProcessDescription | null;
    requests: RequestRecord[];
}

/**
 * A worker envelope, forwarded verbatim.
 *
 * `event` and `data` are deliberately open: core may add an event name at any
 * time, and a hub that rejected an unfamiliar one would disconnect a worker for
 * being newer than it. `known` says whether *this* hub has rendering knowledge
 * about the event, not whether the event is valid.
 */
export interface WorkerEnvelope {
    type: 'event';
    event: string;
    session_id: SessionId;
    worker_id: string;
    request_id: string;
    run_id: string;
    sequence: number | string;
    data: unknown;
    /** Added by the hub: position in this hub process's transcript. */
    hub_sequence?: number;
    received_at?: string;
    known?: boolean;
    issues?: string[];
    connection?: { opened_at: string; protocol_errors: number };
    /** The document as received, before normalisation. */
    raw?: unknown;
    [field: string]: unknown;
}

/**
 * Identifies one hub process's transcript.
 *
 * `hub_sequence` counts envelopes received by *this* hub process, so it restarts
 * at 1 after a restart. A cursor captured before one would silently return
 * nothing at all, which looks exactly like an idle session. The epoch turns that
 * silence into a signal: when it changes, a client discards its cursor and asks
 * for the transcript from the beginning.
 */
export type TranscriptEpoch = string;

/** Metadata about the hub, from `/api/meta` and inside `welcome`. */
export interface HubMetadata {
    name: string;
    version: string;
    protocol: { name: string; version: number };
    worker_protocol: string;
    capabilities: Capability[];
    /** Absent from a hub older than this field; see `TranscriptEpoch`. */
    transcript_epoch?: TranscriptEpoch;
    listen: { host: string; port: number };
    launcher: { kind: string; owns_config: boolean };
    provider_profiles: string[];
    force_kill_process_group: boolean;
    mock: { enabled: boolean };
}

// --------------------------------------------------- panel -> hub messages --

/** Everything a panel may send. `v` is optional and defaults to 1. */
export type PanelMessage =
    | { v?: number; type: 'ping' }
    | { v?: number; type: 'list_sessions' }
    | { v?: number; type: 'subscribe'; session: SessionId; since?: number }
    | { v?: number; type: 'unsubscribe'; session: SessionId }
    | { v?: number; type: 'create_session'; session: SessionId; spec?: SessionSpec }
    | { v?: number; type: 'delete_session'; session: SessionId }
    | {
        v?: number;
        type: 'worker';
        session: SessionId;
        action: 'start' | 'stop' | 'restart' | 'force-kill';
        spec?: SessionSpec;
    }
    | {
        v?: number;
        type: 'input';
        session: SessionId;
        content?: ContentPart[];
        operation?: string;
        request_id?: string;
        options?: PayloadOptions;
    }
    | {
        v?: number;
        type: 'signal';
        session: SessionId;
        operation: 'status' | 'options' | 'cancel' | 'shutdown';
        run_id?: string;
    }
    | {
        v?: number;
        type: 'confirmation';
        session: SessionId;
        confirmation_id: string;
        decision: 'approved' | 'denied';
        reason?: string;
    }
    | { v?: number; type: 'logs'; session: SessionId; limit?: number }
    | { v?: number; type: 'status_snapshot'; session: SessionId; since?: number };

/** The `type` of any message a panel may send. */
export type PanelMessageType = PanelMessage['type'];

/** Message types the hub accepts without naming an existing session. */
export const SESSIONLESS_MESSAGE_TYPES = [
    'ping', 'list_sessions', 'create_session', 'subscribe',
] as const satisfies readonly PanelMessageType[];

// --------------------------------------------------- hub -> panel messages --

/** What the hub sends when an action was accepted. */
export interface AcceptedMessage {
    v?: number;
    type: 'accepted';
    action: string;
    session: SessionId;
    /** Present for `input`. */
    request_id?: string;
    /** Present for `signal`. */
    operation?: string;
    /** Present for `worker`: the supervisor's own result. */
    result?: { ok: boolean; how?: string; forced?: boolean; pid?: number; error?: string };
}

/** What the hub sends when a message was refused. */
export interface ErrorMessage {
    v?: number;
    type: 'error';
    error: ErrorCode | string;
    message: string;
    request?: unknown;
}

/** Everything the hub may send. */
export type HubMessage =
    | {
        v?: number;
        type: 'welcome';
        hub: HubMetadata;
        sessions: SessionDescription[];
        subscriptions: SessionId[];
    }
    | { v?: number; type: 'sessions'; sessions: SessionDescription[] }
    | { v?: number; type: 'session'; session: SessionDescription }
    | { v?: number; type: 'session_removed'; session: SessionId }
    | {
        v?: number;
        type: 'subscribed';
        session: SessionDescription;
        transcript: WorkerEnvelope[];
        logs: string[];
        latest: number;
        /** Echoed so a client can detect that its cursor predates a restart. */
        transcript_epoch?: TranscriptEpoch;
    }
    | { v?: number; type: 'created'; session: SessionDescription }
    | { v?: number; type: 'event'; session: SessionId; hub_seq: number; envelope: WorkerEnvelope }
    | {
        v?: number;
        type: 'confirmation';
        session: SessionId;
        open: boolean;
        confirmation: ConfirmationPrompt;
        outcome?: ConfirmationOutcome;
    }
    | { v?: number; type: 'process'; session: SessionId; process: ProcessDescription | null }
    | {
        v?: number;
        type: 'connection';
        session: SessionId;
        connected: boolean;
        identity: SessionIdentity;
    }
    | { v?: number; type: 'request'; session: SessionId; request: RequestRecord }
    | {
        v?: number;
        type: 'logs';
        session: SessionId;
        lines: string[];
        dropped: number;
        log_path?: string | null;
    }
    | { v?: number; type: 'snapshot'; session: SessionDescription; transcript: WorkerEnvelope[] }
    | AcceptedMessage
    | ErrorMessage
    | { v?: number; type: 'pong'; at: string };

/** The `type` of any message the hub may send. */
export type HubMessageType = HubMessage['type'];

/** Every panel message `type`, in the order `docs/hub-protocol.md` lists them. */
export const PANEL_MESSAGE_TYPES = [
    'ping', 'list_sessions', 'subscribe', 'unsubscribe', 'create_session',
    'delete_session', 'worker', 'input', 'signal', 'confirmation', 'logs',
    'status_snapshot',
] as const satisfies readonly PanelMessageType[];

/** Every hub message `type`, in the order `docs/hub-protocol.md` lists them. */
export const HUB_MESSAGE_TYPES = [
    'welcome', 'sessions', 'session', 'session_removed', 'subscribed', 'created',
    'event', 'confirmation', 'process', 'connection', 'request', 'logs',
    'snapshot', 'accepted', 'error', 'pong',
] as const satisfies readonly HubMessageType[];
