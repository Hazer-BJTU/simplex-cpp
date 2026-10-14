/** Panel REST wire contracts. Transport, validation and URL building belong to consumers. */
import type {
    ApiErrorBody, HubMetadata, SessionSpec, SessionResponse, SessionListResponse,
    RemovedResponse, SupervisorResult, EventsResponse, LogsResponse, SnapshotView,
    SubagentDescription, PanelMessage,
} from './protocol.ts';
import type { AnswerQuery, AnswerPage } from './answers.ts';
import type {
    ConfigKind, ConfigList, ConfigFile, ConfigSelection, ConfigTemplateQuery,
    ConfigTemplateResponse, ConfigPreviewRequest, ConfigPreviewResponse,
    ConfigValidateRequest, ConfigValidateResponse, ConfigSaveRequest,
    ConfigRemoveRequest, ConfigRemoveResponse, ConfigRenameRequest,
} from './configurations.ts';

/** Panel WebSocket path at the Hub origin. Operator authentication and Origin policy still apply. */
export const PANEL_SOCKET_PATH = '/panel/ws';
/** Operator-only worker lifecycle actions; an accepted request is not necessarily a running process. */
export type SupervisorAction = Extract<PanelMessage, { type: 'worker' }>['action'];
/** User-controlled headless tool approval policy; parent workers cannot elevate it. */
export type SubagentPolicy = SubagentDescription['policy'];
/** Retry cleanup, or explicitly attest termination after verifying no owned process remains. */
export type RecoveryAction = 'retry' | 'confirm-terminated';
/** Create an ordinary session. Starting its worker is a separate action. */
export interface CreateSessionRequest {
    /** New ordinary session ID: 1-128 characters from [A-Za-z0-9_-]. */
    session: string;
    /** Operator launch/configuration description, distinct from private launcher state. */
    spec?: SessionSpec;
}
/** Optional launch overrides for start/restart; saved configuration reuse follows Hub policy. */
export interface StartSessionRequest {
    /** Operator launch/configuration description, distinct from private launcher state. */
    spec?: SessionSpec;
}
/** Numeric query values are serialized as URL search parameters by the consumer. */
export interface EventsQuery {
    /** Return retained events after this Hub transcript sequence; omission starts at zero. */
    since?: number;
    /** Requested page/tail bound, subject to server limits and defaults. */
    limit?: number;
}
/** Bounded process-log tail query; omission uses the server default. */
export interface LogsQuery {
    /** Requested page/tail bound, subject to server limits and defaults. */
    limit?: number;
}
/** Change a live headless session policy; pending approvals retain their own decisions. */
export interface SubagentPolicyRequest {
    /** Operator-owned tool approval policy, independent of the parent worker. */
    policy: SubagentPolicy;
}
/** Hub-wide ownership-recovery gate; a blocked Hub needs operator inspection. */
export interface RecoveryStatus {
    /** Whether unresolved ownership prevents automatic recovery/new child starts. */
    blocked: boolean;
    /** Human-readable diagnostic reason; not a stable error code. */
    reason: string | null;
}
/** Lifecycle IDs are opaque correlation tokens, not process IDs or session IDs. */
export interface RecoveryState {
    /** Hub session identifier; it is not a local filesystem path. */
    session_id: string;
    /** Opaque headless lifecycle ID; echo unchanged to avoid acting on a replacement. */
    lifecycle_id: string;
    /** Headless lifecycle label; cleanup-pending still retains ownership. */
    lifecycle?: SubagentDescription['lifecycle'] | undefined;
    /** Whether startup outcome is unknown and needs ownership inspection. */
    uncertain_start: boolean;
    /** Whether owned process termination has been confirmed. */
    termination_confirmed: boolean;
    /** Number of cleanup attempts recorded for this lifecycle. */
    cleanup_attempts: number;
    /** Automatic cleanup retry budget. */
    max_cleanup_attempts: number;
    /** Whether another automatic cleanup attempt is eligible. */
    automatic_retry: boolean;
    /** Scheduled next retry timestamp, or null when none is scheduled. */
    retry_at: string | null;
    /** Human-readable diagnostic reason; not a stable error code. */
    reason?: string | undefined;
}
/** Operator cleanup action tied to one exact headless lifecycle, never just its display ID. */
export interface RecoveryRequest {
    /** Operator cleanup action; never inferred from worker requests. */
    action: RecoveryAction;
    /** Opaque headless lifecycle ID; echo unchanged to avoid acting on a replacement. */
    lifecycle_id: string;
}
/** Supervisor stop outcome. Check ok and how rather than assuming HTTP 200 means terminated. */
export interface StopResponse {
    /** Operation result; inspect it even when the HTTP status is successful. */
    ok: boolean;
    /** Supervisor termination outcome description; check ok as well. */
    how: string;
    /** Whether forced termination was used. */
    forced: boolean;
}
/** Cleanup outcome plus the resulting session description, also returned on operation-level conflict. */
export interface RecoveryResponse extends StopResponse, SessionResponse {}

