import type { AnswerSource } from './answers.ts';
/**
 * Canonical panel wire vocabulary. Internal compatibility paths re-export it.
 * All consumers share these definitions; no Node, DOM or framework dependencies
 * belong here. Protocol drift tests compare the constants to the wire reference
 * in docs/hub/hub-protocol.md. Optional fields preserve legacy compatibility.
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
 * true for every launcher. A capability list
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
    /** Subscription replay can be sent as paced, byte-bounded pages. */
    'transcript-pages',
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
    /** Worker-backed, display-only conversation history queries. */
    'session-history',
    'answer-pages',
    /** Worker context compaction through an input operation. */
    'context-compact',
    /** Headless delegation routes and operator-owned safety policy. */
    'headless-subagents',
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
    'headless_restricted',
    'invalid_policy',
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
    /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
    type: 'text' | 'binary' | 'external_ref' | string;
    /** Content bytes represented as a string; a display projection may be shortened. */
    raw: string;
    /** True when this display value omits original content. */
    truncated?: boolean;
    /** Original content length in bytes when supplied by the worker. */
    bytes?: number;
    /** Media category, independent of `type`: the worker requires it and never
     * infers one from the encoding. */
    modality: 'text' | 'image' | 'audio' | 'video' | 'document' | string;
    /** Opaque extension data; consumers must validate any fields they use. */
    extras?: unknown;
}

/** Identity of the host execution that committed a response or created input. */
export interface ExecutionIdentity {
    /** Worker incarnation identifier; do not substitute a session or process ID. */
    worker_id: string;
    /** Request correlation token; preserve it unchanged across related replies. */
    request_id: string;
    /** Worker run identifier; distinct from the input request and transcript sequence. */
    run_id: string;
}

/** Bounded display projection, never a restorable worker snapshot. */
export interface HistoryTurn {
    /** Marks an internal continuation that must not be shown as a new user message. */
    internal_input?: 'auto_compact_continue';
    /** Creating input execution, preserved when Continue appends responses. */
    source?: ExecutionIdentity;
    /** Zero-based position within the enclosing history collection. */
    index: number;
    /** Projected input content for this user turn. */
    user: ContentPart[];
    /** Projected exchanges belonging to this turn, possibly incomplete. */
    steps: {
        /** Zero-based position within the enclosing history collection. */
        index: number;
        /** Identity for retrieving exact committed answer content separately. */
        answer_source?: AnswerSource;
        /** Decimal response commit counter; keep string precision unchanged. */
        commit_sequence?: string;
        /** Per-response execution; independent of the turn's input source. */
        execution?: ExecutionIdentity;
        /** Ordered content parts; display projections may omit or shorten them. */
        content: ContentPart[];
        /** Optional reasoning preview; not the final answer. */
        reasoning?: ContentPart;
        /** Number of tool calls in the exchange, not full tool-call records. */
        tool_calls: number;
        /** Number of content parts excluded from the display projection. */
        omitted_parts?: number;
    }[];
    /** Number of exchanges excluded from this turn projection. */
    omitted_steps: number;
    /** Number of input parts excluded from this turn projection. */
    omitted_user_parts?: number;
}

/**
 * Built-in worker data is at most 252 KiB of compact UTF-8 JSON, including
 * escaping and metadata. Worker events fit 256 KiB; raw excludes the data body.
 * Answer sources support independent exact-content pagination. Whole turns/steps preserve the existing cursor;
 * continued turns repeat user parts and append steps without new user entries.
 */
export interface HistoryPage {
    /** Request correlation token; preserve it unchanged across related replies. */
    request_id: string;
    /** Snapshot revision used to detect changes while reading or editing. */
    revision: number;
    /** Starting turn/cursor position requested by the consumer. */
    start: number;
    /** Zero-based exchange cursor within the current turn. */
    step: number;
    /** Next turn cursor supplied by the worker; do not derive it from array length. */
    next: number;
    /** Next exchange cursor within next; preserve both cursor components. */
    next_step: number;
    /** Total turns known to the worker for this history revision. */
    total: number;
    /** Projected turns returned by this page; not restorable worker state. */
    turns: HistoryTurn[];
}

/** Payload options; `confirmation.mode` is the one that carries authority. */
export interface PayloadOptions {
    /** Model selection or provider-owned option data, according to the enclosing contract. */
    model?: Record<string, unknown>;
    /** Tool-owned options or remote-tool endpoint URL, according to the enclosing contract. */
    tools?: Record<string, unknown>;
    /** Tool confirmation configuration for this payload. */
    confirmation?: { mode?: 'ask' | 'approve' | 'deny' | string };
}

