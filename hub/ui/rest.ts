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

export const PANEL_SOCKET_PATH = '/panel/ws';
export type SupervisorAction = Extract<PanelMessage, { type: 'worker' }>['action'];
export type SubagentPolicy = SubagentDescription['policy'];
export type RecoveryAction = 'retry' | 'confirm-terminated';
export interface CreateSessionRequest { session: string; spec?: SessionSpec }
export interface StartSessionRequest { spec?: SessionSpec }
/** Numeric query values are serialized as URL search parameters by the consumer. */
export interface EventsQuery { since?: number; limit?: number }
export interface LogsQuery { limit?: number }
export interface SubagentPolicyRequest { policy: SubagentPolicy }
export interface RecoveryStatus { blocked: boolean; reason: string | null }
/** Lifecycle IDs are opaque correlation tokens, not process IDs or session IDs. */
export interface RecoveryState {
    session_id: string;
    lifecycle_id: string;
    lifecycle?: SubagentDescription['lifecycle'] | undefined;
    uncertain_start: boolean;
    termination_confirmed: boolean;
    cleanup_attempts: number;
    max_cleanup_attempts: number;
    automatic_retry: boolean;
    retry_at: string | null;
    reason?: string | undefined;
}
export interface RecoveryRequest { action: RecoveryAction; lifecycle_id: string }
export interface StopResponse { ok: boolean; how: string; forced: boolean }
export interface RecoveryResponse extends StopResponse, SessionResponse {}

/**
 * Status-indexed operation replies include both successful and refused results.
 * Common authentication/validation/server errors may occur for every operation;
 * their codes are open strings, not limited to the WebSocket ERROR_CODES list.
 * No runtime validation or request execution is supplied by this table.
 */
export interface RestOperation<Params, Query, Body, Responses> {
    params: Params;
    query: Query;
    body: Body;
    responses: Responses;
    error: ApiErrorBody;
}

export interface RestOperations {
    'GET /api/meta': RestOperation<never, never, never, { 200: HubMetadata }>;
    'GET /api/configurations': RestOperation<never, never, never, { 200: ConfigList }>;
    'GET /api/configurations/:kind/template': RestOperation<{ kind: ConfigKind }, ConfigTemplateQuery, never, { 200: ConfigTemplateResponse }>;
    'POST /api/configurations/preview': RestOperation<never, never, ConfigPreviewRequest, { 200: ConfigPreviewResponse }>;
    'POST /api/configurations/:kind/validate': RestOperation<{ kind: ConfigKind }, never, ConfigValidateRequest, { 200: ConfigValidateResponse }>;
    'GET /api/configurations/:kind/:id': RestOperation<{ kind: ConfigKind; id: string }, never, never, { 200: ConfigFile }>;
    'PUT /api/configurations/:kind/:id': RestOperation<{ kind: ConfigKind; id: string }, never, ConfigSaveRequest, { 200: ConfigFile }>;
    'DELETE /api/configurations/:kind/:id': RestOperation<{ kind: ConfigKind; id: string }, never, ConfigRemoveRequest, { 200: ConfigRemoveResponse }>;
    'POST /api/configurations/:kind/:id/rename': RestOperation<{ kind: ConfigKind; id: string }, never, ConfigRenameRequest, { 200: ConfigFile }>;
    'GET /api/sessions': RestOperation<never, never, never, { 200: SessionListResponse }>;
    'POST /api/sessions': RestOperation<never, never, CreateSessionRequest, { 201: SessionResponse }>;
    'GET /api/sessions/:id': RestOperation<{ id: string }, never, never, { 200: SessionResponse }>;
    'DELETE /api/sessions/:id': RestOperation<{ id: string }, never, never, { 200: RemovedResponse }>;
    'POST /api/sessions/:id/start': RestOperation<{ id: string }, never, StartSessionRequest, { 200: SupervisorResult; 409: SupervisorResult }>;
    'POST /api/sessions/:id/stop': RestOperation<{ id: string }, never, never, { 200: StopResponse }>;
    'POST /api/sessions/:id/restart': RestOperation<{ id: string }, never, StartSessionRequest, { 200: SupervisorResult; 409: SupervisorResult }>;
    'POST /api/sessions/:id/force-kill': RestOperation<{ id: string }, never, never, { 200: StopResponse | { ok: false; error: string } }>;
    'GET /api/sessions/:id/events': RestOperation<{ id: string }, EventsQuery, never, { 200: EventsResponse }>;
    'GET /api/sessions/:id/logs': RestOperation<{ id: string }, LogsQuery, never, { 200: LogsResponse }>;
    'POST /api/sessions/:id/answer': RestOperation<{ id: string }, never, AnswerQuery, { 200: AnswerPage }>;
    'GET /api/sessions/:id/snapshot': RestOperation<{ id: string }, never, never, { 200: SnapshotView }>;
    'POST /api/sessions/:id/configurations': RestOperation<{ id: string }, never, ConfigSelection, { 200: SessionResponse }>;
    'POST /api/sessions/:id/subagent-policy': RestOperation<{ id: string }, never, SubagentPolicyRequest, { 200: SessionResponse }>;
    'GET /api/subagents/recovery': RestOperation<never, never, never, { 200: RecoveryStatus }>;
    'GET /api/sessions/:id/recovery': RestOperation<{ id: string }, never, never, { 200: RecoveryState }>;
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

/** Response envelope at one known status; common errors may also use that status. */
export type RestResponse<K extends keyof RestOperations, S extends keyof RestOperations[K]['responses']> =
    RestOperations[K]['responses'][S] | ApiErrorBody;