/**
 * Status-indexed operation replies include both successful and refused results.
 * Common authentication/validation/server errors may occur for every operation;
 * their codes are open strings, not limited to the WebSocket ERROR_CODES list.
 * No runtime validation or request execution is supplied by this table.
 *
 * @typeParam Params - Named path parameters, or never when absent.
 * @typeParam Query - URL query shape before consumer-owned serialization.
 * @typeParam Body - JSON request shape, or never when no body is consumed.
 * @typeParam Responses - HTTP status keys mapped to operation-specific replies.
 */
export interface RestOperation<Params, Query, Body, Responses> {
    /** Path parameters; never means this operation has none. */
    params: Params;
    /** URL search-parameter shape; serialization belongs to the consumer. */
    query: Query;
    /** JSON request body shape; never means no body is consumed. */
    body: Body;
    /** Status-indexed operation results; not an exhaustive HTTP failure vocabulary. */
    responses: Responses;
    /** Common HTTP error envelope; possible even at an operation-specific status. */
    error: ApiErrorBody;
}

/** Complete named REST contract table. Keys are method/path pairs; common errors remain open-ended. */
export interface RestOperations {
    /** Read public metadata without operator authentication. */
    'GET /api/meta': RestOperation<never, never, never, { 200: HubMetadata }>;
    /** List saved configuration names without exposing source text. */
    'GET /api/configurations': RestOperation<never, never, never, { 200: ConfigList }>;
    /** Read a template; omission of source selects default. */
    'GET /api/configurations/:kind/template': RestOperation<{ kind: ConfigKind }, ConfigTemplateQuery, never, { 200: ConfigTemplateResponse }>;
    /** Preview worker endpoint URL strings from operator launcher source. */
    'POST /api/configurations/preview': RestOperation<never, never, ConfigPreviewRequest, { 200: ConfigPreviewResponse }>;
    /** Validate source without saving it or starting a worker. */
    'POST /api/configurations/:kind/validate': RestOperation<{ kind: ConfigKind }, never, ConfigValidateRequest, { 200: ConfigValidateResponse }>;
    /** Read secret-bearing source text and its current opaque revision. */
    'GET /api/configurations/:kind/:id': RestOperation<{ kind: ConfigKind; id: string }, never, never, { 200: ConfigFile }>;
    /** Create with a null revision, or replace a matching revision. */
    'PUT /api/configurations/:kind/:id': RestOperation<{ kind: ConfigKind; id: string }, never, ConfigSaveRequest, { 200: ConfigFile }>;
    /** Delete a matching library revision; existing snapshots remain intact. */
    'DELETE /api/configurations/:kind/:id': RestOperation<{ kind: ConfigKind; id: string }, never, ConfigRemoveRequest, { 200: ConfigRemoveResponse }>;
    /** Rename a matching library revision to a new library ID. */
    'POST /api/configurations/:kind/:id/rename': RestOperation<{ kind: ConfigKind; id: string }, never, ConfigRenameRequest, { 200: ConfigFile }>;
    /** List operator-visible sessions; stopped headless sessions are removed. */
    'GET /api/sessions': RestOperation<never, never, never, { 200: SessionListResponse }>;
    /** Create an ordinary session; a 201 result does not start its worker. */
    'POST /api/sessions': RestOperation<never, never, CreateSessionRequest, { 201: SessionResponse }>;
    /** Read one ordinary or headless session description. */
    'GET /api/sessions/:id': RestOperation<{ id: string }, never, never, { 200: SessionResponse }>;
    /** Remove a stopped, disconnected ordinary session and its persistence. */
    'DELETE /api/sessions/:id': RestOperation<{ id: string }, never, never, { 200: RemovedResponse }>;
    /** Start an ordinary worker; a 409 can contain a supervisor refusal. */
    'POST /api/sessions/:id/start': RestOperation<{ id: string }, never, StartSessionRequest, { 200: SupervisorResult; 409: SupervisorResult }>;
    /** Request ordinary worker shutdown; inspect the stop result. */
    'POST /api/sessions/:id/stop': RestOperation<{ id: string }, never, never, { 200: StopResponse }>;
    /** Restart an ordinary worker; a 409 can contain a supervisor refusal. */
    'POST /api/sessions/:id/restart': RestOperation<{ id: string }, never, StartSessionRequest, { 200: SupervisorResult; 409: SupervisorResult }>;
    /** Operator forced termination; even a 200 result may have ok: false. */
    'POST /api/sessions/:id/force-kill': RestOperation<{ id: string }, never, never, { 200: StopResponse | { ok: false; error: string } }>;
    /** Read retained Hub transcript events, not authoritative worker history. */
    'GET /api/sessions/:id/events': RestOperation<{ id: string }, EventsQuery, never, { 200: EventsResponse }>;
    /** Read a bounded ordinary-worker process-log tail. */
    'GET /api/sessions/:id/logs': RestOperation<{ id: string }, LogsQuery, never, { 200: LogsResponse }>;
    /** Read one exact committed answer page; stale sources fail explicitly. */
    'POST /api/sessions/:id/answer': RestOperation<{ id: string }, never, AnswerQuery, { 200: AnswerPage }>;
    /** Inspect persisted worker artifacts; does not restore or mutate them. */
    'GET /api/sessions/:id/snapshot': RestOperation<{ id: string }, never, never, { 200: SnapshotView }>;
    /** Replace saved configuration snapshots for a stopped, disconnected session. */
    'POST /api/sessions/:id/configurations': RestOperation<{ id: string }, never, ConfigSelection, { 200: SessionResponse }>;
    /** Set operator-owned approval policy for a live headless session. */
    'POST /api/sessions/:id/subagent-policy': RestOperation<{ id: string }, never, SubagentPolicyRequest, { 200: SessionResponse }>;
    /** Inspect the global child-ownership recovery gate. */
    'GET /api/subagents/recovery': RestOperation<never, never, never, { 200: RecoveryStatus }>;
    /** Inspect a headless lifecycle and its cleanup retry budget. */
    'GET /api/sessions/:id/recovery': RestOperation<{ id: string }, never, never, { 200: RecoveryState }>;
    /** Apply an operator cleanup action to the exact lifecycle; 409 may carry a failed result. */
    'POST /api/sessions/:id/recover': RestOperation<{ id: string }, never, RecoveryRequest, { 200: RecoveryResponse; 409: RecoveryResponse }>;
}