/** Initial worker configuration and launch parameters. Once config/config.yaml
 * exists, only threads, env and extraArgs affect subsequent launches. */
export interface SessionSpec {
    /** Saved launcher configuration library name. */
    launchConfig?: string;
    /** Saved worker configuration library name. */
    workerConfig?: string;
    /** Provider profile selected for the driver model. */
    provider?: string;
    /** Model selection or provider-owned option data, according to the enclosing contract. */
    model?: string;
    /** Source profile for a new auxiliary model. On config reuse, this reports
     * the provider key selected by the saved worker config; null means off. */
    modalityAssistProvider?: string | null;
    /** Worker execution thread count. */
    threads?: number;
    /** Maximum exchanges allowed in a normal run segment. */
    maxExchanges?: number;
    /** Token threshold for automatic compaction; zero disables the feature. */
    autoCompactThreshold?: number;
    /** Maximum automatic compactions within one accepted input. */
    maxAutoCompactions?: number;
    /** Configured outbound worker event queue capacity. */
    eventCapacity?: number;
    /** Prompt file relative to the worker's installation directory; the worker
     * refuses absolute paths and parent traversal. */
    systemPromptFile?: string;
    /** Advertised workspace path; not a filesystem security boundary. */
    workspace?: string;
    /** Configured system/platform information injected into the worker prompt. */
    platform?: string;
    /** Configured software descriptions injected into the worker prompt. */
    software?: string[];
    /** Worker persistence settings represented by the session launch description. */
    persistence?: {
        enabled?: boolean;
        /** Authoritative store for a new session; saved worker configuration
         * remains authoritative after its first launch. */
        backend?: 'json' | 'sqlite';
        /** @deprecated Accepted from older clients but ignored; omitted from new session metadata. */
        readable?: boolean;
    };
    /** Worker startup restore policy; future policy names may be added. */
    restore?: 'if_present' | 'never' | string;
    /** Environment overrides for launching the worker; may contain secrets. */
    env?: Record<string, string>;
    /** Additional worker command-line arguments; do not assume they are safe to log. */
    extraArgs?: string[];
}

/** Worker identity as the hub sees it. */
export interface SessionIdentity {
    /** Current lifecycle/state label defined by the enclosing record. */
    state: string;
    /** Worker incarnation identifier; do not substitute a session or process ID. */
    worker_id: string | null;
    /** Replay cursor or observation time as specified by this record's type. */
    since: string | null;
}

/** Per-session counters the hub keeps. */
export interface SessionStats {
    /** Observed event count, or ordered event records in a response. */
    events: number;
    /** Count of detected worker-sequence gaps. */
    gaps: number;
    /** Count of duplicate worker events observed by the Hub. */
    duplicates: number;
    /** Count of worker protocol validation errors. */
    protocolErrors: number;
    /** Number of worker incarnations observed for the session. */
    incarnations: number;
}

/** One call the model proposed, as it appears inside a confirmation prompt. */
export interface PendingCall {
    /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
    type?: string;
    /** Declared tool security category, not a user approval decision. */
    security?: string;
    /** Opaque record identifier in the enclosing contract. */
    id?: string;
    /** Protocol, application or tool name as identified by the enclosing record. */
    name?: string;
    /** Display-only tool argument preview; may contain explicit omission markers. */
    arguments?: unknown;
    /** Opaque extension data; consumers must validate any fields they use. */
    extras?: unknown;
}

/** One tool confirmation prompt. */
export interface ConfirmationPrompt {
    /** Display copy contains labelled truncation/omission values; execution uses the original call. */
    arguments_truncated?: boolean;
    /** Original argument JSON size, including encoding/escaping, before the 64 KiB display budget. */
    arguments_bytes?: number;
    /** Tool approval identifier; correlate with session, worker and run identity. */
    confirmation_id: string;
    /** Hub session identifier; it is not a local filesystem path. */
    session_id: SessionId;
    /** Worker incarnation identifier; do not substitute a session or process ID. */
    worker_id: string;
    /** Worker run identifier; distinct from the input request and transcript sequence. */
    run_id: string;
    /** `awaiting-identity`, `awaiting-decision`, `decided`, or `retired`. */
    state: string;
    /** Whether the Hub verified the prompt against the worker event stream. */
    verified: boolean;
    /** Hub's current worker-identity verification state. */
    identity_state: string;
    /** Display description of the proposed tool call; not executable authority. */
    call: PendingCall;
    /** Hub receipt timestamp in ISO 8601 form. */
    received_at: string;
    /** Approval deadline timestamp, or null when no deadline applies. */
    deadline_at: string | null;
    /** Settlement timestamp, or null while pending. */
    settled_at: string | null;
    /** Recorded decision, or null while no decision has settled. */
    decision: string | null;
    /** Human-readable diagnostic reason; not a stable error code. */
    reason: string | null;
}

