/** Compile with ECMAScript types only; every expected error must remain an error. */
import type {
    RestOperations, AnswerQuery, AnswerPage, ConfigSaveRequest, RecoveryRequest,
    ConfigPreviewResponse, SnapshotView, PanelMessage, HubMessage, ApiErrorBody, SessionDescription, SessionSpec, CreateSessionRequest,
} from '../../ui/index.ts';

const source = { worker_id: 'worker', turn: 0, step: 0, commit_sequence: '1', fingerprint: 'opaque' };
const query: AnswerQuery = { source, part: 0, offset: 0 };
const answer: AnswerPage = { ...query, request_id: 'q', raw: 'text', type: 'text', modality: 'text',
    bytes: 4, next_offset: 4, next_part: 1, total_parts: 1, done: true };
const save: ConfigSaveRequest = { text: 'source', revision: null };
const recovery: RecoveryRequest = { action: 'confirm-terminated', lifecycle_id: 'generation' };
const preview: ConfigPreviewResponse = { endpoints: { events: 'ws://host/events', confirm: 'ws://host/confirm', tools: 'ws://host/tools' } };
const snapshot: SnapshotView = { session_id: 's', state: { future: true }, readable: null, files: {} };
const session: SessionDescription = {
    session_id: 's', created_at: '2026-10-14T00:00:00Z', spec: {}, connected: false,
    identity: { state: 'unknown', worker_id: null, since: null },
    stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 0 },
    last_run_id: '', last_event_at: null, last_event: null,
    confirmations: [], process: null, requests: [],
};
const legacy: HubMessage = { type: 'subscribed', session, transcript: [], logs: [], latest: 0 };
const subscribe: PanelMessage = { type: 'subscribe', session: 's', paged: true, request_id: 'r' };
const unknownError: ApiErrorBody = { error: 'future_error', message: 'detail' };
const refused: RestOperations['POST /api/sessions/:id/start']['responses'][409] = { ok: false, error: 'already running' };
const stopped: RestOperations['POST /api/sessions/:id/recover']['responses'][409] = {
    ok: false, how: 'pending', forced: false, session,
};
// @ts-expect-error revision is required even when creating
const badSave: ConfigSaveRequest = { text: 'source' };
// @ts-expect-error fingerprint is required for exact answer correlation
const badQuery: AnswerQuery = { source: { worker_id: 'w', turn: 0, step: 0, commit_sequence: '1' }, part: 0, offset: 0 };
// @ts-expect-error operator recovery must identify the lifecycle
const badRecovery: RecoveryRequest = { action: 'retry' };
// @ts-expect-error endpoints are URL strings
const badPreview: ConfigPreviewResponse = { endpoints: { events: { host: 'host' }, confirm: '', tools: '' } };
// @ts-expect-error session ID is mandatory
const badCreate: RestOperations['POST /api/sessions']['body'] = { spec: {} };
// @ts-expect-error unknown supervisor action
const badAction: PanelMessage = { type: 'worker', session: 's', action: 'delete' };
// @ts-expect-error GET session has no request body
const badBody: RestOperations['GET /api/sessions/:id']['body'] = {};
// @ts-expect-error no Node ambient types
const buffer: Buffer = null;
// @ts-expect-error no DOM ambient types
const socket: WebSocket = null;
void [answer, save, recovery, preview, snapshot, legacy, subscribe, unknownError, refused, stopped];

const jsonSession: CreateSessionRequest = { session: 'json', spec: { persistence: { backend: 'json' } } };
const sqliteSession: SessionSpec = { persistence: { backend: 'sqlite' } };
const sqliteCreate: PanelMessage = { type: 'create_session', session: 'sqlite', spec: sqliteSession };
const jsonCreate: PanelMessage = { type: 'create_session', session: 'json', spec: { persistence: { backend: 'json' } } };
// @ts-expect-error unsupported backend must be rejected by typed REST clients
const invalidBackend: CreateSessionRequest = { session: 'bad', spec: { persistence: { backend: 'mysql' } } };
// @ts-expect-error unsupported backend must be rejected by typed WebSocket clients
const invalidPanelBackend: PanelMessage = { type: 'create_session', session: 'bad', spec: { persistence: { backend: 'mysql' } } };
void [jsonSession, sqliteSession, sqliteCreate, jsonCreate];