/** Static inventory; this is not a router or URL builder. */
export const REST_ROUTES = [
    'GET /api/meta',
    'GET /api/configurations',
    'GET /api/configurations/:kind/template',
    'POST /api/configurations/preview',
    'POST /api/configurations/:kind/validate',
    'GET /api/configurations/:kind/:id',
    'PUT /api/configurations/:kind/:id',
    'DELETE /api/configurations/:kind/:id',
    'POST /api/configurations/:kind/:id/rename',
    'GET /api/sessions',
    'POST /api/sessions',
    'GET /api/sessions/:id',
    'DELETE /api/sessions/:id',
    'POST /api/sessions/:id/start',
    'POST /api/sessions/:id/stop',
    'POST /api/sessions/:id/restart',
    'POST /api/sessions/:id/force-kill',
    'GET /api/sessions/:id/events',
    'GET /api/sessions/:id/logs',
    'POST /api/sessions/:id/answer',
    'GET /api/sessions/:id/snapshot',
    'POST /api/sessions/:id/configurations',
    'POST /api/sessions/:id/subagent-policy',
    'GET /api/subagents/recovery',
    'GET /api/sessions/:id/recovery',
    'POST /api/sessions/:id/recover',
] as const satisfies readonly (keyof RestOperations)[];

/**
 * Response envelope at one known status; common errors may also use that status.
 * @typeParam K - Exact method/path key from RestOperations.
 * @typeParam S - An operation-specific status key; other HTTP failures use ApiErrorBody.
 */
export type RestResponse<K extends keyof RestOperations, S extends keyof RestOperations[K]['responses']> =
    RestOperations[K]['responses'][S] | ApiErrorBody;