/** Outcome attached to a closing confirmation. */
export interface ConfirmationOutcome {
    /** Lifecycle phase associated with the confirmation outcome. */
    phase: string;
    /** Human-readable explanation; consumers must not branch on its exact wording. */
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
    /** Request correlation token; preserve it unchanged across related replies. */
    request_id: string;
    /** Worker payload/signal operation name. */
    operation: string;
    /** Current lifecycle/state label defined by the enclosing record. */
    state: 'sent' | 'admitted' | 'rejected' | 'unknown';
    /** Timestamp when the Hub sent the request to the worker. */
    sent_at: string;
    /** Settlement timestamp, or null while pending. */
    settled_at: string | null;
    /** Human-readable explanation; consumers must not branch on its exact wording. */
    detail: string;
}

/** One supervised worker process. */
export interface ProcessDescription {
    /** Current lifecycle/state label defined by the enclosing record. */
    state: string;
    /** Diagnostic process ID, or null; not proof of ownership or authority to signal it. */
    pid: number | null;
    /** Recorded process start timestamp. */
    started_at: string;
    /** Recorded process exit timestamp, or null while no exit is known. */
    exited_at: string | null;
    /** Observed process exit code, or null if unavailable. */
    exit_code: number | null;
    /** Observed terminating signal name, or null if unavailable. */
    signal: string | null;
    /** Open error code or diagnostic text according to the enclosing response. */
    error: string | null;
    /** Whether the supervisor has recorded stop intent. */
    stop_requested: boolean;
    /** Diagnostic executable name; not a client-side execution instruction. */
    command: string;
    /** Diagnostic command arguments; may contain operator-provided data. */
    args: string[];
    /** Diagnostic worker working directory; do not treat it as a client-local path. */
    cwd: string;
    /** Whether process-group termination was used. */
    process_group_killed: boolean;
    /** Hub-local diagnostic log path, or null; not a client filesystem convention. */
    log_path: string | null;
    /** Number of retained process log lines. */
    log_lines: number;
    /** Number of log lines discarded by retention limits. */
    log_dropped: number;
    /** Decoded UTF-8 bytes omitted from oversized child output lines. */
    log_truncated_bytes?: number;
    /** Records rejected by the optional worker log file's admission policy. */
    file_log_dropped?: number;
    /** The optional worker log sink failed or exceeded its close deadline. */
    file_log_failed?: boolean;
}

/** Minimal operator-visible lifecycle; no raw headless events or conversation. */
export interface SubagentDescription {
    /** Direct cascading parent session ID. */
    parent: string;
    /** Headless lifecycle label; cleanup-pending still retains ownership. */
    lifecycle: 'preparing' | 'starting' | 'ready' | 'stopping' | 'stopped' | 'cleanup-pending';
    /** Operator-owned tool approval policy, independent of the parent worker. */
    policy: 'ask' | 'deny' | 'approve';
    /** Last observed headless health assessment, not proof of process liveness. */
    health: 'healthy' | 'degraded' | 'unknown';
    /** Human-readable diagnostic reason; not a stable error code. */
    reason: string;
    /** Timestamp of the latest observation, or null when unknown. */
    observed_at: string | null;
    /** Whether the last observed worker status reports an active run. */
    active: boolean;
}

/** One session, as the panel sees it. */
export interface SessionDescription {
    /** Ordinary/headless session kind; legacy ordinary sessions may omit it. */
    kind?: 'ordinary' | 'headless';
    /** Headless lifecycle summary; not the child's raw event stream. */
    subagent?: SubagentDescription;
    /** Hub session identifier; it is not a local filesystem path. */
    session_id: SessionId;
    /** Session creation timestamp. */
    created_at: string;
    /** Operator launch/configuration description, distinct from private launcher state. */
    spec: SessionSpec;
    /** Whether the Hub currently has a worker-channel connection. */
    connected: boolean;
    /** Current worker's advertised capabilities; null until its status is known. */
    worker_capabilities?: string[] | null;
    /** Current worker incarnation and identity verification state. */
    identity: SessionIdentity;
    /** Hub observation counters for this session. */
    stats: SessionStats;
    /** Most recently observed worker run ID. */
    last_run_id: string;
    /** Latest worker event receipt time, or null if none. */
    last_event_at: string | null;
    /** Latest observed worker event name, or null if none. */
    last_event: string | null;
    /** Current approval descriptions; reconcile snapshots by composite identity. */
    confirmations: ConfirmationPrompt[];
    /** Supervisor process description, or null when none is tracked. */
    process: ProcessDescription | null;
    /** Retained payload admission records; not the complete transcript. */
    requests: RequestRecord[];
}

/**
 * A worker envelope. History replies are forwarded live but omitted from
 * replay, so a client recovers them with a fresh query.
 *
 * `event` and `data` are deliberately open: core may add an event name at any
 * time, and a hub that rejected an unfamiliar one would disconnect a worker for
 * being newer than it. `known` says whether *this* hub has rendering knowledge
 * about the event, not whether the event is valid.
 */
export interface WorkerEnvelope {
    /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
    type: 'event';
    /** Open worker event name; unknown names do not imply malformed events. */
    event: string;
    /** Hub session identifier; it is not a local filesystem path. */
    session_id: SessionId;
    /** Worker incarnation identifier; do not substitute a session or process ID. */
    worker_id: string;
    /** Request correlation token; preserve it unchanged across related replies. */
    request_id: string;
    /** Worker run identifier; distinct from the input request and transcript sequence. */
    run_id: string;
    /** Null when the worker sent a sequence the hub could not read at all. */
    sequence: number | string | null;
    /** Opaque event body; validate the specific event shape before reading fields. */
    data: unknown;
    /** Added by the hub: position in this hub process's transcript. */
    hub_sequence?: number;
    /** Hub receipt timestamp in ISO 8601 form. */
    received_at?: string;
    /** Whether this Hub recognizes the event vocabulary, not whether it is valid. */
    known?: boolean;
    /** Validation diagnostics attached to the display projection. */
    issues?: string[];
    /** Optional diagnostic connection metadata. */
    connection?: { opened_at: string; protocol_errors: number };
    /** Bounded original envelope metadata; never a duplicate data body. */
    raw?: unknown;
    [field: string]: unknown;
}

/**
 * Identifies one hub process's transcript.
 *
 * `hub_sequence` counts retained transcript envelopes in *this* hub process, so it restarts
 * at 1 after a restart. A cursor captured before one would silently return
 * nothing at all, which looks exactly like an idle session. The epoch turns that
 * silence into a signal: when it changes, a client discards its cursor and asks
 * for the transcript from the beginning.
 */
export type TranscriptEpoch = string;

/** Public decoration descriptor; never contains a local path or access token. */
export interface PanelBackground {
    /** Fixed same-origin authenticated image route. */
    url: '/api/panel/background/image';
    /** Content hash for this Hub's immutable startup asset. */
    revision: string;
    /** Validated CSS size and named position. */
    size: string;
    /** Named alignment within the full viewport. */
    position: string;
    /** Per-theme image opacity in [0, 1]. */
    opacity: { light: number; dark: number };
    /** Per-theme tool card surface opacity in [0, 1]; 0 lets the image through. */
    toolCardOpacity?: { light: number; dark: number };
}

/** Public Hub metadata shared by REST discovery and the panel welcome frame. */
export interface HubMetadata {
    /** Optional global static decoration; absent when disabled or unavailable. */
    background?: PanelBackground;
    /** Protocol, application or tool name as identified by the enclosing record. */
    name: string;
    /** Protocol or application version as identified by the enclosing record. */
    version: string;
    /** Announced panel protocol name and version. */
    protocol: { name: string; version: number };
    /** Worker-side protocol name advertised by this Hub. */
    worker_protocol: string;
    /** Advertised capabilities; gate features on the remote list, not npm versions. */
    capabilities: Capability[];
    /** Absent from a hub older than this field; see `TranscriptEpoch`. */
    transcript_epoch?: TranscriptEpoch | undefined;
    /** Advertised Hub bind address; it may not be the public reverse-proxy address. */
    listen: { host: string; port: number };
    /** Public launcher summary; excludes private launcher inputs. */
    launcher: {
        /** Ordinary/headless session kind; legacy ordinary sessions may omit it. */
        kind: string;
        /** Deprecated in protocol v1; always false. */
        owns_config: boolean;
    };
    /** Names of configured provider profiles, not credentials. */
    provider_profiles: string[];
    /** Whether operator process-group termination is enabled. */
    force_kill_process_group: boolean;
    /** Mock worker availability for this Hub deployment. */
    mock: { enabled: boolean };
}

// --------------------------------------------------------------- JSON API --

/**
 * The REST responses, from `docs/hub/hub-protocol.md` §"JSON API".
 *
 * They live here rather than in the browser bundle because they are the same
 * contract as the socket messages above, just delivered over HTTP: the panel
 * reads them, and a test can compare them to the prose table without a browser.
 * Trusted server response construction uses these canonical DTOs; protocol
 * drift and representative reply tests check the wire behavior.
 */

/** `GET /api/sessions` */
export interface SessionListResponse {
    /** Operator-visible session descriptions; stopped headless children are omitted. */
    sessions: SessionDescription[];
}

/** `POST /api/sessions`, `GET /api/sessions/:id` */
export interface SessionResponse {
    /** Target session ID or description according to the message variant. */
    session: SessionDescription;
}

/** `DELETE /api/sessions/:id` */
export interface RemovedResponse {
    /** Identifier of the removed session or configuration entry. */
    removed: SessionId;
}

/**
 * What the process supervisor reports back through start/stop/restart/force-kill.
 *
 * `ok: false` is a normal answer here, not a transport failure: "a worker is
 * already running" arrives as HTTP 409 with a well-formed body, which is why
 * the panel reads `error` rather than catching.
 */
export interface SupervisorResult {
    /** Operation result; inspect it even when the HTTP status is successful. */
    ok: boolean;
    /** Diagnostic process ID, or null; not proof of ownership or authority to signal it. */
    pid?: number | undefined;
    /** Open error code or diagnostic text according to the enclosing response. */
    error?: string | undefined;
    /** The normalized launch/configuration description; absent when none was read. */
    config?: unknown;
    /** `stop`/`force-kill`: how the process ended. */
    how?: string | undefined;
    /** Whether forced termination was used. */
    forced?: boolean | undefined;
}

/** `GET /api/sessions/:id/events?since=&limit=` */
export interface EventsResponse {
    /** Target session ID or description according to the message variant. */
    session: SessionId;
    /** Replay cursor or observation time as specified by this record's type. */
    since: number;
    /** Highest `hub_sequence` this hub process holds. */
    latest: number;
    /** Observed event count, or ordered event records in a response. */
    events: WorkerEnvelope[];
}

/** `GET /api/sessions/:id/logs?limit=` */
export interface LogsResponse {
    /** Target session ID or description according to the message variant. */
    session: SessionId;
    /** Ordered process log text lines; subject to retention limits. */
    lines: string[];
    /** Number of process log lines omitted from retention. */
    dropped: number;
    /** Hub-local diagnostic log path, or null; not a client filesystem convention. */
    log_path: string | null;
}

/** `GET /api/sessions/:id/snapshot` — the worker's own persisted state, read-only. */
export interface SnapshotView {
    /** Authoritative storage kind; absent for older Hub/worker responses. The
     * state field is decoded JSON even when storage is a SQLite database. */
    backend?: string;
    /** Hub session identifier; it is not a local filesystem path. */
    session_id: SessionId;
    /** Current lifecycle/state label defined by the enclosing record. */
    state: unknown;
    readable: string | null;
    /** Legacy diagnostic Hub-local paths. Managed snapshots return an empty map; target paths are not client filesystem authority. */
    files: Record<string, string>;
    /** Optional explanation when persisted state could not be read. */
    state_error?: string | undefined;
}

/** Common REST error envelope; supervisor refusals can instead carry SupervisorResult. */
export interface ApiErrorBody {
    /** Open error code or diagnostic text according to the enclosing response. */
    error: ErrorCode | string;
    /** Human-readable explanation; not a stable code for branching. */
    message: string;
    /** Opaque optional diagnostics; avoid logging sensitive values. */
    details?: unknown;
}

// --------------------------------------------------- panel -> hub messages --

/** Everything a panel may send. `v` is optional and defaults to 1. */
export type PanelMessage =
    | { v?: number; type: 'ping' }
    | { v?: number; type: 'list_sessions' }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'subscribe';
        /** Target session ID or description according to the message variant. */
        session: SessionId;
        /** Replay cursor or observation time as specified by this record's type. */
        since?: number;
        /** Opt into paced replies when transcript-pages is advertised. */
        paged?: boolean;
        /** Replace the display on the first page rather than merge a delta. */
        replace?: boolean;
        /** Correlates all pages with this subscription attempt. */
        request_id?: string;
    }
    | { v?: number; type: 'unsubscribe'; session: SessionId }
    | { v?: number; type: 'create_session'; session: SessionId; spec?: SessionSpec }
    | { v?: number; type: 'delete_session'; session: SessionId }
    | { v?: number; type: 'subagent_policy'; session: SessionId; policy: 'ask' | 'deny' | 'approve' }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'worker';
        /** Target session ID or description according to the message variant. */
        session: SessionId;
        /** Requested operator action or acknowledged command name. */
        action: 'start' | 'stop' | 'restart' | 'force-kill';
        /** Operator launch/configuration description, distinct from private launcher state. */
        spec?: SessionSpec;
    }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'input';
        /** Target session ID or description according to the message variant. */
        session: SessionId;
        /** Ordered content parts; display projections may omit or shorten them. */
        content?: ContentPart[];
        /** Worker payload/signal operation name. */
        operation?: string;
        /** Request correlation token; preserve it unchanged across related replies. */
        request_id?: string;
        /** Payload-scoped options applied when the worker admits that input. */
        options?: PayloadOptions;
    }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'signal';
        /** Target session ID or description according to the message variant. */
        session: SessionId;
        /** Worker payload/signal operation name. */
        operation: 'status' | 'options' | 'cancel' | 'shutdown';
        /** Worker run identifier; distinct from the input request and transcript sequence. */
        run_id?: string;
    }
    | { v?: number; type: 'history'; session: SessionId; request_id?: string;
        /** Starting turn/cursor position requested by the consumer. */
        start?: number; step?: number; limit?: number }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'confirmation';
        /** Target session ID or description according to the message variant. */
        session: SessionId;
        /** Tool approval identifier; correlate with session, worker and run identity. */
        confirmation_id: string;
        /** Optional correlation for one submission attempt, not tool permission. */
        request_id?: string;
        /** Recorded decision, or null while no decision has settled. */
        decision: 'approved' | 'denied';
        /** Human-readable diagnostic reason; not a stable error code. */
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
    /** Panel protocol version; omission retains legacy v1 behavior. */
    v?: number;
    /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
    type: 'accepted';
    /** Requested operator action or acknowledged command name. */
    action: string;
    /** Target session ID or description according to the message variant. */
    session: SessionId;
    /** Present for input/history and correlated confirmation submissions. */
    request_id?: string;
    /** Present for `signal`. */
    operation?: string;
    /** Present for `confirmation`. */
    confirmation_id?: string;
    /** Present for `worker`: the supervisor's own result, passed through. */
    result?: { ok: boolean; how?: string; forced?: boolean; pid?: number; error?: string };
}

/**
 * What the hub sends when a message was refused.
 *
 * An error that answers a specific command echoes enough of it to say which:
 * `action` names the command, `session` the target, and `result` carries the
 * supervisor's own explanation when there is one.
 */
export interface ErrorMessage {
    /** Panel protocol version; omission retains legacy v1 behavior. */
    v?: number;
    /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
    type: 'error';
    /** Open error code or diagnostic text according to the enclosing response. */
    error: ErrorCode | string;
    /** Human-readable explanation; not a stable code for branching. */
    message: string;
    /** Original request correlation/diagnostic preview, possibly shortened. */
    request?: unknown;
    /** Requested operator action or acknowledged command name. */
    action?: string;
    /** Target session ID or description according to the message variant. */
    session?: SessionId;
    /** Tool approval identifier; correlate with session, worker and run identity. */
    confirmation_id?: string;
    /** Operation-specific result, which may also describe a refusal. */
    result?: unknown;
}

/** Everything the hub may send. */
/** Latest hub-owned Markdown plan; an empty string hides the Plan tab. */
export interface SessionPlan {
    /** Hub-owned plan text; an empty string means no visible plan. */
    markdown: string;
    /** Snapshot revision used to detect changes while reading or editing. */
    revision: number;
    /** Last plan update timestamp, or null before any update. */
    updated_at: string | null;
}

/** All Hub-to-panel wire variants. Validate unknown input before narrowing by type. */
export type HubMessage =
    | { v?: number; type: 'plan'; session: SessionId; plan: SessionPlan }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'welcome';
        /** Hub metadata announced at connection establishment. */
        hub: HubMetadata;
        /** Operator-visible session descriptions; stopped headless children are omitted. */
        sessions: SessionDescription[];
        /** Session IDs currently subscribed on this panel connection. */
        subscriptions: SessionId[];
    }
    | { v?: number; type: 'sessions'; sessions: SessionDescription[] }
    | { v?: number; type: 'session'; session: SessionDescription }
    | { v?: number; type: 'session_removed'; session: SessionId }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'subscribed';
        /** Latest Hub-owned plan snapshot. */
        plan?: SessionPlan;
        /** Target session ID or description according to the message variant. */
        session: SessionDescription;
        /** Ordered retained Hub event projections; not full worker conversation history. */
        transcript: WorkerEnvelope[];
        /** Bounded diagnostic process-log tail. */
        logs: string[];
        /** Highest Hub transcript sequence covered by this reply. */
        latest: number;
        /** True while a paged replay is incomplete; latest covers this page only. */
        replay_more?: boolean;
        /** The first page replaces the displayed transcript instead of merging it. */
        replay_reset?: boolean;
        /** Echoes the subscription request so superseded replies can be ignored. */
        request_id?: string;
        /** Echoed so a client can detect that its cursor predates a restart. */
        transcript_epoch?: TranscriptEpoch | undefined;
    }
    | { v?: number; type: 'created'; session: SessionDescription }
    | { v?: number; type: 'event'; session: SessionId; hub_seq: number; envelope: WorkerEnvelope }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'confirmation';
        /** Target session ID or description according to the message variant. */
        session: SessionId;
        /** True for an active approval prompt; false for a settled/retired prompt. */
        open: boolean;
        /** Tool confirmation configuration for this payload. */
        confirmation: ConfirmationPrompt;
        /** Optional explanation of why an approval closed. */
        outcome?: ConfirmationOutcome;
    }
    | { v?: number; type: 'process'; session: SessionId; process: ProcessDescription | null }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'connection';
        /** Target session ID or description according to the message variant. */
        session: SessionId;
        /** Whether the Hub currently has a worker-channel connection. */
        connected: boolean;
        /** Current worker incarnation and identity verification state. */
        identity: SessionIdentity;
    }
    | { v?: number; type: 'request'; session: SessionId; request: RequestRecord }
    | {
        /** Panel protocol version; omission retains legacy v1 behavior. */
        v?: number;
        /** Wire discriminator or content encoding; narrow the enclosing variant before use. */
        type: 'logs';
        /** Target session ID or description according to the message variant. */
        session: SessionId;
        /** Ordered process log text lines; subject to retention limits. */
        lines: string[];
        /** Number of process log lines omitted from retention. */
        dropped: number;
        /** Hub-local diagnostic log path, or null; not a client filesystem convention. */
        log_path?: string | null;
    }
    | { v?: number; type: 'snapshot'; session: SessionDescription; transcript: WorkerEnvelope[] }
    | AcceptedMessage
    | ErrorMessage
    | { v?: number; type: 'pong'; at: string };

/** The `type` of any message the hub may send. */
export type HubMessageType = HubMessage['type'];

/** Every panel message `type`, in the order `docs/hub/hub-protocol.md` lists them. */
export const PANEL_MESSAGE_TYPES = [
    'ping', 'list_sessions', 'subscribe', 'unsubscribe', 'create_session',
    'delete_session', 'worker', 'input', 'history', 'signal', 'confirmation', 'logs',
    'status_snapshot', 'subagent_policy',
] as const satisfies readonly PanelMessageType[];

/** Every hub message `type`, in the order `docs/hub/hub-protocol.md` lists them. */
export const HUB_MESSAGE_TYPES = [
    'welcome', 'sessions', 'session', 'session_removed', 'subscribed', 'created',
    'event', 'confirmation', 'process', 'connection', 'request', 'logs',
    'snapshot', 'accepted', 'error', 'pong', 'plan',
] as const satisfies readonly HubMessageType[];
